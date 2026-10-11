#include <obs-module.h>
#include <util/platform.h>

#include <srtla_platform.h>

#include "plugin-log.hpp"
#include "srtla-receiver.hpp"
#include "srtla-websocket.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <srt/access_control.h>
#include <srt/srt.h>
// libsrt pulls in <syslog.h> (or its Windows stand-in), whose LOG_* macros
// shadow OBS's log levels.
#undef LOG_ERROR
#undef LOG_WARNING
#undef LOG_INFO
#undef LOG_DEBUG

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-srtla-source", "en-US")

namespace {

constexpr char setting_port[] = "srtla_port";
constexpr char setting_passphrase[] = "srt_passphrase";
constexpr char setting_pbkeylen[] = "srt_pbkeylen";
constexpr char setting_streamid[] = "srt_streamid";
constexpr uint16_t default_port = 5000;
constexpr int default_pbkeylen = 32;
constexpr size_t min_passphrase_length = 10;
constexpr size_t max_passphrase_length = 79;
constexpr int avio_buffer_size = 32 * 1024;
// Upper bound for how long blocking SRT calls take to notice a stop request.
constexpr int srt_timeout_ms = 250;
// Bonded links deliver packets out of order. SRT waits for up to this many
// later packets before reporting a gap as lost, instead of requesting a
// retransmission immediately (BELABOX recommends 10-50 for SRTLA receivers).
constexpr int srt_reorder_tolerance = 40;

void srt_log_to_obs(void*, int level, const char*, int, const char*, const char* message) {
    // The plugin reports actionable SRT failures itself; libsrt's own error
    // lines (e.g. accept on a listener closed during shutdown) stay at debug.
    const int obs_level = level <= LOG_CRIT ? LOG_WARNING : LOG_DEBUG;
    std::string text = message ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    const size_t start = text.find_first_not_of(": ");
    plugin_log(obs_level, "libsrt: %s", start == std::string::npos ? "" : text.c_str() + start);
}

// libsrt is started on first use and cleaned up only at module unload:
// srt_cleanup() blocks for about a second, too long for a source update.
std::once_flag srt_started_once;
bool srt_started = false;

bool start_srt() {
    std::call_once(srt_started_once, [] {
		srt_started = srt_startup() != SRT_ERROR;
		if (!srt_started)
			plugin_log(LOG_ERROR, "libsrt startup failed: %s", srt_getlasterror_str()); });
    return srt_started;
}

void close_socket(std::atomic<SRTSOCKET>& socket) {
    const SRTSOCKET value = socket.exchange(SRT_INVALID_SOCK);
    if (value != SRT_INVALID_SOCK)
        srt_close(value);
}

void log_av_error(const char* message, int error) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    plugin_log(LOG_WARNING, "%s: %s", message, text);
}

struct AvDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
    void operator()(AVFormatContext* format) const { avformat_close_input(&format); }
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
    void operator()(SwsContext* scaler) const { sws_freeContext(scaler); }
    void operator()(SwrContext* resampler) const { swr_free(&resampler); }
    void operator()(AVIOContext* io) const {
        av_freep(&io->buffer);
        avio_context_free(&io);
    }
};

template <typename T>
using AvPtr = std::unique_ptr<T, AvDeleter>;

// Formats OBS renders natively; anything else is converted to I420.
video_format obs_video_format(int format) {
    switch (format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            return VIDEO_FORMAT_I420;
        case AV_PIX_FMT_NV12:
            return VIDEO_FORMAT_NV12;
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUVJ422P:
            return VIDEO_FORMAT_I422;
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ444P:
            return VIDEO_FORMAT_I444;
        case AV_PIX_FMT_YUYV422:
            return VIDEO_FORMAT_YUY2;
        case AV_PIX_FMT_UYVY422:
            return VIDEO_FORMAT_UYVY;
        case AV_PIX_FMT_YVYU422:
            return VIDEO_FORMAT_YVYU;
        case AV_PIX_FMT_YUV420P10LE:
            return VIDEO_FORMAT_I010;
        case AV_PIX_FMT_P010LE:
            return VIDEO_FORMAT_P010;
        case AV_PIX_FMT_YUV422P10LE:
            return VIDEO_FORMAT_I210;
        case AV_PIX_FMT_YUV444P12LE:
            return VIDEO_FORMAT_I412;
        case AV_PIX_FMT_YUVA420P:
            return VIDEO_FORMAT_I40A;
        case AV_PIX_FMT_YUVA422P:
            return VIDEO_FORMAT_I42A;
        case AV_PIX_FMT_YUVA444P:
            return VIDEO_FORMAT_YUVA;
        default:
            return VIDEO_FORMAT_NONE;
    }
}

