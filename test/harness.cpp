// Headless OBS host for end-to-end tests.
//
// Loads the plugin module, creates SRTLA sources through its obs_source_info,
// and replaces the libobs frame-output functions so every video/audio frame
// the plugin produces is recorded. After the run it prints one
// "RESULT key=value ..." line that test/e2e.sh checks.
//
// usage: harness PLUGIN.so --port PORT --seconds N
//                [--initial-port PORT] [--extra-source PORT]
//                [--passphrase TEXT] [--pbkeylen N] [--streamid TEXT]
//                [--websocket 0]
//   --initial-port  create the source on this port, then switch it to --port
//                   through update() (tests port changes)
//   --extra-source  run a second, idle source on this port at the same time
//   --passphrase, --pbkeylen, --streamid  SRT security settings of the source

#include <dlfcn.h>
#include <obs-module.h>
#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace {

obs_source_info plugin_source{};
proc_handler_t* global_handler = nullptr;
proc_handler_t* websocket_handler = nullptr;
proc_handler_t* source_handlers[2]{};
bool source_alive[2]{};
struct WebsocketCallback {
    void (*callback)(obs_data_t*, obs_data_t*, void*);
    void* private_data;
} websocket_callback{};
unsigned websocket_failures = 0;
unsigned connected_queries = 0;
bool websocket_enabled = true;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "WebSocket check failed: %s\n", message);
        ++websocket_failures;
    }
}

obs_data_t* query_stats(const char* name) {
    obs_data_t* request = obs_data_create();
    obs_data_t* response = obs_data_create();
    obs_data_set_string(request, "sourceName", name);
    websocket_callback.callback(request, response, websocket_callback.private_data);
    obs_data_release(request);
    return response;
}

void check_error(const char* name) {
    obs_data_t* response = query_stats(name);
    require(!obs_data_get_bool(response, "success") && *obs_data_get_string(response, "error"),
            "invalid request must return an explicit error");
    obs_data_release(response);
}

void poll_stats() {
    obs_data_t* response = query_stats("SRTLA Test");
    require(obs_data_get_bool(response, "success"), "source query must succeed");
    require(std::strcmp(obs_data_get_string(response, "sourceName"), "SRTLA Test") == 0,
            "source name must be returned");
    obs_data_t* values = obs_data_get_obj(response, "stats");
    if (obs_data_get_bool(response, "connected")) {
        require(values && obs_data_has_user_value(values, "pktRecvTotal") &&
                    obs_data_has_user_value(values, "byteRecvTotal") &&
                    obs_data_has_user_value(values, "mbpsRecvRate") &&
                    obs_data_has_user_value(values, "msRTT") &&
                    obs_data_has_user_value(values, "pktRcvLossTotal") &&
                    obs_data_has_user_value(values, "pktRcvRetrans") &&
                    obs_data_has_user_value(values, "msRcvBuf"),
                "connected source must return the SRT stats schema");
        if (values) {
            if (obs_data_get_int(values, "pktRecvTotal") > 0 && obs_data_get_int(values, "byteRecvTotal") > 0)
                ++connected_queries;
            obs_data_t* second = query_stats("SRTLA Test");
            obs_data_t* second_values = obs_data_get_obj(second, "stats");
            if (second_values && obs_data_get_int(second_values, "msTimeStamp") >=
                                     obs_data_get_int(values, "msTimeStamp"))
                require(obs_data_get_int(second_values, "pktRecvTotal") >=
                            obs_data_get_int(values, "pktRecvTotal"),
                        "queries must not reset receive counters");
            obs_data_release(second_values);
            obs_data_release(second);
        }
    } else {
        require(!values, "disconnected source must not expose stale stats");
    }
    obs_data_release(values);
    require(!obs_data_has_user_value(response, "srt_passphrase") &&
                !obs_data_has_user_value(response, "srt_streamid"),
            "response must not expose source secrets");
    obs_data_release(response);
    if (source_alive[1]) {
        response = query_stats("Idle Test");
        require(obs_data_get_bool(response, "success") && !obs_data_get_bool(response, "connected"),
                "idle source stats must be isolated from the active source");
        obs_data_release(response);
    }
}

struct Stats {
    std::mutex mutex;
    unsigned long long video_frames = 0;
    unsigned long long audio_packets = 0;
    unsigned long long audio_samples = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int trc = -1;
    bool color_matrix_set = false;
    uint64_t first_video_ts = 0;
    uint64_t last_video_ts = 0;
    std::set<std::string> video_formats;
    std::set<std::string> audio_formats;
    std::set<int> speakers;
} stats;

