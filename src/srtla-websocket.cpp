#include "srtla-websocket.hpp"
#include "plugin-log.hpp"

#include <obs-module.h>
#include <cstring>

namespace {

constexpr char vendor_name[] = "obs-srtla-source";
constexpr char request_type[] = "GetSrtStats";
proc_handler_t* websocket_handler = nullptr;
void* vendor = nullptr;
bool registered = false;

// Public obs-websocket vendor API callback payload.
struct RequestCallback {
    void (*callback)(obs_data_t*, obs_data_t*, void*);
    void* private_data;
};

void set_error(obs_data_t* response, const char* error) {
    obs_data_set_bool(response, "success", false);
    obs_data_set_string(response, "error", error);
}

void get_srt_stats(obs_data_t* request, obs_data_t* response, void*) {
    const char* name = obs_data_get_string(request, "sourceName");
    if (!*name) {
        set_error(response, "sourceName must be a non-empty string");
        return;
    }
    obs_source_t* source = obs_get_source_by_name(name);
    if (!source) {
        set_error(response, "Source not found");
        return;
    }
    if (std::strcmp(obs_source_get_unversioned_id(source), "srtla_source") != 0) {
        obs_source_release(source);
        set_error(response, "Source is not an SRTLA source");
        return;
    }

    calldata_t data{};
    calldata_set_ptr(&data, "response", response);
    if (!proc_handler_call(obs_source_get_proc_handler(source), "get_srt_stats", &data))
        set_error(response, "SRT statistics are unavailable");
    else
        obs_data_set_string(response, "sourceName", obs_source_get_name(source));
    calldata_free(&data);
    obs_source_release(source);
}

}  // namespace

void srtla_websocket_initialize() {
    calldata_t data{};
    proc_handler_call(obs_get_proc_handler(), "obs_websocket_api_get_ph", &data);
    websocket_handler = static_cast<proc_handler_t*>(calldata_ptr(&data, "ph"));
    calldata_free(&data);
    if (!websocket_handler) {
        plugin_log(LOG_INFO, "obs-websocket unavailable; SRT statistics vendor request disabled");
        return;
    }

    data = {};
    calldata_set_string(&data, "name", vendor_name);
    proc_handler_call(websocket_handler, "vendor_register", &data);
    vendor = calldata_ptr(&data, "vendor");
    calldata_free(&data);
    if (!vendor) {
        plugin_log(LOG_WARNING, "could not register obs-websocket SRT statistics vendor");
        websocket_handler = nullptr;
        return;
    }

    RequestCallback callback{get_srt_stats, nullptr};
    data = {};
    calldata_set_ptr(&data, "vendor", vendor);
    calldata_set_string(&data, "type", request_type);
    calldata_set_ptr(&data, "callback", &callback);
    proc_handler_call(websocket_handler, "vendor_request_register", &data);
    registered = calldata_bool(&data, "success");
    calldata_free(&data);
    if (!registered) {
        plugin_log(LOG_WARNING, "could not register obs-websocket GetSrtStats request");
        srtla_websocket_shutdown();
        return;
    }
    plugin_log(LOG_INFO, "obs-websocket GetSrtStats vendor request registered");
}

void srtla_websocket_shutdown() {
    if (registered) {
        calldata_t data{};
        calldata_set_ptr(&data, "vendor", vendor);
        calldata_set_string(&data, "type", request_type);
        proc_handler_call(websocket_handler, "vendor_request_unregister", &data);
        calldata_free(&data);
    }
    registered = false;
    vendor = nullptr;
    websocket_handler = nullptr;
}