video_colorspace obs_colorspace(const AVFrame* frame) {
    if (frame->color_trc == AVCOL_TRC_SMPTE2084)
        return VIDEO_CS_2100_PQ;
    if (frame->color_trc == AVCOL_TRC_ARIB_STD_B67)
        return VIDEO_CS_2100_HLG;
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709:
            return frame->color_trc == AVCOL_TRC_IEC61966_2_1 ? VIDEO_CS_SRGB : VIDEO_CS_709;
        case AVCOL_SPC_FCC:
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_SMPTE240M:
            return VIDEO_CS_601;
        default:
            // Untagged streams: HD content is almost always BT.709.
            return frame->height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
    }
}

video_trc obs_trc(AVColorTransferCharacteristic trc) {
    switch (trc) {
        case AVCOL_TRC_SMPTE2084:
            return VIDEO_TRC_PQ;
        case AVCOL_TRC_ARIB_STD_B67:
            return VIDEO_TRC_HLG;
        default:
            return VIDEO_TRC_DEFAULT;
    }
}

audio_format obs_audio_format(int format) {
    switch (format) {
        case AV_SAMPLE_FMT_U8:
            return AUDIO_FORMAT_U8BIT;
        case AV_SAMPLE_FMT_S16:
            return AUDIO_FORMAT_16BIT;
        case AV_SAMPLE_FMT_S32:
            return AUDIO_FORMAT_32BIT;
        case AV_SAMPLE_FMT_FLT:
            return AUDIO_FORMAT_FLOAT;
        case AV_SAMPLE_FMT_U8P:
            return AUDIO_FORMAT_U8BIT_PLANAR;
        case AV_SAMPLE_FMT_S16P:
            return AUDIO_FORMAT_16BIT_PLANAR;
        case AV_SAMPLE_FMT_S32P:
            return AUDIO_FORMAT_32BIT_PLANAR;
        case AV_SAMPLE_FMT_FLTP:
            return AUDIO_FORMAT_FLOAT_PLANAR;
        default:
            return AUDIO_FORMAT_UNKNOWN;
    }
}

// OBS speaker layouts have fixed channel orders, so multichannel audio is
// passed through only when its FFmpeg layout has exactly that order.
speaker_layout obs_speakers(const AVChannelLayout& layout) {
    if (layout.nb_channels == 1)
        return SPEAKERS_MONO;
    if (layout.nb_channels == 2)
        return SPEAKERS_STEREO;
    static const struct
    {
        AVChannelLayout layout;
        speaker_layout speakers;
    } layouts[] = {
        {AV_CHANNEL_LAYOUT_2POINT1, SPEAKERS_2POINT1},
        {AV_CHANNEL_LAYOUT_4POINT0, SPEAKERS_4POINT0},
        {AV_CHANNEL_LAYOUT_4POINT1, SPEAKERS_4POINT1},
        {AV_CHANNEL_LAYOUT_5POINT1, SPEAKERS_5POINT1},
        {AV_CHANNEL_LAYOUT_5POINT1_BACK, SPEAKERS_5POINT1},
        {AV_CHANNEL_LAYOUT_7POINT1, SPEAKERS_7POINT1},
    };
    for (const auto& entry : layouts)
        if (av_channel_layout_compare(&layout, &entry.layout) == 0)
            return entry.speakers;
    return SPEAKERS_UNKNOWN;
}

struct SrtReader {
    SRTSOCKET socket;
    const std::atomic<bool>& stopping;
};

int read_srt(void* opaque, uint8_t* buffer, int size) {
    auto* reader = static_cast<SrtReader*>(opaque);
    while (!reader->stopping) {
        const int received = srt_recvmsg(reader->socket, reinterpret_cast<char*>(buffer), size);
        if (received > 0)
            return received;
        const int error = srt_getlasterror(nullptr);
        if (error != SRT_ETIMEOUT && error != SRT_EASYNCRCV)
            break;
    }
    return AVERROR_EOF;
}

// Demuxes and decodes one SRT connection's MPEG-TS stream into OBS frames.
// Decoded frames are handed to OBS without conversion whenever OBS supports
// their pixel/sample format; OBS copies them before output_* returns.
class MediaDecoder {
   public:
    explicit MediaDecoder(obs_source_t* source) : source_(source) {}