const char* audio_format_name(audio_format format) {
    switch (format) {
        case AUDIO_FORMAT_U8BIT:
            return "u8";
        case AUDIO_FORMAT_16BIT:
            return "s16";
        case AUDIO_FORMAT_32BIT:
            return "s32";
        case AUDIO_FORMAT_FLOAT:
            return "float";
        case AUDIO_FORMAT_U8BIT_PLANAR:
            return "u8p";
        case AUDIO_FORMAT_16BIT_PLANAR:
            return "s16p";
        case AUDIO_FORMAT_32BIT_PLANAR:
            return "s32p";
        case AUDIO_FORMAT_FLOAT_PLANAR:
            return "floatp";
        default:
            return "unknown";
    }
}

template <typename Set>
std::string join(const Set& values) {
    std::string text;
    for (const auto& value : values) {
        if (!text.empty())
            text += ',';
        if constexpr (std::is_same_v<typename Set::value_type, std::string>)
            text += value;
        else
            text += std::to_string(value);
    }
    return text.empty() ? "none" : text;
}

double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

// Overrides of libobs functions; the executable is linked with -rdynamic so
// the plugin binds to these instead of the libobs implementations.
extern "C" {

proc_handler_t* obs_get_proc_handler(void) {
    return global_handler;
}

proc_handler_t* obs_source_get_proc_handler(const obs_source_t* source) {
    return source_handlers[source == reinterpret_cast<const obs_source_t*>(0x1) ? 0 : 1];
}

obs_source_t* obs_get_source_by_name(const char* name) {
    if (source_alive[0] && std::strcmp(name, "SRTLA Test") == 0)
        return reinterpret_cast<obs_source_t*>(0x1);
    if (source_alive[1] && std::strcmp(name, "Idle Test") == 0)
        return reinterpret_cast<obs_source_t*>(0x2);
    if (std::strcmp(name, "Wrong Type") == 0)
        return reinterpret_cast<obs_source_t*>(0x3);
    return nullptr;
}

void obs_source_release(obs_source_t*) {}

const char* obs_source_get_unversioned_id(const obs_source_t* source) {
    return source == reinterpret_cast<const obs_source_t*>(0x3) ? "other_source" : "srtla_source";
}

const char* obs_source_get_name(const obs_source_t* source) {
    return source == reinterpret_cast<const obs_source_t*>(0x1) ? "SRTLA Test" : "Idle Test";
}

void obs_register_source_s(const obs_source_info* info, size_t size) {
    std::memcpy(&plugin_source, info, size < sizeof(plugin_source) ? size : sizeof(plugin_source));
}

void obs_source_output_video(obs_source_t*, const obs_source_frame* frame) {
    std::lock_guard<std::mutex> lock(stats.mutex);
    stats.video_frames++;
    stats.video_formats.insert(get_video_format_name(frame->format));
    stats.width = frame->width;
    stats.height = frame->height;
    stats.trc = frame->trc;
    if (!stats.first_video_ts)
        stats.first_video_ts = frame->timestamp;
    stats.last_video_ts = frame->timestamp;
    for (float value : frame->color_matrix)
        stats.color_matrix_set |= value != 0.0f;
}

void obs_source_output_audio(obs_source_t*, const obs_source_audio* audio) {
    std::lock_guard<std::mutex> lock(stats.mutex);
    stats.audio_packets++;
    stats.audio_samples += audio->frames;
    stats.audio_formats.insert(audio_format_name(audio->format));
    stats.speakers.insert(get_audio_channels(audio->speakers));
}
}

