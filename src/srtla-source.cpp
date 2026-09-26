#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <obs-module.h>
#include <util/platform.h>

extern "C" {
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

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace {

constexpr char setting_port[] = "srtla_port";
constexpr char setting_executable[] = "srtla_executable";
constexpr int avio_buffer_size = 32 * 1024;

class SrtRuntime {
public:
	SrtRuntime()
	{
		std::lock_guard<std::mutex> lock(runtime_mutex());
		if (runtime_references() == 0 && srt_startup() == SRT_ERROR)
			return;
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

class ChildProcess {
public:
	bool start(const std::string &executable, uint16_t srtla_port, uint16_t srt_port)
	{
		const std::string port_arg = std::to_string(srtla_port);
		const std::string srt_port_arg = std::to_string(srt_port);
		std::vector<std::string> arguments = {
			executable, "--srtla_port", port_arg, "--srt_hostname", "127.0.0.1",
			"--srt_port", srt_port_arg,
		};

#ifdef _WIN32
		std::wstring command_line;
		for (const std::string &argument : arguments) {
			const std::wstring wide_argument = to_wide(argument);
			if (wide_argument.empty()) {
				blog(LOG_ERROR, "[obs-srtla-source] executable path is not valid UTF-8");
				return false;
			}
			if (!command_line.empty())
				command_line.push_back(L' ');
			command_line += quote_windows_argument(wide_argument);
		}
		STARTUPINFOW startup_info{};
		startup_info.cb = sizeof(startup_info);
		PROCESS_INFORMATION process_info{};
		if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
				    CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info)) {
			blog(LOG_ERROR, "[obs-srtla-source] could not start srtla_rec (Windows error %lu)",
			     GetLastError());
			return false;
		}
		CloseHandle(process_info.hThread);
		process_ = process_info.hProcess;
#else
		std::vector<char *> argv;
		argv.reserve(arguments.size() + 1);
		for (std::string &argument : arguments)
			argv.push_back(argument.data());
		argv.push_back(nullptr);

		pid_t child = -1;
		const int error = posix_spawnp(&child, executable.c_str(), nullptr, nullptr,
					       argv.data(), environ);
		if (error != 0) {
			blog(LOG_ERROR, "[obs-srtla-source] could not start '%s': %s",
			     executable.c_str(), strerror(error));
			return false;
		}
		process_ = child;
#endif
		return true;
	}

	bool running()
	{
#ifdef _WIN32
		return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
		if (process_ <= 0)
			return false;
		int status = 0;
		const pid_t result = waitpid(process_, &status, WNOHANG);
		if (result == 0)
			return true;
		if (result < 0 && errno == EINTR)
			return true;
		if (result < 0)
			blog(LOG_WARNING, "[obs-srtla-source] could not check srtla_rec process: %s",
			     strerror(errno));
		process_ = -1;
		return false;
#endif
	}

	void stop()
	{
#ifdef _WIN32
		if (!process_)
			return;
		if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
			TerminateProcess(process_, 0);
			WaitForSingleObject(process_, 1000);
		}
		CloseHandle(process_);
		process_ = nullptr;
#else
		if (process_ <= 0)
			return;
		if (running()) {
			kill(process_, SIGTERM);
			for (int attempt = 0; attempt < 20; ++attempt) {
				if (!running())
					return;
				os_sleep_ms(50);
			}
			kill(process_, SIGKILL);
			while (waitpid(process_, nullptr, 0) == -1 && errno == EINTR) {
			}
		}
		process_ = -1;
#endif
	}

	~ChildProcess() { stop(); }

	ChildProcess() = default;
	ChildProcess(const ChildProcess &) = delete;
	ChildProcess &operator=(const ChildProcess &) = delete;

private:
#ifdef _WIN32
	static std::wstring to_wide(const std::string &value)
	{
		const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(),
						     -1, nullptr, 0);
		if (size <= 0)
			return {};
		std::wstring result(static_cast<size_t>(size), L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1,
				    result.data(), size);
		result.pop_back();
		return result;
	}

	static std::wstring quote_windows_argument(const std::wstring &value)
	{
		std::wstring result = L"\"";
		size_t backslashes = 0;
		for (wchar_t character : value) {
			if (character == L'\\') {
				++backslashes;
			} else if (character == L'"') {
				result.append(backslashes * 2 + 1, L'\\');
				result.push_back(character);
				backslashes = 0;
			} else {
				result.append(backslashes, L'\\');
				backslashes = 0;
				result.push_back(character);
			}
		}
		result.append(backslashes * 2, L'\\');
		result.push_back(L'"');
		return result;
	}

	HANDLE process_ = nullptr;
#else
	pid_t process_ = -1;
#endif
};