    void run(SRTSOCKET socket, const std::atomic<bool>& stopping) {
        SrtReader reader{socket, stopping};
        auto* buffer = static_cast<uint8_t*>(av_malloc(avio_buffer_size));
        AvPtr<AVIOContext> io(buffer ? avio_alloc_context(buffer, avio_buffer_size, 0, &reader, read_srt, nullptr, nullptr) : nullptr);
        if (!io) {
            av_free(buffer);
            return;
        }

        AVFormatContext* raw_format = avformat_alloc_context();
        if (!raw_format)
            return;
        raw_format->pb = io.get();
        // On failure avformat_open_input frees the context itself.
        int result = avformat_open_input(&raw_format, nullptr, av_find_input_format("mpegts"), nullptr);
        if (result < 0) {
            if (!stopping)
                log_av_error("could not open MPEG-TS input", result);
            return;
        }
        AvPtr<AVFormatContext> format(raw_format);

        result = avformat_find_stream_info(format.get(), nullptr);
        if (result < 0) {
            if (!stopping)
                log_av_error("could not read MPEG-TS stream information", result);
            return;
        }
        decode(format.get(), stopping);
    }

   private:
    int open_decoder(AVFormatContext* format, AVMediaType type, AvPtr<AVCodecContext>& decoder) {
        const AVCodec* codec = nullptr;
        const int index = av_find_best_stream(format, type, -1, -1, &codec, 0);
        const char* type_name = av_get_media_type_string(type);
        if (index == AVERROR_DECODER_NOT_FOUND)
            plugin_log(LOG_WARNING, "no decoder for the %s stream", type_name);
        if (index < 0)
            return -1;

        const AVCodecParameters* parameters = format->streams[index]->codecpar;
        AvPtr<AVCodecContext> context(avcodec_alloc_context3(codec));
        if (!context || avcodec_parameters_to_context(context.get(), parameters) < 0)
            return -1;
        const int result = avcodec_open2(context.get(), codec, nullptr);
        if (result < 0) {
            log_av_error("could not open decoder", result);
            return -1;
        }
        if (type == AVMEDIA_TYPE_VIDEO)
            plugin_log(LOG_INFO, "video stream: %s %dx%d", codec->name, parameters->width, parameters->height);
        else
            plugin_log(LOG_INFO, "audio stream: %s %d Hz, %d channels", codec->name, parameters->sample_rate, parameters->ch_layout.nb_channels);
        decoder = std::move(context);
        return index;
    }

    void decode(AVFormatContext* format, const std::atomic<bool>& stopping) {
        const int video_index = open_decoder(format, AVMEDIA_TYPE_VIDEO, video_);
        const int audio_index = open_decoder(format, AVMEDIA_TYPE_AUDIO, audio_);
        if (!video_ && !audio_) {
            plugin_log(LOG_WARNING, "stream has no supported audio or video");
            return;
        }

        AvPtr<AVPacket> packet(av_packet_alloc());
        AvPtr<AVFrame> frame(av_frame_alloc());
        if (!packet || !frame)
            return;

        int result = 0;
        while (!stopping && (result = av_read_frame(format, packet.get())) >= 0) {
            const int index = packet->stream_index;
            AVCodecContext* decoder = index == video_index   ? video_.get()
                                      : index == audio_index ? audio_.get()
                                                             : nullptr;
            if (decoder)
                decode_packet(decoder, packet.get(), frame.get(), format->streams[index]->time_base);
            av_packet_unref(packet.get());
        }
        if (!stopping && result < 0 && result != AVERROR_EOF)
            log_av_error("MPEG-TS read failed", result);
    }

