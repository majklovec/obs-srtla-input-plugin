#include "srtla-receiver.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include "connection/connection_registry.h"
#include "plugin-log.hpp"
#include "protocol/srt_handler.h"
#include "protocol/srtla_handler.h"
#include "quality/load_balancer.h"
#include "quality/metrics_collector.h"
#include "quality/quality_evaluator.h"
#include "utils/auth_rate_limiter.h"
#include "utils/network_utils.h"

extern "C" {
#include "common.h"
}

namespace srtla_embed {
namespace {

constexpr int max_epoll_events = 16;

// Upstream keeps its metrics counters in plain process-wide globals because
// srtla_rec is single-threaded. Several OBS sources may run receivers at the
// same time, so packet processing is serialized across all of them.
std::mutex upstream_mutex;

// blog() is thread-safe, so the sink needs no lock of its own.
class ObsLogSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
   protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        int level = LOG_DEBUG;
        switch (msg.level) {
            case spdlog::level::critical:
            case spdlog::level::err:
                level = LOG_ERROR;
                break;
            case spdlog::level::warn:
                level = LOG_WARNING;
                break;
            case spdlog::level::info:
                level = LOG_INFO;
                break;
            default:
                break;
        }
        plugin_log(level, "srtla: %.*s", static_cast<int>(msg.payload.size()),
                   msg.payload.data());
    }

    void flush_() override {}
};

void install_log_sink() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto logger = std::make_shared<spdlog::logger>("srtla",
                                                       std::make_shared<ObsLogSink>());
        logger->set_level(spdlog::level::info);
        spdlog::set_default_logger(logger);
    });
}

void close_fd(int& fd) {
    if (fd >= 0)
        close(fd);
    fd = -1;
}

void set_socket_buffers(int fd) {
    int size = RECV_BUF_SIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0)
        plugin_log(LOG_WARNING, "could not set SRTLA receive buffer: %s",
                   strerror(errno));
    size = SEND_BUF_SIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0)
        plugin_log(LOG_WARNING, "could not set SRTLA send buffer: %s",
                   strerror(errno));
}

}  // namespace

struct EmbeddedSrtlaReceiver::Impl {
    int epoll_fd = -1;
    int srtla_sock = -1;
    int wake_fd = -1;
    sockaddr_storage srt_addr{};
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    std::thread thread;

    void close_all() {
        close_fd(srtla_sock);
        close_fd(wake_fd);
        close_fd(epoll_fd);
    }

    void loop() {
        try {
            run_upstream_loop();
        } catch (const std::exception& error) {
            plugin_log(LOG_ERROR, "SRTLA receiver failed: %s",
                       error.what());
        }
        running.store(false);
    }

    void run_upstream_loop() {
        // Declared before the upstream objects so it is released after they
        // are destroyed; their destructors also touch shared upstream state.
        std::unique_lock<std::mutex> lock(upstream_mutex, std::defer_lock);

        srtla::connection::ConnectionRegistry registry;
        srtla::quality::MetricsCollector metrics_collector;
        srtla::utils::AuthRateLimiter rate_limiter;
        srtla::protocol::SRTHandler srt_handler(srtla_sock, srt_addr, epoll_fd, registry,
                                                rate_limiter);
        srtla::protocol::SRTLAHandler srtla_handler(srtla_sock, registry, srt_handler,
                                                    metrics_collector, rate_limiter);
        srtla::quality::QualityEvaluator quality_evaluator;
        srtla::quality::LoadBalancer load_balancer;

        const auto keepalive = [&srtla_handler](const srtla::connection::ConnectionPtr& conn,
                                                time_t ts) {
            srtla_handler.send_keepalive(conn, ts);
        };

        while (!stopping.load()) {
            epoll_event events[max_epoll_events];
            const int count = epoll_wait(epoll_fd, events, max_epoll_events, 1000);
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                plugin_log(LOG_ERROR, "SRTLA epoll_wait failed: %s",
                           strerror(errno));
                break;
            }
            if (stopping.load())
                break;

            time_t ts = 0;
            if (get_seconds(&ts) != 0)
                continue;

            lock.lock();
            for (int i = 0; i < count; i++) {
                void* tag = events[i].data.ptr;
                if (tag == this)
                    continue;

                // Same guard as upstream: handlers may remove groups,
                // invalidating group pointers in the remaining events.
                const std::size_t group_count = registry.groups().size();
                if (tag == nullptr) {
                    srtla_handler.process_packets(ts);
                } else {
                    auto* raw_group =
                        static_cast<srtla::connection::ConnectionGroup*>(tag);
                    auto group = registry.find_group_by_id(raw_group->id().data());
                    if (group)
                        srt_handler.handle_srt_data(group);
                }
                if (registry.groups().size() < group_count)
                    break;
            }

            registry.cleanup_inactive(ts, keepalive);
            rate_limiter.cleanup(ts);
            for (auto& group : registry.groups()) {
                quality_evaluator.evaluate_group(group, ts);
                load_balancer.adjust_weights(group, ts);
            }
            lock.unlock();
        }