static void close_socket(SRTSOCKET &socket)
{
	if (socket != SRT_INVALID_SOCK) {
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

struct AvioReader {
	SRTSOCKET socket = SRT_INVALID_SOCK;
	const std::atomic<bool> *stopping = nullptr;
};

static int read_srt(void *opaque, uint8_t *buffer, int buffer_size)
{
	auto *reader = static_cast<AvioReader *>(opaque);
	while (!reader->stopping->load()) {
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

struct PacketDeleter {
	void operator()(AVPacket *packet) const { av_packet_free(&packet); }
};

struct FrameDeleter {
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
	if (!origin_set) {
		origin_set = true;
		media_origin_ns = media_time_ns;
		capture_origin_ns = os_gettime_ns();
	}
	const int64_t elapsed_ns = media_time_ns - media_origin_ns;
	return elapsed_ns < 0 ? capture_origin_ns
			      : capture_origin_ns + static_cast<uint64_t>(elapsed_ns);
}

class MediaDecoder {
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
		if (!io) {
			av_free(buffer);
			return false;
		}

		AVFormatContext *format = avformat_alloc_context();
		if (!format) {
			av_freep(&io->buffer);
			avio_context_free(&io);
			return false;
		}
		format->pb = io;
		format->flags |= AVFMT_FLAG_CUSTOM_IO;

		int result = avformat_open_input(&format, nullptr, nullptr, nullptr);
		if (result < 0) {
			log_av_error("could not open MPEG-TS input", result);
			if (format)
				avformat_free_context(format);
			format = nullptr;
		} else {
			result = avformat_find_stream_info(format, nullptr);
			if (result < 0) {
				log_av_error("could not read MPEG-TS stream information", result);
			} else {
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

	AVCodecContext *create_decoder(AVFormatContext *format, int stream_index)
	{
		if (stream_index < 0)
			return nullptr;
		AVStream *stream = format->streams[stream_index];
		const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
		if (!codec) {
			blog(LOG_WARNING, "[obs-srtla-source] no decoder for stream %d",
			     stream_index);
			return nullptr;
		}
		AVCodecContext *context = avcodec_alloc_context3(codec);
		if (!context)
			return nullptr;
		if (avcodec_parameters_to_context(context, stream->codecpar) < 0 ||
		    avcodec_open2(context, codec, nullptr) < 0) {
			avcodec_free_context(&context);
			return nullptr;
		}
		return context;
	}

	bool decode(AVFormatContext *format, const std::atomic<bool> &stopping)
	{
		int video_index = -1;
		int audio_index = -1;
		for (unsigned int i = 0; i < format->nb_streams; ++i) {
			if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
			    video_index < 0)
				video_index = static_cast<int>(i);
			else if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
				 audio_index < 0)
				audio_index = static_cast<int>(i);
		}
		video_codec_ = create_decoder(format, video_index);
		audio_codec_ = create_decoder(format, audio_index);
		if (!video_codec_ && !audio_codec_) {
			blog(LOG_WARNING, "[obs-srtla-source] stream has no supported audio or video");
			return false;
		}

		std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
		std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
		if (!packet || !frame)
			return false;

		int read_result = 0;
		while (!stopping.load() && (read_result = av_read_frame(format, packet.get())) >= 0) {
			AVCodecContext *codec = packet->stream_index == video_index
							? video_codec_
							: packet->stream_index == audio_index ? audio_codec_ : nullptr;
			if (codec && avcodec_send_packet(codec, packet.get()) >= 0) {
				while (avcodec_receive_frame(codec, frame.get()) >= 0) {
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
			}
			av_packet_unref(packet.get());
		}
		if (read_result < 0 && !stopping.load() && read_result != AVERROR_EOF)
			log_av_error("MPEG-TS read failed", read_result);
		return stopping.load() || read_result == AVERROR_EOF;
	}

	void output_video(AVFrame *decoded, uint64_t timestamp)
	{
		scaler_ = sws_getCachedContext(scaler_, decoded->width, decoded->height,
					       static_cast<AVPixelFormat>(decoded->format),
					       decoded->width, decoded->height, AV_PIX_FMT_NV12,
					       SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!scaler_)
			return;

		AVFrame *converted = av_frame_alloc();
		if (!converted)
			return;
		converted->format = AV_PIX_FMT_NV12;
		converted->width = decoded->width;
		converted->height = decoded->height;
		if (av_frame_get_buffer(converted, 32) < 0) {
			av_frame_free(&converted);
			return;
		}
		if (sws_scale(scaler_, decoded->data, decoded->linesize, 0, decoded->height,
			      converted->data, converted->linesize) <= 0) {
			av_frame_free(&converted);
			return;
		}

		obs_source_frame frame{};
		frame.format = VIDEO_FORMAT_NV12;
		frame.width = static_cast<uint32_t>(converted->width);
		frame.height = static_cast<uint32_t>(converted->height);
		frame.timestamp = timestamp;
		frame.data[0] = converted->data[0];
		frame.data[1] = converted->data[1];
		frame.linesize[0] = static_cast<uint32_t>(converted->linesize[0]);
		frame.linesize[1] = static_cast<uint32_t>(converted->linesize[1]);
		obs_source_output_video(source_, &frame);
		av_frame_free(&converted);
	}

	bool ensure_resampler(const AVFrame *frame)
	{
		AVChannelLayout source_layout{};
		if (frame->ch_layout.nb_channels > 0) {
			if (av_channel_layout_copy(&source_layout, &frame->ch_layout) < 0)
				return false;
		} else {
			return false;
		}
		if (source_layout.nb_channels <= 0 || source_layout.nb_channels > MAX_AV_PLANES) {
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
		if (same_layout) {
			av_channel_layout_uninit(&source_layout);
			av_channel_layout_uninit(&wanted_output);
			return true;
		}

		swr_free(&resampler_);
		av_channel_layout_uninit(&input_layout_);
		av_channel_layout_uninit(&output_layout_);
		if (av_channel_layout_copy(&input_layout_, &source_layout) < 0 ||
		    av_channel_layout_copy(&output_layout_, &wanted_output) < 0) {
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
		    swr_init(resampler_) < 0) {
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
		if (samples > 0) {
			obs_source_audio audio{};
			audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
			audio.samples_per_sec = static_cast<uint32_t>(decoded->sample_rate);
			audio.speakers = channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
			audio.frames = static_cast<uint32_t>(samples);
			audio.timestamp = timestamp;
			for (int i = 0; i < channels; ++i)
				audio.data[i] = planes[i];
			obs_source_output_audio(source_, &audio);
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
};

class SrtlaInput {
public:
	SrtlaInput(obs_source_t *source, uint16_t port, std::string executable)
		: source_(source), port_(port), executable_(std::move(executable))
	{
	}

	~SrtlaInput() { stop(); }

	bool start()
	{
		if (thread_.joinable())
			return false;
		stopping_.store(false);
		try {
			thread_ = std::thread(&SrtlaInput::run, this);
		} catch (const std::system_error &error) {
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
		if (!runtime.ready()) {
			blog(LOG_ERROR, "[obs-srtla-source] libsrt startup failed");
			return;
		}
		const SRTSOCKET listener = srt_create_socket();
		if (listener == SRT_INVALID_SOCK) {
			blog(LOG_ERROR, "[obs-srtla-source] could not create local SRT socket");
			return;
		}
		listener_.store(listener);
		int timeout_ms = 250;
		if (srt_setsockflag(listener, SRTO_RCVTIMEO, &timeout_ms,
				    sizeof(timeout_ms)) == SRT_ERROR) {
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
		    srt_listen(listener, 1) == SRT_ERROR) {
			blog(LOG_ERROR, "[obs-srtla-source] could not bind local SRT listener: %s",
			     srt_getlasterror_str());
			close_socket(listener_);
			return;
		}
		sockaddr_in bound_address{};
		int address_length = sizeof(bound_address);
		if (srt_getsockname(listener, reinterpret_cast<sockaddr *>(&bound_address),
				    &address_length) == SRT_ERROR) {
			blog(LOG_ERROR, "[obs-srtla-source] could not read local SRT port: %s",
			     srt_getlasterror_str());
			close_socket(listener_);
			return;
		}

		ChildProcess helper;
		if (!helper.start(executable_, port_, ntohs(bound_address.sin_port))) {
			close_socket(listener_);
			return;
		}
		blog(LOG_INFO, "[obs-srtla-source] listening for SRTLA on UDP port %u", port_);

		while (!stopping_.load() && helper.running()) {
			sockaddr_storage peer{};
			int peer_length = sizeof(peer);
			const SRTSOCKET active_socket =
				srt_accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_length);
			active_socket_.store(active_socket);
			if (active_socket == SRT_INVALID_SOCK) {
				const int error = srt_getlasterror(nullptr);
				if (!stopping_.load() && error != SRT_ETIMEOUT &&
				    error != SRT_EASYNCRCV)
					blog(LOG_WARNING, "[obs-srtla-source] SRT accept failed: %s",
					     srt_getlasterror_str());
				continue;
			}
			int receive_timeout = 1000;
			if (srt_setsockflag(active_socket, SRTO_RCVTIMEO, &receive_timeout,
					    sizeof(receive_timeout)) == SRT_ERROR) {
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
		if (!stopping_.load() && !helper.running())
			blog(LOG_ERROR, "[obs-srtla-source] srtla_rec exited unexpectedly");

		helper.stop();
		close_socket(active_socket_);
		close_socket(listener_);
	}

	obs_source_t *source_;
	uint16_t port_;
	std::string executable_;
	std::atomic<bool> stopping_{false};
	std::atomic<SRTSOCKET> listener_{SRT_INVALID_SOCK};
	std::atomic<SRTSOCKET> active_socket_{SRT_INVALID_SOCK};
	std::thread thread_;
};

struct Source {
	obs_source_t *source = nullptr;
	uint16_t port = 5000;
	std::string executable = "srtla_rec";
	std::unique_ptr<SrtlaInput> input;
};

static uint16_t configured_port(obs_data_t *settings)
{
	const int64_t port = obs_data_get_int(settings, setting_port);
	return port >= 1 && port <= 65535 ? static_cast<uint16_t>(port) : 5000;
}

static std::string configured_executable(obs_data_t *settings)
{
	const char *value = obs_data_get_string(settings, setting_executable);
	return value && value[0] ? value : "srtla_rec";
}

static void start_input(Source *source)
{
	source->input = std::make_unique<SrtlaInput>(source->source, source->port,
						     source->executable);
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
	source->executable = configured_executable(settings);
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
	const std::string executable = configured_executable(settings);
	if (source->port == port && source->executable == executable)
		return;

	if (source->input)
		source->input->stop();
	source->input.reset();
	source->port = port;
	source->executable = executable;
	start_input(source);
}

static obs_properties_t *source_properties(void *)
{
	obs_properties_t *properties = obs_properties_create();
	obs_properties_add_int(properties, setting_port, obs_module_text("Port"), 1, 65535, 1);
	obs_properties_add_text(properties, setting_executable,
				obs_module_text("SRTLAExecutable"), OBS_TEXT_DEFAULT);
	return properties;
}

static void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, setting_port, 5000);
	obs_data_set_default_string(settings, setting_executable, "srtla_rec");
}

static obs_source_info source_info = {
	.id = "srtla_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = source_get_name,
	.create = source_create,
	.destroy = source_destroy,
	.get_defaults = source_defaults,
	.get_properties = source_properties,
	.update = source_update,
};

} // namespace

extern "C" void srtla_source_register(void)
{
	obs_register_source(&source_info);
}