    void decode_packet(AVCodecContext* decoder, const AVPacket* packet, AVFrame* frame, AVRational time_base) {
        int result = avcodec_send_packet(decoder, packet);
        while (result >= 0) {
            result = avcodec_receive_frame(decoder, frame);
            if (result < 0)
                break;
            const uint64_t timestamp = obs_timestamp(frame->best_effort_timestamp, time_base);
            if (decoder == video_.get())
                output_video(frame, timestamp);
            else
                output_audio(frame, timestamp);
            av_frame_unref(frame);
        }
        // Lossy links produce corrupt packets; one report per stream is enough.
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF && !decode_error_logged_) {
            decode_error_logged_ = true;
            log_av_error("decoding failed", result);
        }
    }

    // Maps media time onto the OBS clock. Audio and video share one origin so
    // OBS keeps them in sync; OBS itself handles timestamp jumps.
    uint64_t obs_timestamp(int64_t pts, AVRational time_base) {
        if (pts == AV_NOPTS_VALUE)
            return os_gettime_ns();
        const int64_t media_ns = av_rescale_q(pts, time_base, AVRational{1, 1000000000});
        if (!origin_set_) {
            origin_set_ = true;
            media_origin_ns_ = media_ns;
            clock_origin_ns_ = static_cast<int64_t>(os_gettime_ns());
        }
        const int64_t timestamp = clock_origin_ns_ + (media_ns - media_origin_ns_);
        return timestamp > 0 ? static_cast<uint64_t>(timestamp) : 0;
    }

    const AVFrame* convert_to_i420(const AVFrame* decoded) {
        scaler_.reset(sws_getCachedContext(scaler_.release(), decoded->width, decoded->height,
                                           static_cast<AVPixelFormat>(decoded->format), decoded->width,
                                           decoded->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr,
                                           nullptr));
        if (!scaler_)
            return nullptr;
        if (!converted_ || converted_->width != decoded->width || converted_->height != decoded->height) {
            converted_.reset(av_frame_alloc());
            if (!converted_)
                return nullptr;
            converted_->format = AV_PIX_FMT_YUV420P;
            converted_->width = decoded->width;
            converted_->height = decoded->height;
            if (av_frame_get_buffer(converted_.get(), 0) < 0) {
                converted_.reset();
                return nullptr;
            }
        }
        if (sws_scale(scaler_.get(), decoded->data, decoded->linesize, 0, decoded->height, converted_->data, converted_->linesize) <= 0)
            return nullptr;
        return converted_.get();
    }

    void output_video(const AVFrame* decoded, uint64_t timestamp) {
        video_format format = obs_video_format(decoded->format);
        const AVFrame* image = decoded;
        if (format == VIDEO_FORMAT_NONE) {
            format = VIDEO_FORMAT_I420;
            image = convert_to_i420(decoded);
            if (!image)
                return;
        }

        // A zeroed color matrix renders black, so it is always filled in;
        // it is recomputed only when the stream's color properties change.
        const video_colorspace space = obs_colorspace(decoded);
        const video_range_type range = decoded->color_range == AVCOL_RANGE_JPEG ? VIDEO_RANGE_FULL
                                                                                : VIDEO_RANGE_PARTIAL;
        if (format != frame_.format || space != color_space_ || range != color_range_) {
            frame_.format = format;
            color_space_ = space;
            color_range_ = range;
            color_valid_ = video_format_get_parameters_for_format(space, range, format, frame_.color_matrix, frame_.color_range_min, frame_.color_range_max);
            plugin_log(color_valid_ ? LOG_INFO : LOG_WARNING,
                       "video format: %s -> %s, colorspace %d, %s range%s",
                       av_get_pix_fmt_name(static_cast<AVPixelFormat>(decoded->format)),
                       get_video_format_name(format), static_cast<int>(space),
                       range == VIDEO_RANGE_FULL ? "full" : "partial",
                       color_valid_ ? "" : " (unsupported, frames dropped)");
        }
        if (!color_valid_)
            return;

        frame_.width = static_cast<uint32_t>(image->width);
        frame_.height = static_cast<uint32_t>(image->height);
        frame_.timestamp = timestamp;
        frame_.full_range = range == VIDEO_RANGE_FULL;
        frame_.trc = obs_trc(decoded->color_trc);
        for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane) {
            frame_.data[plane] = image->data[plane];
            frame_.linesize[plane] = static_cast<uint32_t>(image->linesize[plane]);
        }
        obs_source_output_video(source_, &frame_);
    }

    // swr_convert_frame() configures the resampler from the frames and
    // reports AVERROR_INPUT_CHANGED when the input format changes.
    const AVFrame* downmix_to_stereo(const AVFrame* decoded) {
        if (!resampler_)
            resampler_.reset(swr_alloc());
        if (!downmixed_)
            downmixed_.reset(av_frame_alloc());
        if (!resampler_ || !downmixed_)
            return nullptr;
        if (!downmix_logged_) {
            downmix_logged_ = true;
            char layout[64] = {};
            av_channel_layout_describe(&decoded->ch_layout, layout, sizeof(layout));
            plugin_log(LOG_INFO, "audio %s %s is downmixed to stereo", av_get_sample_fmt_name(static_cast<AVSampleFormat>(decoded->format)), layout);
        }
        int result = AVERROR_INPUT_CHANGED;
        for (int attempt = 0; attempt < 2 && result == AVERROR_INPUT_CHANGED; ++attempt) {
            if (attempt > 0)
                swr_close(resampler_.get());
            av_frame_unref(downmixed_.get());
            downmixed_->format = AV_SAMPLE_FMT_FLTP;
            downmixed_->sample_rate = decoded->sample_rate;
            av_channel_layout_default(&downmixed_->ch_layout, 2);
            result = swr_convert_frame(resampler_.get(), downmixed_.get(), decoded);
        }
        return result < 0 ? nullptr : downmixed_.get();
    }

    void output_audio(const AVFrame* decoded, uint64_t timestamp) {
        const AVFrame* samples = decoded;
        obs_source_audio audio{};
        audio.format = obs_audio_format(decoded->format);
        audio.speakers = obs_speakers(decoded->ch_layout);
        if (audio.format == AUDIO_FORMAT_UNKNOWN || audio.speakers == SPEAKERS_UNKNOWN) {
            samples = downmix_to_stereo(decoded);
            if (!samples)
                return;
            audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
            audio.speakers = SPEAKERS_STEREO;
        }
        audio.samples_per_sec = static_cast<uint32_t>(samples->sample_rate);
        audio.frames = static_cast<uint32_t>(samples->nb_samples);
        audio.timestamp = timestamp;
        const int planes = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(samples->format))
                               ? samples->ch_layout.nb_channels
                               : 1;
        for (int plane = 0; plane < planes; ++plane)
            audio.data[plane] = samples->extended_data[plane];
        obs_source_output_audio(source_, &audio);
    }

    obs_source_t* source_;
    AvPtr<AVCodecContext> video_;
    AvPtr<AVCodecContext> audio_;
    AvPtr<SwsContext> scaler_;
    AvPtr<AVFrame> converted_;
    AvPtr<SwrContext> resampler_;
    AvPtr<AVFrame> downmixed_;
    obs_source_frame frame_{};
    video_colorspace color_space_ = VIDEO_CS_DEFAULT;
    video_range_type color_range_ = VIDEO_RANGE_DEFAULT;
    bool color_valid_ = false;
    bool origin_set_ = false;
    int64_t media_origin_ns_ = 0;
    int64_t clock_origin_ns_ = 0;
    bool decode_error_logged_ = false;
    bool downmix_logged_ = false;
};

