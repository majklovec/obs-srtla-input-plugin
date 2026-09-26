#include "srtla-receiver.hpp"

#include <srtla_platform.h>

#include <obs-module.h>
#include <util/platform.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <srt/srt.h>
// libsrt pulls in <syslog.h> (or its Windows stand-in), whose LOG_* macros
// shadow OBS's log levels.
#undef LOG_ERROR
#undef LOG_WARNING
#undef LOG_INFO
#undef LOG_DEBUG

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{

	constexpr char setting_port[] = "srtla_port";
	constexpr int avio_buffer_size = 32 * 1024;

	static void srt_log_to_obs(void *, int level, const char *, int, const char *,
							   const char *message)
	{
		// The plugin reports actionable SRT failures itself; libsrt's own error
		// lines (e.g. accept on a listener closed during shutdown) stay at debug.
		const int obs_level = level <= LOG_CRIT ? LOG_WARNING : LOG_DEBUG;
		std::string text = message ? message : "";
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
			text.pop_back();
		const size_t start = text.find_first_not_of(": ");
		blog(obs_level, "[obs-srtla-source] libsrt: %s",
			 start == std::string::npos ? "" : text.c_str() + start);
	}

	class SrtRuntime
	{
	public:
		SrtRuntime()
		{
			std::lock_guard<std::mutex> lock(runtime_mutex());
			if (runtime_references() == 0)
			{
				srt_setloghandler(nullptr, srt_log_to_obs);
				srt_setlogflags(SRT_LOGF_DISABLE_TIME | SRT_LOGF_DISABLE_THREADNAME |
								SRT_LOGF_DISABLE_SEVERITY | SRT_LOGF_DISABLE_EOL);
				srt_setloglevel(LOG_ERR);
				if (srt_startup() == SRT_ERROR)
					return;
			}
			ready_ = true;
			++runtime_references();
		}

		~SrtRuntime()
		{
			if (!ready_)
				return;
			std::lock_guard<std::mutex> lock(runtime_mutex());
			if (--runtime_references() == 0)
				srt_cleanup();
		}

		bool ready() const { return ready_; }

	private:
		static std::mutex &runtime_mutex()
		{
			static std::mutex mutex;
			return mutex;
		}

		static unsigned &runtime_references()
		{
			static unsigned references = 0;
			return references;
		}

		bool ready_ = false;
	};

	static void close_socket(SRTSOCKET &socket)
	{
		if (socket != SRT_INVALID_SOCK)
		{
			srt_close(socket);
			socket = SRT_INVALID_SOCK;
		}
	}

	static void close_socket(std::atomic<SRTSOCKET> &socket)
	{
		const SRTSOCKET value = socket.exchange(SRT_INVALID_SOCK);
		if (value != SRT_INVALID_SOCK)
			srt_close(value);
	}

	struct AvioReader
	{
		SRTSOCKET socket = SRT_INVALID_SOCK;
		const std::atomic<bool> *stopping = nullptr;
	};

	static int read_srt(void *opaque, uint8_t *buffer, int buffer_size)
	{
		auto *reader = static_cast<AvioReader *>(opaque);
		while (!reader->stopping->load())
		{
			const int received = srt_recvmsg(reader->socket, reinterpret_cast<char *>(buffer),
											 buffer_size);
			if (received > 0)
				return received;
			if (reader->stopping->load())
				return AVERROR_EOF;
			const int error = srt_getlasterror(nullptr);
			if (error == SRT_EASYNCRCV || error == SRT_ETIMEOUT)
				continue;
			return AVERROR_EOF;
		}
		return AVERROR_EOF;
	}

	struct PacketDeleter
	{
		void operator()(AVPacket *packet) const { av_packet_free(&packet); }
	};

	struct FrameDeleter
	{
		void operator()(AVFrame *frame) const { av_frame_free(&frame); }
	};

	static uint64_t timestamp_ns(int64_t pts, AVRational time_base,
								 bool &origin_set, int64_t &media_origin_ns,
								 uint64_t &capture_origin_ns)
	{
		if (pts == AV_NOPTS_VALUE)
			return os_gettime_ns();
		const int64_t media_time_ns = av_rescale_q(pts, time_base,
												   AVRational{1, 1000000000});
		if (!origin_set)
		{
			origin_set = true;
			media_origin_ns = media_time_ns;
			capture_origin_ns = os_gettime_ns();
		}
		const int64_t elapsed_ns = media_time_ns - media_origin_ns;
		return elapsed_ns < 0 ? capture_origin_ns
							  : capture_origin_ns + static_cast<uint64_t>(elapsed_ns);
	}

	class MediaDecoder
	{
	public:
		explicit MediaDecoder(obs_source_t *source) : source_(source) {}

		~MediaDecoder()
		{
			if (video_codec_)
				avcodec_free_context(&video_codec_);
			if (audio_codec_)
				avcodec_free_context(&audio_codec_);
			if (scaler_)
				sws_freeContext(scaler_);
			if (resampler_)
				swr_free(&resampler_);
			av_channel_layout_uninit(&input_layout_);
			av_channel_layout_uninit(&output_layout_);
		}

		bool run(SRTSOCKET socket, const std::atomic<bool> &stopping)
		{
			AvioReader reader{socket, &stopping};
			uint8_t *buffer = static_cast<uint8_t *>(av_malloc(avio_buffer_size));
			if (!buffer)
				return false;
			AVIOContext *io = avio_alloc_context(buffer, avio_buffer_size, 0, &reader,
												 read_srt, nullptr, nullptr);
			if (!io)
			{
				av_free(buffer);
				return false;
			}

			AVFormatContext *format = avformat_alloc_context();
			if (!format)
			{
				av_freep(&io->buffer);
				avio_context_free(&io);
				return false;
			}
			format->pb = io;
			format->flags |= AVFMT_FLAG_CUSTOM_IO;

			int result = avformat_open_input(&format, nullptr, nullptr, nullptr);
			if (result < 0)
			{
				log_av_error("could not open MPEG-TS input", result);
				if (format)
					avformat_free_context(format);
				format = nullptr;
			}
			else
			{
				blog(LOG_INFO,
					 "[obs-srtla-source] MPEG-TS opened: iformat=%s nb_streams=%u",
					 format->iformat ? format->iformat->name : "?", format->nb_streams);
				result = avformat_find_stream_info(format, nullptr);
				if (result < 0)
				{
					log_av_error("could not read MPEG-TS stream information", result);
				}
				else
				{
					for (unsigned int i = 0; i < format->nb_streams; ++i)
					{
						const AVCodecParameters *p = format->streams[i]->codecpar;
						blog(LOG_INFO,
							 "[obs-srtla-source]   stream %u: type=%d codec=%s w=%d h=%d sr=%d",
							 i, static_cast<int>(p->codec_type),
							 avcodec_get_name(p->codec_id), p->width, p->height,
							 p->sample_rate);
					}
					result = decode(format, stopping);
				}
				avformat_close_input(&format);
			}

			av_freep(&io->buffer);
			avio_context_free(&io);
			return result >= 0 || stopping.load();
		}

	private:
		static void log_av_error(const char *message, int error)
		{
			char error_text[AV_ERROR_MAX_STRING_SIZE] = {};
			av_strerror(error, error_text, sizeof(error_text));
			blog(LOG_WARNING, "[obs-srtla-source] %s: %s", message, error_text);
		}

		// Codec errors can repeat once per packet; only the first is reported.
		static void log_av_error_once(bool &logged, const char *message, int error)
		{
			if (logged)
				return;
			logged = true;
			log_av_error(message, error);
		}

		AVCodecContext *create_decoder(AVFormatContext *format, int stream_index)
		{
			if (stream_index < 0)
				return nullptr;
			AVStream *stream = format->streams[stream_index];
			const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
			if (!codec)
			{
				blog(LOG_WARNING, "[obs-srtla-source] no decoder for stream %d",
					 stream_index);
				return nullptr;
			}
			AVCodecContext *context = avcodec_alloc_context3(codec);
			if (!context)
				return nullptr;
			if (avcodec_parameters_to_context(context, stream->codecpar) < 0 ||
				avcodec_open2(context, codec, nullptr) < 0)
			{
				avcodec_free_context(&context);
				return nullptr;
			}
			return context;
		}

		bool decode(AVFormatContext *format, const std::atomic<bool> &stopping)
		{
			int video_index = -1;
			int audio_index = -1;
			for (unsigned int i = 0; i < format->nb_streams; ++i)
			{
				if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
					video_index < 0)
					video_index = static_cast<int>(i);
				else if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
						 audio_index < 0)
					audio_index = static_cast<int>(i);
			}
			video_codec_ = create_decoder(format, video_index);
			audio_codec_ = create_decoder(format, audio_index);
			blog(LOG_INFO, "[obs-srtla-source] decoder: video=%s audio=%s",
				 video_codec_ ? "ok" : "none", audio_codec_ ? "ok" : "none");
			if (!video_codec_ && !audio_codec_)
			{
				blog(LOG_WARNING, "[obs-srtla-source] stream has no supported audio or video");
				return false;
			}

			std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
			std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
			if (!packet || !frame)
				return false;

			int read_result = 0;
			uint64_t packets = 0;
			while (!stopping.load() && (read_result = av_read_frame(format, packet.get())) >= 0)
			{
				if (++packets % 300 == 1)
					blog(LOG_INFO,
						 "[obs-srtla-source] packets demuxed: %llu (last stream_index=%d size=%d)",
						 static_cast<unsigned long long>(packets), packet->stream_index,
						 packet->size);
				AVCodecContext *codec = packet->stream_index == video_index
											? video_codec_
										: packet->stream_index == audio_index ? audio_codec_
																			  : nullptr;
				int send_result = codec ? avcodec_send_packet(codec, packet.get())
										: AVERROR(EINVAL);
				if (codec && send_result < 0)
					log_av_error_once(codec == video_codec_ ? video_send_logged_
															: audio_send_logged_,
									  "avcodec_send_packet failed", send_result);
				if (codec && send_result >= 0)
				{
					int receive_result = 0;
					while ((receive_result = avcodec_receive_frame(codec, frame.get())) >= 0)
					{
						const AVStream *stream = format->streams[packet->stream_index];
						if (codec == video_codec_)
							output_video(frame.get(), timestamp_ns(
														  frame->best_effort_timestamp,
														  stream->time_base, timestamp_origin_set_,
														  media_origin_ns_, capture_origin_ns_));
						else
							output_audio(frame.get(), timestamp_ns(
														  frame->best_effort_timestamp,
														  stream->time_base, timestamp_origin_set_,
														  media_origin_ns_, capture_origin_ns_));
						av_frame_unref(frame.get());
					}
					if (receive_result != AVERROR(EAGAIN) && receive_result != AVERROR_EOF)
						log_av_error_once(codec == video_codec_ ? video_receive_logged_
																: audio_receive_logged_,
										  "avcodec_receive_frame failed", receive_result);
				}
				av_packet_unref(packet.get());
			}
			if (read_result < 0 && !stopping.load() && read_result != AVERROR_EOF)
				log_av_error("MPEG-TS read failed", read_result);
			blog(LOG_INFO,
				 "[obs-srtla-source] decode loop ended: read_result=%d packets=%llu video=%llu audio=%llu",
				 read_result, static_cast<unsigned long long>(packets),
				 static_cast<unsigned long long>(video_frames_),
				 static_cast<unsigned long long>(audio_frames_));
			return stopping.load() || read_result == AVERROR_EOF;
		}

		// obs_source_frame carries the YUV->RGB matrix the renderer uses, and a
		// zeroed matrix renders black. Recomputed only when the stream changes.
		void update_color_parameters(const AVFrame *decoded, obs_source_frame &frame)
		{
			const video_range_type range = decoded->color_range == AVCOL_RANGE_JPEG
											   ? VIDEO_RANGE_FULL
											   : VIDEO_RANGE_PARTIAL;
			video_colorspace colorspace = VIDEO_CS_DEFAULT;
			switch (decoded->colorspace)
			{
			case AVCOL_SPC_BT709:
				colorspace = VIDEO_CS_709;
				break;
			case AVCOL_SPC_BT470BG:
			case AVCOL_SPC_SMPTE170M:
				colorspace = VIDEO_CS_601;
				break;
			default:
				colorspace = decoded->height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
				break;
			}

			if (!color_parameters_set_ || colorspace != color_space_ ||
				range != color_range_ || frame.format != color_format_)
			{
				if (!video_format_get_parameters_for_format(
						colorspace, range, frame.format, color_matrix_,
						color_range_min_, color_range_max_))
				{
					blog(LOG_WARNING,
						 "[obs-srtla-source] unsupported color parameters: cs=%d range=%d",
						 static_cast<int>(colorspace), static_cast<int>(range));
					return;
				}
				color_parameters_set_ = true;
				color_space_ = colorspace;
				color_range_ = range;
				color_format_ = frame.format;
				blog(LOG_INFO,
					 "[obs-srtla-source] color parameters: cs=%d range=%s format=%d",
					 static_cast<int>(colorspace),
					 range == VIDEO_RANGE_FULL ? "full" : "partial",
					 static_cast<int>(frame.format));
			}

			std::memcpy(frame.color_matrix, color_matrix_, sizeof(frame.color_matrix));
			std::memcpy(frame.color_range_min, color_range_min_,
						sizeof(frame.color_range_min));
			std::memcpy(frame.color_range_max, color_range_max_,
						sizeof(frame.color_range_max));
			frame.full_range = range == VIDEO_RANGE_FULL;
		}

		void output_video(AVFrame *decoded, uint64_t timestamp)
		{
			const AVPixelFormat source_format = static_cast<AVPixelFormat>(decoded->format);
			// Planar 4:2:0 is what OBS wants, so the common case skips swscale.
			const bool passthrough = source_format == AV_PIX_FMT_YUV420P ||
									 source_format == AV_PIX_FMT_YUVJ420P;

			AVFrame *converted = nullptr;
			if (!passthrough)
			{
				scaler_ = sws_getCachedContext(scaler_, decoded->width, decoded->height,
											   source_format, decoded->width,
											   decoded->height, AV_PIX_FMT_YUV420P,
											   SWS_BILINEAR, nullptr, nullptr, nullptr);
				if (!scaler_)
					return;

				converted = av_frame_alloc();
				if (!converted)
					return;
				converted->format = AV_PIX_FMT_YUV420P;
				converted->width = decoded->width;
				converted->height = decoded->height;
				if (av_frame_get_buffer(converted, 32) < 0)
				{
					av_frame_free(&converted);
					return;
				}
				if (sws_scale(scaler_, decoded->data, decoded->linesize, 0,
							  decoded->height, converted->data, converted->linesize) <= 0)
				{
					blog(LOG_WARNING, "[obs-srtla-source] sws_scale produced no output");
					av_frame_free(&converted);
					return;
				}
			}

			const AVFrame *source = converted ? converted : decoded;
			obs_source_frame frame{};
			frame.format = VIDEO_FORMAT_I420;
			frame.width = static_cast<uint32_t>(source->width);
			frame.height = static_cast<uint32_t>(source->height);
			frame.timestamp = timestamp;
			for (int plane = 0; plane < 3; ++plane)
			{
				frame.data[plane] = source->data[plane];
				frame.linesize[plane] = static_cast<uint32_t>(source->linesize[plane]);
			}
			update_color_parameters(decoded, frame);
			obs_source_output_video(source_, &frame);
			if (++video_frames_ % 30 == 1)
				blog(LOG_INFO,
					 "[obs-srtla-source] video frames output: %llu (%ux%u src_fmt=%d %s ts=%llu)",
					 static_cast<unsigned long long>(video_frames_), frame.width,
					 frame.height, decoded->format,
					 passthrough ? "passthrough" : "scaled",
					 static_cast<unsigned long long>(timestamp));
			if (converted)
				av_frame_free(&converted);
		}

		bool ensure_resampler(const AVFrame *frame)
		{
			AVChannelLayout source_layout{};
			if (frame->ch_layout.nb_channels > 0)
			{
				if (av_channel_layout_copy(&source_layout, &frame->ch_layout) < 0)
					return false;
			}
			else
			{
				return false;
			}
			if (source_layout.nb_channels <= 0 || source_layout.nb_channels > MAX_AV_PLANES)
			{
				av_channel_layout_uninit(&source_layout);
				return false;
			}

			const int output_channels = source_layout.nb_channels == 1 ? 1 : 2;
			AVChannelLayout wanted_output{};
			av_channel_layout_default(&wanted_output, output_channels);
			const bool same_layout = resampler_ &&
									 av_channel_layout_compare(&source_layout, &input_layout_) == 0 &&
									 av_channel_layout_compare(&wanted_output, &output_layout_) == 0 &&
									 input_format_ == static_cast<AVSampleFormat>(frame->format) &&
									 input_sample_rate_ == frame->sample_rate;
			if (same_layout)
			{
				av_channel_layout_uninit(&source_layout);
				av_channel_layout_uninit(&wanted_output);
				return true;
			}

			swr_free(&resampler_);
			av_channel_layout_uninit(&input_layout_);
			av_channel_layout_uninit(&output_layout_);
			if (av_channel_layout_copy(&input_layout_, &source_layout) < 0 ||
				av_channel_layout_copy(&output_layout_, &wanted_output) < 0)
			{
				av_channel_layout_uninit(&source_layout);
				av_channel_layout_uninit(&wanted_output);
				return false;
			}
			av_channel_layout_uninit(&source_layout);
			av_channel_layout_uninit(&wanted_output);
			input_format_ = static_cast<AVSampleFormat>(frame->format);
			input_sample_rate_ = frame->sample_rate;
			if (swr_alloc_set_opts2(&resampler_, &output_layout_, AV_SAMPLE_FMT_FLTP,
									frame->sample_rate, &input_layout_, input_format_,
									frame->sample_rate, 0, nullptr) < 0 ||
				swr_init(resampler_) < 0)
			{
				swr_free(&resampler_);
				return false;
			}
			return true;
		}

		void output_audio(AVFrame *decoded, uint64_t timestamp)
		{
			if (!ensure_resampler(decoded))
				return;
			const int channels = output_layout_.nb_channels;
			const int capacity = swr_get_out_samples(resampler_, decoded->nb_samples);
			if (capacity <= 0)
				return;

			uint8_t *planes[MAX_AV_PLANES] = {};
			const uint8_t *input_planes[MAX_AV_PLANES] = {};
			const int input_plane_count =
				av_sample_fmt_is_planar(static_cast<AVSampleFormat>(decoded->format))
					? input_layout_.nb_channels
					: 1;
			if (input_plane_count > MAX_AV_PLANES)
				return;
			for (int i = 0; i < input_plane_count; ++i)
				input_planes[i] = decoded->extended_data[i];
			int line_size = 0;
			if (av_samples_alloc(planes, &line_size, channels, capacity,
								 AV_SAMPLE_FMT_FLTP, 0) < 0)
				return;
			const int samples = swr_convert(resampler_, planes, capacity,
											input_planes, decoded->nb_samples);
			if (samples > 0)
			{
				obs_source_audio audio{};
				audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
				audio.samples_per_sec = static_cast<uint32_t>(decoded->sample_rate);
				audio.speakers = channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
				audio.frames = static_cast<uint32_t>(samples);
				audio.timestamp = timestamp;
				for (int i = 0; i < channels; ++i)
					audio.data[i] = planes[i];
				obs_source_output_audio(source_, &audio);
				if (++audio_frames_ % 100 == 1)
					blog(LOG_INFO,
						 "[obs-srtla-source] audio frames decoded: %llu (ch=%d sr=%d)",
						 static_cast<unsigned long long>(audio_frames_), channels,
						 decoded->sample_rate);
			}
			av_freep(&planes[0]);
		}

		obs_source_t *source_;
		AVCodecContext *video_codec_ = nullptr;
		AVCodecContext *audio_codec_ = nullptr;
		SwsContext *scaler_ = nullptr;
		SwrContext *resampler_ = nullptr;
		AVChannelLayout input_layout_{};
		AVChannelLayout output_layout_{};
		AVSampleFormat input_format_ = AV_SAMPLE_FMT_NONE;
		int input_sample_rate_ = 0;
		bool timestamp_origin_set_ = false;
		int64_t media_origin_ns_ = 0;
		uint64_t capture_origin_ns_ = 0;
		uint64_t video_frames_ = 0;
		uint64_t audio_frames_ = 0;
		float color_matrix_[16] = {};
		float color_range_min_[3] = {};
		float color_range_max_[3] = {};
		video_colorspace color_space_ = VIDEO_CS_DEFAULT;
		video_range_type color_range_ = VIDEO_RANGE_DEFAULT;
		video_format color_format_ = VIDEO_FORMAT_NONE;
		bool color_parameters_set_ = false;
		bool video_send_logged_ = false;
		bool audio_send_logged_ = false;
		bool video_receive_logged_ = false;
		bool audio_receive_logged_ = false;
	};

	class SrtlaInput
	{
	public:
		SrtlaInput(obs_source_t *source, uint16_t port) : source_(source), port_(port) {}

		~SrtlaInput() { stop(); }

		bool start()
		{
			if (thread_.joinable())
				return false;
			stopping_.store(false);
			try
			{
				thread_ = std::thread(&SrtlaInput::run, this);
			}
			catch (const std::system_error &error)
			{
				blog(LOG_ERROR, "[obs-srtla-source] could not start receiver thread: %s",
					 error.what());
				return false;
			}
			return true;
		}

		void stop()
		{
			stopping_.store(true);
			close_socket(active_socket_);
			close_socket(listener_);
			if (thread_.joinable())
				thread_.join();
		}

	private:
		void run()
		{
			SrtRuntime runtime;
			if (!runtime.ready())
			{
				blog(LOG_ERROR, "[obs-srtla-source] libsrt startup failed");
				return;
			}
			const SRTSOCKET listener = srt_create_socket();
			if (listener == SRT_INVALID_SOCK)
			{
				blog(LOG_ERROR, "[obs-srtla-source] could not create local SRT socket");
				return;
			}
			listener_.store(listener);
			int timeout_ms = 250;
			if (srt_setsockflag(listener, SRTO_RCVTIMEO, &timeout_ms,
								sizeof(timeout_ms)) == SRT_ERROR)
			{
				blog(LOG_ERROR, "[obs-srtla-source] could not configure local SRT listener: %s",
					 srt_getlasterror_str());
				close_socket(listener_);
				return;
			}
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = 0;
			if (srt_bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
					SRT_ERROR ||
				srt_listen(listener, 1) == SRT_ERROR)
			{
				blog(LOG_ERROR, "[obs-srtla-source] could not bind local SRT listener: %s",
					 srt_getlasterror_str());
				close_socket(listener_);
				return;
			}
			sockaddr_in bound_address{};
			int address_length = sizeof(bound_address);
			if (srt_getsockname(listener, reinterpret_cast<sockaddr *>(&bound_address),
								&address_length) == SRT_ERROR)
			{
				blog(LOG_ERROR, "[obs-srtla-source] could not read local SRT port: %s",
					 srt_getlasterror_str());
				close_socket(listener_);
				return;
			}

			srtla_embed::EmbeddedSrtlaReceiver receiver;
			if (!receiver.start(port_, ntohs(bound_address.sin_port)))
			{
				close_socket(listener_);
				return;
			}
			blog(LOG_INFO, "[obs-srtla-source] listening for SRTLA on UDP port %u", port_);

			while (!stopping_.load() && receiver.running())
			{
				sockaddr_storage peer{};
				int peer_length = sizeof(peer);
				const SRTSOCKET active_socket =
					srt_accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_length);
				active_socket_.store(active_socket);
				if (active_socket == SRT_INVALID_SOCK)
				{
					const int error = srt_getlasterror(nullptr);
					if (!stopping_.load() && error != SRT_ETIMEOUT &&
						error != SRT_EASYNCRCV)
						blog(LOG_WARNING, "[obs-srtla-source] SRT accept failed: %s",
							 srt_getlasterror_str());
					continue;
				}
				int receive_timeout = 1000;
				if (srt_setsockflag(active_socket, SRTO_RCVTIMEO, &receive_timeout,
									sizeof(receive_timeout)) == SRT_ERROR)
				{
					if (!stopping_.load())
						blog(LOG_WARNING,
							 "[obs-srtla-source] could not configure SRT stream: %s",
							 srt_getlasterror_str());
					close_socket(active_socket_);
					continue;
				}
				MediaDecoder decoder(source_);
				if (!decoder.run(active_socket, stopping_) && !stopping_.load())
					blog(LOG_WARNING, "[obs-srtla-source] could not decode SRT media");
				close_socket(active_socket_);
			}
			if (!stopping_.load() && !receiver.running())
				blog(LOG_ERROR, "[obs-srtla-source] SRTLA receiver stopped unexpectedly");

			receiver.stop();
			close_socket(active_socket_);
			close_socket(listener_);
		}

		obs_source_t *source_;
		uint16_t port_;
		std::atomic<bool> stopping_{false};
		std::atomic<SRTSOCKET> listener_{SRT_INVALID_SOCK};
		std::atomic<SRTSOCKET> active_socket_{SRT_INVALID_SOCK};
		std::thread thread_;
	};

	struct Source
	{
		obs_source_t *source = nullptr;
		uint16_t port = 5000;
		std::unique_ptr<SrtlaInput> input;
	};

	static uint16_t configured_port(obs_data_t *settings)
	{
		const int64_t port = obs_data_get_int(settings, setting_port);
		return port >= 1 && port <= 65535 ? static_cast<uint16_t>(port) : 5000;
	}

	static void start_input(Source *source)
	{
		source->input = std::make_unique<SrtlaInput>(source->source, source->port);
		if (!source->input->start())
			source->input.reset();
	}

	static const char *source_get_name(void *)
	{
		return obs_module_text("SRTLASource");
	}

	static void *source_create(obs_data_t *settings, obs_source_t *obs_source)
	{
		auto source = std::make_unique<Source>();
		source->source = obs_source;
		source->port = configured_port(settings);
		start_input(source.get());
		return source.release();
	}

	static void source_destroy(void *data)
	{
		delete static_cast<Source *>(data);
	}

	static void source_update(void *data, obs_data_t *settings)
	{
		auto *source = static_cast<Source *>(data);
		const uint16_t port = configured_port(settings);
		if (source->port == port)
			return;

		if (source->input)
			source->input->stop();
		source->input.reset();
		source->port = port;
		start_input(source);
	}

	static obs_properties_t *source_properties(void *)
	{
		obs_properties_t *properties = obs_properties_create();
		obs_properties_add_int(properties, setting_port, obs_module_text("Port"), 1, 65535, 1);
		return properties;
	}

	static void source_defaults(obs_data_t *settings)
	{
		obs_data_set_default_int(settings, setting_port, 5000);
	}

	// Designated initializers are C++20; MSVC rejects them in C++17 mode.
	static obs_source_info make_source_info()
	{
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

	static obs_source_info source_info = make_source_info();

} // namespace

extern "C" void srtla_source_register(void)
{
	obs_register_source(&source_info);
}
