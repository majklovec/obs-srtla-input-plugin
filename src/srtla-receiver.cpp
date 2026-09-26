#include "srtla-receiver.hpp"

#include <obs-module.h>

#include <srtla_platform.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include "connection/connection_registry.h"
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

class ObsLogSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
	void sink_it_(const spdlog::details::log_msg &msg) override
	{
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
		const std::string text(msg.payload.data(), msg.payload.size());
		blog(level, "[obs-srtla-source] srtla: %s", text.c_str());
	}

	void flush_() override {}
};

void install_log_sink()
{
	static std::once_flag once;
	std::call_once(once, [] {
		auto logger = std::make_shared<spdlog::logger>("srtla",
							       std::make_shared<ObsLogSink>());
		logger->set_level(spdlog::level::info);
		spdlog::set_default_logger(logger);
	});
}

void close_socket(int &fd)
{
	if (fd >= 0)
		srtla_close_socket(fd);
	fd = -1;
}

void close_epoll(int &fd)
{
	if (fd >= 0)
		srtla_epoll_close(fd);
	fd = -1;
}

void set_socket_buffers(int fd)
{
	int size = RECV_BUF_SIZE;
	if (srtla_setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0)
		blog(LOG_WARNING, "[obs-srtla-source] could not set SRTLA receive buffer: %s",
		     srtla_socket_strerror());
	size = SEND_BUF_SIZE;
	if (srtla_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0)
		blog(LOG_WARNING, "[obs-srtla-source] could not set SRTLA send buffer: %s",
		     srtla_socket_strerror());
}

// A UDP socket bound to loopback and connected to itself: a byte sent on it is
// received on it, which wakes epoll_wait() on every platform (no eventfd).
int create_wake_socket()
{
	const int fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
	if (fd < 0)
		return -1;

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	socklen_t addr_len = sizeof(addr);
	if (srtla_bind(fd, reinterpret_cast<const sockaddr *>(&addr), addr_len) != 0 ||
	    srtla_getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0 ||
	    srtla_connect(fd, reinterpret_cast<const sockaddr *>(&addr), addr_len) != 0 ||
	    srtla_configure_udp_socket(fd) != 0) {
		srtla_close_socket(fd);
		return -1;
	}
	return fd;
}

} // namespace

struct EmbeddedSrtlaReceiver::Impl {
	int epoll_fd = -1;
	int srtla_sock = -1;
	int wake_fd = -1;
	bool sockets_initialized = false;
	sockaddr_storage srt_addr{};
	std::atomic<bool> stopping{false};
	std::atomic<bool> running{false};
	std::thread thread;

	void close_all()
	{
		close_socket(srtla_sock);
		close_socket(wake_fd);
		close_epoll(epoll_fd);
		if (sockets_initialized)
			srtla_sockets_cleanup();
		sockets_initialized = false;
	}

	void loop()
	{
		try {
			run_upstream_loop();
		} catch (const std::exception &error) {
			blog(LOG_ERROR, "[obs-srtla-source] SRTLA receiver failed: %s",
			     error.what());
		}
		running.store(false);
	}

	void run_upstream_loop()
	{
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

		const auto keepalive = [&srtla_handler](const srtla::connection::ConnectionPtr &conn,
							time_t ts) {
			srtla_handler.send_keepalive(conn, ts);
		};

		while (!stopping.load()) {
			epoll_event events[max_epoll_events];
			const int count = srtla_epoll_wait(epoll_fd, events, max_epoll_events, 1000);
			if (count < 0) {
				if (srtla_socket_interrupted())
					continue;
				blog(LOG_ERROR, "[obs-srtla-source] SRTLA epoll_wait failed: %s",
				     srtla_socket_strerror());
				break;
			}
			if (stopping.load())
				break;

			time_t ts = 0;
			if (get_seconds(&ts) != 0)
				continue;

			lock.lock();
			for (int i = 0; i < count; i++) {
				void *tag = events[i].data.ptr;
				if (tag == this) {
					char drain[8];
					srtla_recv(wake_fd, drain, sizeof(drain), 0);
					continue;
				}

				// Same guard as upstream: handlers may remove groups,
				// invalidating group pointers in the remaining events.
				const std::size_t group_count = registry.groups().size();
				if (tag == nullptr) {
					srtla_handler.process_packets(ts);
				} else {
					auto *raw_group =
						static_cast<srtla::connection::ConnectionGroup *>(tag);
					auto group = registry.find_group_by_id(raw_group->id().data());
					if (group)
						srt_handler.handle_srt_data(group);
				}
				if (registry.groups().size() < group_count)
					break;
			}

			registry.cleanup_inactive(ts, keepalive);
			rate_limiter.cleanup(ts);
			for (auto &group : registry.groups()) {
				quality_evaluator.evaluate_group(group, ts);
				load_balancer.adjust_weights(group, ts);
			}
			lock.unlock();
		}

		lock.lock();
	}
};