// Settings that require restarting the receiver when they change. The
// passphrase and stream ID apply to every SRT connection the sender makes.
struct ReceiverConfig {
    uint16_t port = default_port;
    std::string passphrase;
    int pbkeylen = default_pbkeylen;
    std::string streamid;

    bool operator==(const ReceiverConfig& other) const {
        return port == other.port && passphrase == other.passphrase && pbkeylen == other.pbkeylen && streamid == other.streamid;
    }
};

ReceiverConfig read_config(obs_data_t* settings) {
    ReceiverConfig config;
    const long long port = obs_data_get_int(settings, setting_port);
    if (port >= 1 && port <= 65535)
        config.port = static_cast<uint16_t>(port);
    const long long pbkeylen = obs_data_get_int(settings, setting_pbkeylen);
    if (pbkeylen == 0 || pbkeylen == 16 || pbkeylen == 24 || pbkeylen == 32)
        config.pbkeylen = static_cast<int>(pbkeylen);
    config.passphrase = obs_data_get_string(settings, setting_passphrase);
    config.streamid = obs_data_get_string(settings, setting_streamid);
    return config;
}

// The stream ID comes from the remote sender, so it is sanitized for the log.
std::string printable(const char* text) {
    std::string result;
    for (const char* c = text; *c && result.size() < 128; ++c)
        result += *c >= 0x20 && *c < 0x7f ? *c : '?';
    return result;
}

// Runs in the libsrt receive thread before a connection is accepted. The
// rejection travels back through the SRTLA relay, which also throttles
// senders that keep failing.
int check_stream_id(void* opaque, SRTSOCKET socket, int, const sockaddr*, const char* streamid) {
    const auto& required = *static_cast<const std::string*>(opaque);
    const char* received = streamid ? streamid : "";
    if (required == received)
        return 0;
    srt_setrejectreason(socket, SRT_REJX_UNAUTHORIZED);
    plugin_log(LOG_WARNING, "rejected SRT stream: stream ID \"%s\" does not match the required stream ID", printable(received).c_str());
    return -1;
}