int main(int argc, char** argv) {
    const char* plugin_path = argc > 1 ? argv[1] : nullptr;
    int port = 0, seconds = 0, initial_port = 0, extra_port = 0, pbkeylen = -1;
    const char *passphrase = nullptr, *streamid = nullptr;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const int value = std::atoi(argv[i + 1]);
        if (option == "--passphrase")
            passphrase = argv[i + 1];
        else if (option == "--streamid")
            streamid = argv[i + 1];
        else if (option == "--pbkeylen")
            pbkeylen = value;
        else if (option == "--port")
            port = value;
        else if (option == "--seconds")
            seconds = value;
        else if (option == "--initial-port")
            initial_port = value;
        else if (option == "--extra-source")
            extra_port = value;
        else if (option == "--websocket")
            websocket_enabled = value != 0;
    }
    if (!plugin_path || port <= 0 || seconds <= 0) {
        std::fprintf(stderr,
                     "usage: %s PLUGIN.so --port PORT --seconds N [--initial-port PORT] [--extra-source PORT] "
                     "[--passphrase TEXT] [--pbkeylen N] [--streamid TEXT] [--websocket 0]\n",
                     argv[0]);
        return 2;
    }

    global_handler = proc_handler_create();
    websocket_handler = proc_handler_create();
    source_handlers[0] = proc_handler_create();
    source_handlers[1] = proc_handler_create();
    if (websocket_enabled)
        proc_handler_add(global_handler, "void obs_websocket_api_get_ph(out ptr ph)",
                         [](void*, calldata_t* data) { calldata_set_ptr(data, "ph", websocket_handler); }, nullptr);
    proc_handler_add(websocket_handler, "void vendor_register(string name, out ptr vendor)",
                     [](void*, calldata_t* data) {
                         require(std::strcmp(calldata_string(data, "name"), "obs-srtla-source") == 0,
                                 "vendor name");
                         calldata_set_ptr(data, "vendor", websocket_handler);
                     }, nullptr);
    proc_handler_add(websocket_handler,
                     "void vendor_request_register(ptr vendor, string type, ptr callback, out bool success)",
                     [](void*, calldata_t* data) {
                         require(std::strcmp(calldata_string(data, "type"), "GetSrtStats") == 0,
                                 "request type");
                         websocket_callback = *static_cast<WebsocketCallback*>(calldata_ptr(data, "callback"));
                         calldata_set_bool(data, "success", true);
                     }, nullptr);
    proc_handler_add(websocket_handler, "void vendor_request_unregister(ptr vendor, string type)",
                     [](void*, calldata_t*) { websocket_callback = {}; }, nullptr);

    void* module = dlopen(plugin_path, RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    auto module_load = reinterpret_cast<bool (*)()>(dlsym(module, "obs_module_load"));
    auto module_unload = reinterpret_cast<void (*)()>(dlsym(module, "obs_module_unload"));
    auto module_post_load = reinterpret_cast<void (*)()>(dlsym(module, "obs_module_post_load"));
    if (!module_load || !module_load() || !plugin_source.create) {
        std::fprintf(stderr, "plugin did not load or register a source\n");
        return 1;
    }
    require(module_post_load != nullptr, "module must register requests after all plugins load");
    if (module_post_load)
        module_post_load();
    require((websocket_callback.callback != nullptr) == websocket_enabled, "optional WebSocket registration");
    if (websocket_callback.callback) {
        check_error("");
        check_error("Missing Source");
        check_error("Wrong Type");
    }

    obs_data_t* settings = obs_data_create();
    if (plugin_source.get_defaults)
        plugin_source.get_defaults(settings);
    obs_data_set_int(settings, "srtla_port", initial_port ? initial_port : port);
    if (passphrase)
        obs_data_set_string(settings, "srt_passphrase", passphrase);
    if (streamid)
        obs_data_set_string(settings, "srt_streamid", streamid);
    if (pbkeylen >= 0)
        obs_data_set_int(settings, "srt_pbkeylen", pbkeylen);
    void* source = plugin_source.create(settings, reinterpret_cast<obs_source_t*>(0x1));
    source_alive[0] = true;

    void* extra_source = nullptr;
    if (extra_port) {
        obs_data_t* extra_settings = obs_data_create();
        obs_data_set_int(extra_settings, "srtla_port", extra_port);
        extra_source = plugin_source.create(extra_settings, reinterpret_cast<obs_source_t*>(0x2));
        source_alive[1] = true;
        obs_data_release(extra_settings);
    }

    double update_ms = 0;
    if (initial_port) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        obs_data_set_int(settings, "srtla_port", port);
        const auto start = std::chrono::steady_clock::now();
        plugin_source.update(source, settings);
        update_ms = elapsed_ms(start);
    }

    for (int i = 0; i < seconds * 20; ++i) {
        if (websocket_callback.callback)
            poll_stats();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const auto start = std::chrono::steady_clock::now();
    plugin_source.destroy(source);
    if (extra_source)
        plugin_source.destroy(extra_source);
    const double destroy_ms = elapsed_ms(start);
    source_alive[0] = source_alive[1] = false;
    if (websocket_callback.callback)
        check_error("SRTLA Test");
    obs_data_release(settings);
    if (module_unload)
        module_unload();
    require(!websocket_callback.callback, "request must be unregistered on module unload");
    proc_handler_destroy(source_handlers[0]);
    proc_handler_destroy(source_handlers[1]);
    proc_handler_destroy(websocket_handler);
    proc_handler_destroy(global_handler);

    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    std::lock_guard<std::mutex> lock(stats.mutex);
    std::printf(
        "RESULT video_frames=%llu video_formats=%s size=%ux%u trc=%d color_matrix=%d "
        "video_span_ms=%.0f audio_packets=%llu audio_samples=%llu audio_formats=%s channels=%s "
        "update_ms=%.0f destroy_ms=%.0f cpu_s=%.2f maxrss_kb=%ld "
        "websocket_failures=%u connected_queries=%u\n",
        stats.video_frames, join(stats.video_formats).c_str(), stats.width, stats.height, stats.trc,
        stats.color_matrix_set ? 1 : 0, (stats.last_video_ts - stats.first_video_ts) / 1e6,
        stats.audio_packets, stats.audio_samples, join(stats.audio_formats).c_str(),
        join(stats.speakers).c_str(), update_ms, destroy_ms,
        usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
            (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6,
        usage.ru_maxrss, websocket_failures, connected_queries);
    return websocket_failures ? 1 : 0;
}