EmbeddedSrtlaReceiver::EmbeddedSrtlaReceiver() : impl_(std::make_unique<Impl>()) {}

EmbeddedSrtlaReceiver::~EmbeddedSrtlaReceiver()
{
	stop();
}

bool EmbeddedSrtlaReceiver::start(uint16_t srtla_port, uint16_t srt_port)
{
	Impl &impl = *impl_;
	if (impl.thread.joinable())
		return false;

	install_log_sink();
	impl.stopping.store(false);

	if (srtla_sockets_init() != 0) {
		blog(LOG_ERROR, "[obs-srtla-source] could not initialize sockets");
		return false;
	}
	impl.sockets_initialized = true;

	const std::string srt_port_text = std::to_string(srt_port);
	if (srtla::utils::NetworkUtils::resolve_srt_address("127.0.0.1", srt_port_text.c_str(),
							    &impl.srt_addr, RECV_BUF_SIZE,
							    SEND_BUF_SIZE) < 0) {
		blog(LOG_ERROR, "[obs-srtla-source] could not resolve local SRT listener");
		impl.close_all();
		return false;
	}

	impl.epoll_fd = srtla_epoll_create();
	impl.srtla_sock = static_cast<int>(
		socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
	impl.wake_fd = create_wake_socket();
	if (impl.epoll_fd < 0 || impl.srtla_sock < 0 || impl.wake_fd < 0) {
		blog(LOG_ERROR, "[obs-srtla-source] could not create SRTLA sockets: %s",
		     srtla_socket_strerror());
		impl.close_all();
		return false;
	}

	if (srtla_configure_udp_socket(impl.srtla_sock) != 0) {
		blog(LOG_ERROR, "[obs-srtla-source] could not configure SRTLA socket: %s",
		     srtla_socket_strerror());
		impl.close_all();
		return false;
	}

	int v6only = 0;
	if (srtla_setsockopt(impl.srtla_sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
			     sizeof(v6only)) != 0)
		blog(LOG_WARNING, "[obs-srtla-source] SRTLA socket is IPv6-only: %s",
		     srtla_socket_strerror());
	set_socket_buffers(impl.srtla_sock);

	sockaddr_in6 listen_addr{};
	listen_addr.sin6_family = AF_INET6;
	listen_addr.sin6_addr = in6addr_any;
	listen_addr.sin6_port = htons(srtla_port);
	if (srtla_bind(impl.srtla_sock, reinterpret_cast<const sockaddr *>(&listen_addr),
		       sizeof(listen_addr)) != 0) {
		blog(LOG_ERROR, "[obs-srtla-source] could not bind SRTLA UDP port %u: %s",
		     srtla_port, srtla_socket_strerror());
		impl.close_all();
		return false;
	}

	if (srtla::utils::NetworkUtils::epoll_add(impl.epoll_fd, impl.srtla_sock, EPOLLIN,
						  nullptr) != 0 ||
	    srtla::utils::NetworkUtils::epoll_add(impl.epoll_fd, impl.wake_fd, EPOLLIN,
						  &impl) != 0) {
		blog(LOG_ERROR, "[obs-srtla-source] could not register SRTLA sockets: %s",
		     srtla_socket_strerror());
		impl.close_all();
		return false;
	}

	impl.running.store(true);
	try {
		impl.thread = std::thread(&Impl::loop, &impl);
	} catch (const std::system_error &error) {
		blog(LOG_ERROR, "[obs-srtla-source] could not start SRTLA thread: %s",
		     error.what());
		impl.running.store(false);
		impl.close_all();
		return false;
	}
	return true;
}

void EmbeddedSrtlaReceiver::stop()
{
	Impl &impl = *impl_;
	impl.stopping.store(true);
	if (impl.wake_fd >= 0) {
		const char one = 1;
		if (srtla_send(impl.wake_fd, &one, sizeof(one), 0) < 0 &&
		    !srtla_socket_would_block())
			blog(LOG_DEBUG, "[obs-srtla-source] SRTLA wakeup failed: %s",
			     srtla_socket_strerror());
	}
	if (impl.thread.joinable())
		impl.thread.join();
	impl.close_all();
	impl.running.store(false);
}

bool EmbeddedSrtlaReceiver::running() const
{
	return impl_->running.load();
}

} // namespace srtla_embed