// Loopback SRT listener on an ephemeral port that the SRTLA receiver feeds.
// Accepted sockets inherit its encryption settings; SRT rejects senders whose
// passphrase does not match, including unencrypted senders.
SRTSOCKET open_local_listener(const ReceiverConfig& config, uint16_t& port) {
    const SRTSOCKET listener = srt_create_socket();
    if (listener == SRT_INVALID_SOCK) {
        plugin_log(LOG_ERROR, "could not create local SRT socket: %s", srt_getlasterror_str());
        return SRT_INVALID_SOCK;
    }
    bool ok = srt_setsockflag(listener, SRTO_RCVTIMEO, &srt_timeout_ms, sizeof(srt_timeout_ms)) != SRT_ERROR &&
              srt_setsockflag(listener, SRTO_LOSSMAXTTL, &srt_reorder_tolerance, sizeof(srt_reorder_tolerance)) != SRT_ERROR;
    if (ok && !config.passphrase.empty()) {
        const bool enforce = true;
        ok = srt_setsockflag(listener, SRTO_ENFORCEDENCRYPTION, &enforce, sizeof(enforce)) != SRT_ERROR &&
             srt_setsockflag(listener, SRTO_PASSPHRASE, config.passphrase.data(), static_cast<int>(config.passphrase.size())) != SRT_ERROR &&
             srt_setsockflag(listener, SRTO_PBKEYLEN, &config.pbkeylen, sizeof(config.pbkeylen)) != SRT_ERROR;
    }
    if (ok && !config.streamid.empty())
        ok = srt_listen_callback(listener, check_stream_id,
                                 const_cast<std::string*>(&config.streamid)) != SRT_ERROR;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_length = sizeof(address);
    if (!ok || srt_bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SRT_ERROR ||
        srt_listen(listener, 1) == SRT_ERROR ||
        srt_getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) == SRT_ERROR) {
        plugin_log(LOG_ERROR, "could not open local SRT listener: %s", srt_getlasterror_str());
        srt_close(listener);
        return SRT_INVALID_SOCK;
    }
    port = ntohs(address.sin_port);
    return listener;
}

bool valid_passphrase(const std::string& passphrase) {
    return passphrase.empty() ||
           (passphrase.size() >= min_passphrase_length && passphrase.size() <= max_passphrase_length);
}

std::string encryption_state(SRTSOCKET stream) {
    int state = SRT_KM_S_UNSECURED;
    int size = sizeof(state);
    if (srt_getsockflag(stream, SRTO_RCVKMSTATE, &state, &size) == SRT_ERROR || state != SRT_KM_S_SECURED)
        return "unencrypted";
    int pbkeylen = 0;
    size = sizeof(pbkeylen);
    srt_getsockflag(stream, SRTO_PBKEYLEN, &pbkeylen, &size);
    return "AES-" + std::to_string(pbkeylen * 8) + " encrypted";
}

// One OBS source: a worker thread that runs the SRTLA receiver and decodes
// SRT connections one at a time until the source is stopped.
class SrtlaSource {
   public:
    explicit SrtlaSource(obs_source_t* source) : source_(source) {
        proc_handler_add(obs_source_get_proc_handler(source_), "void get_srt_stats(ptr response)",
                         [](void* opaque, calldata_t* data) {
                             static_cast<SrtlaSource*>(opaque)->get_stats(
                                 static_cast<obs_data_t*>(calldata_ptr(data, "response")));
                         },
                         this);
    }
    ~SrtlaSource() { stop(); }

    SrtlaSource(const SrtlaSource&) = delete;
    SrtlaSource& operator=(const SrtlaSource&) = delete;

    void update(obs_data_t* settings) {
        ReceiverConfig config = read_config(settings);
        if (thread_.joinable() && config == config_)
            return;
        stop();
        config_ = std::move(config);
        stopping_ = false;
        try {
            thread_ = std::thread(&SrtlaSource::run, this);
        } catch (const std::system_error& error) {
            plugin_log(LOG_ERROR, "could not start receiver thread: %s", error.what());
        }
    }