        lock.lock();
    }
};

EmbeddedSrtlaReceiver::EmbeddedSrtlaReceiver() : impl_(std::make_unique<Impl>()) {}

EmbeddedSrtlaReceiver::~EmbeddedSrtlaReceiver() {
    stop();
}

bool EmbeddedSrtlaReceiver::start(uint16_t srtla_port, uint16_t srt_port) {
    Impl& impl = *impl_;
    if (impl.thread.joinable())
        return false;

    install_log_sink();
    impl.stopping.store(false);

    const std::string srt_port_text = std::to_string(srt_port);
    if (srtla::utils::NetworkUtils::resolve_srt_address("127.0.0.1", srt_port_text.c_str(),
                                                        &impl.srt_addr, RECV_BUF_SIZE,
                                                        SEND_BUF_SIZE) < 0) {
        plugin_log(LOG_ERROR, "could not resolve local SRT listener");
        return false;
    }

    impl.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    impl.srtla_sock = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    impl.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (impl.epoll_fd < 0 || impl.srtla_sock < 0 || impl.wake_fd < 0) {
        plugin_log(LOG_ERROR, "could not create SRTLA sockets: %s",
                   strerror(errno));
        impl.close_all();
        return false;
    }

    int v6only = 0;
    if (setsockopt(impl.srtla_sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0)
        plugin_log(LOG_WARNING, "SRTLA socket is IPv6-only: %s",
                   strerror(errno));
    set_socket_buffers(impl.srtla_sock);

    sockaddr_in6 listen_addr{};
    listen_addr.sin6_family = AF_INET6;
    listen_addr.sin6_addr = in6addr_any;
    listen_addr.sin6_port = htons(srtla_port);
    if (bind(impl.srtla_sock, reinterpret_cast<const sockaddr*>(&listen_addr),
             sizeof(listen_addr)) != 0) {
        plugin_log(LOG_ERROR, "could not bind SRTLA UDP port %u: %s",
                   srtla_port, strerror(errno));
        impl.close_all();
        return false;
    }

    if (srtla::utils::NetworkUtils::epoll_add(impl.epoll_fd, impl.srtla_sock, EPOLLIN,
                                              nullptr) != 0 ||
        srtla::utils::NetworkUtils::epoll_add(impl.epoll_fd, impl.wake_fd, EPOLLIN,
                                              &impl) != 0) {
        plugin_log(LOG_ERROR, "could not register SRTLA sockets: %s",
                   strerror(errno));
        impl.close_all();
        return false;
    }

    impl.running.store(true);
    try {
        impl.thread = std::thread(&Impl::loop, &impl);
    } catch (const std::system_error& error) {
        plugin_log(LOG_ERROR, "could not start SRTLA thread: %s",
                   error.what());
        impl.running.store(false);
        impl.close_all();
        return false;
    }
    return true;
}

void EmbeddedSrtlaReceiver::stop() {
    Impl& impl = *impl_;
    impl.stopping.store(true);
    if (impl.wake_fd >= 0) {
        const uint64_t one = 1;
        if (write(impl.wake_fd, &one, sizeof(one)) < 0 && errno != EAGAIN)
            plugin_log(LOG_DEBUG, "SRTLA wakeup failed: %s",
                       strerror(errno));
    }
    if (impl.thread.joinable())
        impl.thread.join();
    impl.close_all();
    impl.running.store(false);
}

bool EmbeddedSrtlaReceiver::running() const {
    return impl_->running.load();
}

}  // namespace srtla_embed