   private:
    void get_stats(obs_data_t* response) {
        // Keep the socket alive throughout the query, including source updates.
        std::lock_guard<std::mutex> lock(stream_mutex_);
        const SRTSOCKET stream = stream_.load();
        const bool connected = stream != SRT_INVALID_SOCK && srt_getsockstate(stream) == SRTS_CONNECTED;
        obs_data_set_bool(response, "connected", connected);
        if (!connected) {
            obs_data_set_bool(response, "success", true);
            return;
        }
        SRT_TRACEBSTATS stats{};
        if (srt_bistats(stream, &stats, 0, 1) == SRT_ERROR) {
            obs_data_set_bool(response, "success", false);
            obs_data_set_string(response, "error", srt_getlasterror_str());
            return;
        }
        obs_data_t* values = obs_data_create();
        obs_data_set_int(values, "msTimeStamp", stats.msTimeStamp);
        obs_data_set_int(values, "pktRecvTotal", stats.pktRecvTotal);
        obs_data_set_int(values, "pktRecvUniqueTotal", stats.pktRecvUniqueTotal);
        obs_data_set_int(values, "byteRecvTotal", stats.byteRecvTotal);
        obs_data_set_int(values, "byteRecvUniqueTotal", stats.byteRecvUniqueTotal);
        obs_data_set_int(values, "pktRcvLossTotal", stats.pktRcvLossTotal);
        obs_data_set_int(values, "byteRcvLossTotal", stats.byteRcvLossTotal);
        obs_data_set_int(values, "pktRcvDropTotal", stats.pktRcvDropTotal);
        obs_data_set_int(values, "byteRcvDropTotal", stats.byteRcvDropTotal);
        obs_data_set_int(values, "pktRcvUndecryptTotal", stats.pktRcvUndecryptTotal);
        obs_data_set_int(values, "pktRcvRetrans", stats.pktRcvRetrans);
        obs_data_set_int(values, "pktRcvBelated", stats.pktRcvBelated);
        obs_data_set_int(values, "pktSentACKTotal", stats.pktSentACKTotal);
        obs_data_set_int(values, "pktSentNAKTotal", stats.pktSentNAKTotal);
        obs_data_set_double(values, "mbpsRecvRate", stats.mbpsRecvRate);
        obs_data_set_double(values, "msRTT", stats.msRTT);
        obs_data_set_double(values, "mbpsBandwidth", stats.mbpsBandwidth);
        obs_data_set_int(values, "pktReorderDistance", stats.pktReorderDistance);
        obs_data_set_int(values, "pktReorderTolerance", stats.pktReorderTolerance);
        obs_data_set_int(values, "pktRcvBuf", stats.pktRcvBuf);
        obs_data_set_int(values, "byteRcvBuf", stats.byteRcvBuf);
        obs_data_set_int(values, "msRcvBuf", stats.msRcvBuf);
        obs_data_set_int(values, "msRcvTsbPdDelay", stats.msRcvTsbPdDelay);
        obs_data_set_int(values, "byteAvailRcvBuf", stats.byteAvailRcvBuf);
        obs_data_set_obj(response, "stats", values);
        obs_data_release(values);
        obs_data_set_bool(response, "success", true);
    }

    void close_stream() {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        close_socket(stream_);
    }

    void stop() {
        stopping_ = true;
        // Closing the sockets unblocks accept/receive in the worker.
        close_stream();
        close_socket(listener_);
        if (thread_.joinable())
            thread_.join();
    }

    void run() {
        if (start_srt())
            serve();
        close_stream();
        close_socket(listener_);
    }

    void serve() {
        // Refusing to start is safer than silently accepting unencrypted streams.
        if (!valid_passphrase(config_.passphrase)) {
            plugin_log(LOG_ERROR, "SRT passphrase must be %zu to %zu characters; not accepting streams", min_passphrase_length, max_passphrase_length);
            return;
        }

        uint16_t srt_port = 0;
        const SRTSOCKET listener = open_local_listener(config_, srt_port);
        if (listener == SRT_INVALID_SOCK)
            return;
        listener_ = listener;

        srtla_embed::EmbeddedSrtlaReceiver receiver;
        if (!receiver.start(config_.port, srt_port))
            return;
        if (config_.passphrase.empty())
            plugin_log(LOG_INFO, "SRT encryption disabled, stream ID check %s", config_.streamid.empty() ? "disabled" : "enabled");
        else
            plugin_log(LOG_INFO, "SRT encryption required (%s), stream ID check %s",
                       config_.pbkeylen ? ("AES-" + std::to_string(config_.pbkeylen * 8)).c_str()
                                        : "key length chosen by sender",
                       config_.streamid.empty() ? "disabled" : "enabled");
        plugin_log(LOG_INFO, "listening for SRTLA on UDP port %u", config_.port);

        while (!stopping_ && receiver.running()) {
            const SRTSOCKET stream = srt_accept(listener, nullptr, nullptr);
            if (stream == SRT_INVALID_SOCK) {
                const int error = srt_getlasterror(nullptr);
                if (!stopping_ && error != SRT_ETIMEOUT && error != SRT_EASYNCRCV)
                    plugin_log(LOG_WARNING, "SRT accept failed: %s", srt_getlasterror_str());
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(stream_mutex_);
                stream_ = stream;
            }
            if (srt_setsockflag(stream, SRTO_RCVTIMEO, &srt_timeout_ms, sizeof(srt_timeout_ms)) == SRT_ERROR) {
                close_stream();
                continue;
            }
            plugin_log(LOG_INFO, "SRT stream connected (%s)", encryption_state(stream).c_str());
            MediaDecoder(source_).run(stream, stopping_);
            close_stream();
            if (!stopping_)
                plugin_log(LOG_INFO, "SRT stream ended");
        }
        if (!stopping_)
            plugin_log(LOG_ERROR, "SRTLA receiver stopped unexpectedly");
    }

    obs_source_t* source_;
    // Read by the worker and libsrt callbacks; replaced only while stopped.
    ReceiverConfig config_;
    std::atomic<bool> stopping_{false};
    std::atomic<SRTSOCKET> listener_{SRT_INVALID_SOCK};
    std::atomic<SRTSOCKET> stream_{SRT_INVALID_SOCK};
    std::mutex stream_mutex_;
    std::thread thread_;
};

const char* source_get_name(void*) {
    return obs_module_text("SRTLASource");
}

void* source_create(obs_data_t* settings, obs_source_t* obs_source) {
    auto* source = new SrtlaSource(obs_source);
    source->update(settings);
    return source;
}

void source_destroy(void* data) {
    delete static_cast<SrtlaSource*>(data);
}

void source_update(void* data, obs_data_t* settings) {
    static_cast<SrtlaSource*>(data)->update(settings);
}

obs_properties_t* source_properties(void*) {
    obs_properties_t* properties = obs_properties_create();
    // Every change restarts the receiver, so apply them only on OK, not per keystroke.
    obs_properties_set_flags(properties, OBS_PROPERTIES_DEFER_UPDATE);
    obs_properties_add_int(properties, setting_port, obs_module_text("Port"), 1, 65535, 1);

    obs_property_t* passphrase = obs_properties_add_text(properties, setting_passphrase, obs_module_text("Passphrase"), OBS_TEXT_PASSWORD);
    obs_property_set_long_description(passphrase, obs_module_text("Passphrase.Description"));

    obs_property_t* pbkeylen = obs_properties_add_list(properties, setting_pbkeylen, obs_module_text("KeyLength"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(pbkeylen, obs_module_text("KeyLength.Auto"), 0);
    obs_property_list_add_int(pbkeylen, "AES-128", 16);
    obs_property_list_add_int(pbkeylen, "AES-192", 24);
    obs_property_list_add_int(pbkeylen, "AES-256", 32);
    obs_property_set_long_description(pbkeylen, obs_module_text("KeyLength.Description"));

    obs_property_t* streamid = obs_properties_add_text(properties, setting_streamid, obs_module_text("StreamId"), OBS_TEXT_PASSWORD);
    obs_property_set_long_description(streamid, obs_module_text("StreamId.Description"));
    return properties;
}

void source_defaults(obs_data_t* settings) {
    obs_data_set_default_int(settings, setting_port, default_port);
    obs_data_set_default_string(settings, setting_passphrase, "");
    obs_data_set_default_int(settings, setting_pbkeylen, default_pbkeylen);
    obs_data_set_default_string(settings, setting_streamid, "");
}

// Designated initializers are C++20; MSVC rejects them in C++17 mode.
obs_source_info make_source_info() {
    obs_source_info info{};
    info.id = "srtla_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO;
    info.get_name = source_get_name;
    info.create = source_create;
    info.destroy = source_destroy;
    info.get_defaults = source_defaults;
    info.get_properties = source_properties;
    info.update = source_update;
    info.icon_type = OBS_ICON_TYPE_CAMERA;
    return info;
}

obs_source_info source_info = make_source_info();

}  // namespace

bool obs_module_load(void) {
    srt_setloghandler(nullptr, srt_log_to_obs);
    srt_setlogflags(SRT_LOGF_DISABLE_TIME | SRT_LOGF_DISABLE_THREADNAME | SRT_LOGF_DISABLE_SEVERITY |
                    SRT_LOGF_DISABLE_EOL);
    srt_setloglevel(LOG_ERR);
    obs_register_source(&source_info);
    plugin_log(LOG_INFO, "plugin loaded");
    return true;
}

void obs_module_post_load(void) {
    srtla_websocket_initialize();
}

void obs_module_unload(void) {
    srtla_websocket_shutdown();
    if (srt_started)
        srt_cleanup();
}
