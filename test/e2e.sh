#!/usr/bin/env bash
# End-to-end tests for the built plugin.
#
# Each case streams a generated MPEG-TS clip through the real SRTLA path:
#
#   ffmpeg --SRT--> srtla_send --SRTLA/UDP--> plugin (embedded SRTLA receiver
#   -> libsrt -> FFmpeg decode) --> test/harness.cpp (headless OBS host)
#
# and checks what the plugin handed to OBS: frame counts, pixel/sample formats,
# channel layouts, HDR transfer, and how long source update/destroy blocked.
#
# usage: test/e2e.sh [PLUGIN.so] [CASE...]
#   PLUGIN.so  defaults to build_x86_64/obs-srtla-source.so
#   CASE       run only the named cases (see the list at the bottom)
#
# environment:
#   E2E_WORK_DIR   build/cache directory (default: build_e2e in the repo root)
#   E2E_BASE_PORT  first UDP port to use (default: 17000)
#   E2E_KEEP_LOGS  set to 1 to print the plugin log of passing cases too
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="$ROOT_DIR/test"
WORK_DIR="${E2E_WORK_DIR:-$ROOT_DIR/build_e2e}"
BASE_PORT="${E2E_BASE_PORT:-17000}"

PLUGIN="$ROOT_DIR/build_x86_64/obs-srtla-source.so"
if [[ $# -gt 0 && "$1" == *.so ]]; then
	PLUGIN="$(realpath "$1")"
	shift
fi
SELECTED=("$@")

CLIP_SECONDS=6
FPS=30

die() {
	printf 'Error: %s\n' "$*" >&2
	exit 1
}

for tool in ffmpeg g++ git cmake ninja pkg-config; do
	command -v "$tool" >/dev/null 2>&1 || die "$tool is required"
done
pkg-config --exists libobs || die "libobs development files are required"
grep -qw srt <<<"$(ffmpeg -hide_banner -protocols 2>/dev/null)" || die "ffmpeg must be built with SRT support"
encoders="$(ffmpeg -hide_banner -encoders 2>/dev/null)"
for encoder in libx264 libx265 aac ac3 mp2; do
	grep -qw "$encoder" <<<"$encoders" || die "ffmpeg encoder $encoder is required"
done
[[ -f "$PLUGIN" ]] || die "plugin not found: $PLUGIN (build it first with scripts/build.sh)"

mkdir -p "$WORK_DIR/clips" "$WORK_DIR/logs"

# --- harness ---------------------------------------------------------------
HARNESS="$WORK_DIR/harness"
if [[ ! -x "$HARNESS" || "$TEST_DIR/harness.cpp" -nt "$HARNESS" ]]; then
	printf 'Building test harness...\n'
	# -rdynamic lets the plugin bind to the harness's obs_source_output_* overrides.
	# shellcheck disable=SC2046
	g++ -std=c++17 -O2 -rdynamic $(pkg-config --cflags libobs) "$TEST_DIR/harness.cpp" \
		-o "$HARNESS" $(pkg-config --libs libobs) -ldl -pthread
fi

# --- srtla_send (sender side of the protocol, same pinned revision) ---------
SRTLA_GIT_TAG="$(sed -n 's/^set(SRTLA_GIT_TAG "\([0-9a-f]*\)".*/\1/p' "$ROOT_DIR/CMakeLists.txt")"
SENDER_SRC="$WORK_DIR/srtla"
SENDER="$SENDER_SRC/build/srtla_send"
if [[ ! -x "$SENDER" ]]; then
	printf 'Building srtla_send at %s...\n' "${SRTLA_GIT_TAG:0:7}"
	if [[ ! -d "$SENDER_SRC/.git" ]]; then
		git clone --quiet https://github.com/irlserver/srtla.git "$SENDER_SRC"
	fi
	git -C "$SENDER_SRC" checkout --quiet "$SRTLA_GIT_TAG"
	git -C "$SENDER_SRC" submodule update --quiet --init --depth 1 deps/argparse
	spdlog_args=()
	if [[ -d "$ROOT_DIR/build_x86_64/_deps/spdlog-src" ]]; then
		spdlog_args=("-DFETCHCONTENT_SOURCE_DIR_SPDLOG=$ROOT_DIR/build_x86_64/_deps/spdlog-src")
	fi
	# Debug on purpose: upstream srtla_send calls get_ms() inside assert(), so a
	# Release (NDEBUG) build never reads the clock and never registers.
	cmake -S "$SENDER_SRC" -B "$SENDER_SRC/build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
		"${spdlog_args[@]}" >/dev/null
	cmake --build "$SENDER_SRC/build" --target srtla_send >/dev/null
fi
echo 127.0.0.1 >"$WORK_DIR/sender_ips"

# --- clips -----------------------------------------------------------------
make_clip() {
	local name=$1 size=$2 rate=$3
	shift 3
	local clip="$WORK_DIR/clips/$name.ts"
	[[ -f "$clip" ]] && return
	printf 'Generating clip %s...\n' "$name"
	ffmpeg -hide_banner -loglevel error -y \
		-f lavfi -i "testsrc2=size=$size:rate=$FPS" \
		-f lavfi -i "sine=frequency=440:sample_rate=$rate" \
		-t "$CLIP_SECONDS" -g "$FPS" "$@" -f mpegts "$clip"
}

make_clip h264-aac-stereo 1920x1080 48000 \
	-c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -ac 2
make_clip hevc10-hlg-aac51 1920x1080 48000 \
	-c:v libx265 -preset ultrafast -x265-params log-level=error -pix_fmt yuv420p10le \
	-color_trc arib-std-b67 -colorspace bt2020nc -color_primaries bt2020 \
	-c:a aac -af "aformat=channel_layouts=5.1"
make_clip h264-422-mp2-mono 1280x720 48000 \
	-c:v libx264 -preset veryfast -pix_fmt yuv422p -c:a mp2 -ac 1
make_clip hevc444p10-aac-mono 1280x720 44100 \
	-c:v libx265 -preset ultrafast -x265-params log-level=error -pix_fmt yuv444p10le -c:a aac -ac 1
make_clip h264-aac50 640x360 48000 \
	-c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -af "aformat=channel_layouts=5.0"
make_clip h264-ac3-30 640x360 48000 \
	-c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a ac3 -af "aformat=channel_layouts=3.0"

# --- runner ----------------------------------------------------------------
declare -A result
case_index=0
failures=0
passed=0

# Extra SRT URL options for the sender (passphrase, streamid), set per case.
SENDER_SRT_OPTIONS=""

# stream CASE HARNESS_SECONDS "EXTRA HARNESS OPTIONS" CLIP...
stream() {
	local name=$1 seconds=$2 options=$3
	shift 3
	local port=$((BASE_PORT + case_index * 10))
	local sender_port=$((port + 1))
	local log="$WORK_DIR/logs/$name.log"
	case_index=$((case_index + 1))

	# shellcheck disable=SC2086
	"$HARNESS" "$PLUGIN" --port "$port" --seconds "$seconds" $options >"$log" 2>&1 &
	local harness_pid=$!
	for _ in $(seq 50); do
		grep -q "listening for SRTLA on UDP port $port\$" "$log" && break
		sleep 0.1
	done

	"$SENDER" "$sender_port" 127.0.0.1 "$port" "$WORK_DIR/sender_ips" >"$WORK_DIR/logs/$name.sender.log" 2>&1 &
	local sender_pid=$!
	sleep 1
	local clip
	for clip in "$@"; do
		# Fails by design when the source is destroyed mid-stream.
		timeout "$seconds" ffmpeg -hide_banner -loglevel error -re -i "$WORK_DIR/clips/$clip.ts" -c copy \
			-f mpegts "srt://127.0.0.1:$sender_port?pkt_size=1316&latency=200000$SENDER_SRT_OPTIONS" \
			>>"$WORK_DIR/logs/$name.ffmpeg.log" 2>&1 || true
		sleep 1
	done

	local harness_status=0
	wait "$harness_pid" || harness_status=$?
	kill "$sender_pid" 2>/dev/null || true
	wait "$sender_pid" 2>/dev/null || true

	result=()
	result[exit]=$harness_status
	local field
	for field in $(sed -n 's/^RESULT //p' "$log"); do
		result[${field%%=*}]=${field#*=}
	done
	current_case=$name
	current_log=$log
	current_failed=0
	SENDER_SRT_OPTIONS=""
}

# check_log PATTERN: the plugin log must contain a line matching PATTERN.
check_log() {
	if ! grep -q -- "$1" "$current_log"; then
		printf '    log: expected a line matching "%s"\n' "$1"
		current_failed=1
	fi
}

check() {
	local key=$1 op=$2 expected=$3
	local actual=${result[$key]:-missing}
	local ok=0
	case $op in
	=) [[ "$actual" == "$expected" ]] && ok=1 ;;
	'>=') [[ "$actual" =~ ^[0-9.]+$ ]] && awk "BEGIN{exit !($actual >= $expected)}" && ok=1 ;;
	'<=') [[ "$actual" =~ ^[0-9.]+$ ]] && awk "BEGIN{exit !($actual <= $expected)}" && ok=1 ;;
	esac
	if ((!ok)); then
		printf '    %s: expected %s %s, got %s\n' "$key" "$op" "$expected" "$actual"
		current_failed=1
	fi
}

finish() {
	if ((current_failed)); then
		failures=$((failures + 1))
		printf 'FAIL %s (log: %s)\n' "$current_case" "$current_log"
		grep -v ' srtla: ' "$current_log" | sed 's/^/    | /'
	else
		passed=$((passed + 1))
		printf 'PASS %s\n' "$current_case"
		[[ "${E2E_KEEP_LOGS:-0}" == 1 ]] && grep -v ' srtla: ' "$current_log" | sed 's/^/    | /'
	fi
	return 0
}

# Checks shared by every case that streams whole clips.
check_common() {
	local clips=$1
	check exit = 0
	check websocket_failures = 0
	check connected_queries '>=' 1
	check color_matrix = 1
	check video_frames '>=' $((clips * CLIP_SECONDS * FPS * 8 / 10))
	# Source removal must not block the OBS UI thread (libsrt cleanup takes ~1 s).
	check destroy_ms '<=' 500
}

selected() {
	((${#SELECTED[@]} == 0)) && return 0
	local name
	for name in "${SELECTED[@]}"; do
		[[ "$name" == "$1" ]] && return 0
	done
	return 1
}

# --- cases -----------------------------------------------------------------
single_clip_seconds=$((CLIP_SECONDS + 4))
printf 'Testing %s\n' "$PLUGIN"

if selected h264-aac-stereo; then
	stream h264-aac-stereo "$single_clip_seconds" "" h264-aac-stereo
	check_common 1
	check size = 1920x1080
	check video_formats = I420
	check trc = 0
	check audio_formats = floatp
	check channels = 2
	check audio_samples '>=' $((CLIP_SECONDS * 48000 * 8 / 10))
	finish
fi

if selected hevc10-hlg-aac51; then
	stream hevc10-hlg-aac51 "$single_clip_seconds" "" hevc10-hlg-aac51
	check_common 1
	check video_formats = I010
	check trc = 3 # VIDEO_TRC_HLG
	check audio_formats = floatp
	check channels = 6
	finish
fi

if selected h264-422-mp2-mono; then
	stream h264-422-mp2-mono "$single_clip_seconds" "" h264-422-mp2-mono
	check_common 1
	check size = 1280x720
	check video_formats = I422
	check audio_formats = s16p
	check channels = 1
	finish
fi

# yuv444p10 has no OBS equivalent, so it goes through the swscale fallback.
if selected hevc444p10-fallback; then
	stream hevc444p10-fallback "$single_clip_seconds" "" hevc444p10-aac-mono
	check_common 1
	check video_formats = I420
	check audio_formats = floatp
	check channels = 1
	check audio_samples '>=' $((CLIP_SECONDS * 44100 * 8 / 10))
	finish
fi

# OBS has no 5.0 or 3.0 layout, so these are downmixed to stereo.
if selected aac50-downmix; then
	stream aac50-downmix "$single_clip_seconds" "" h264-aac50
	check_common 1
	check audio_formats = floatp
	check channels = 2
	finish
fi

if selected ac3-30-downmix; then
	stream ac3-30-downmix "$single_clip_seconds" "" h264-ac3-30
	check_common 1
	check audio_formats = floatp
	check channels = 2
	finish
fi

# The sender reconnects with a different stream; decoding restarts cleanly.
if selected reconnect; then
	stream reconnect $((2 * CLIP_SECONDS + 6)) "" h264-aac-stereo h264-422-mp2-mono
	check_common 2
	check video_formats = I420,I422
	check channels = 1,2
	finish
fi

# The source is destroyed while a stream is still being decoded.
if selected destroy-mid-stream; then
	stream destroy-mid-stream 4 "" hevc10-hlg-aac51
	check exit = 0
	check video_frames '>=' 1
	check destroy_ms '<=' 500
	finish
fi

# The source starts on another port, is moved by update(), and runs next to
# a second idle source.
if selected port-update-two-sources; then
	stream port-update-two-sources "$single_clip_seconds" \
		"--initial-port $((BASE_PORT + 900)) --extra-source $((BASE_PORT + 901))" h264-aac-stereo
	check_common 1
	check update_ms '<=' 500
	finish
fi

if selected websocket-unavailable; then
	stream websocket-unavailable "$single_clip_seconds" "--websocket 0" h264-aac-stereo
	check exit = 0
	check websocket_failures = 0
	check connected_queries = 0
	check video_frames '>=' $((CLIP_SECONDS * FPS * 8 / 10))
	check destroy_ms '<=' 500
	check_log 'obs-websocket unavailable'
	finish
fi

# Security: SRT AES passphrase and required stream ID.
passphrase="e2e-secret-passphrase"
streamid="live/e2e-publish-key"

# The sender leaves the key length unset, as most SRTLA senders do; the
# source's default (AES-256) is used.
if selected passphrase; then
	SENDER_SRT_OPTIONS="&passphrase=$passphrase"
	stream passphrase "$single_clip_seconds" "--passphrase $passphrase" h264-aac-stereo
	check_common 1
	check_log 'SRT stream connected (AES-256 encrypted)'
	finish
fi

# Sender and source ask for different key lengths; SRT settles on one.
if selected passphrase-pbkeylen-mismatch; then
	SENDER_SRT_OPTIONS="&passphrase=$passphrase&pbkeylen=16"
	stream passphrase-pbkeylen-mismatch "$single_clip_seconds" "--passphrase $passphrase --pbkeylen 32" \
		h264-aac-stereo
	check_common 1
	check_log 'SRT stream connected (AES-[0-9]* encrypted)'
	finish
fi

if selected passphrase-wrong; then
	SENDER_SRT_OPTIONS="&passphrase=wrong-passphrase"
	stream passphrase-wrong 5 "--passphrase $passphrase" h264-aac-stereo
	check exit = 0
	check video_frames = 0
	check_log 'SRT connection rejected before established'
	finish
fi

# An unencrypted sender must not get through to an encrypted source.
if selected passphrase-missing; then
	stream passphrase-missing 5 "--passphrase $passphrase" h264-aac-stereo
	check exit = 0
	check video_frames = 0
	finish
fi

if selected passphrase-invalid; then
	stream passphrase-invalid 3 "--passphrase short" h264-aac-stereo
	check exit = 0
	check video_frames = 0
	check_log 'SRT passphrase must be 10 to 79 characters'
	finish
fi

if selected streamid; then
	SENDER_SRT_OPTIONS="&passphrase=$passphrase&streamid=$streamid"
	stream streamid "$single_clip_seconds" "--passphrase $passphrase --streamid $streamid" h264-aac-stereo
	check_common 1
	finish
fi

if selected streamid-wrong; then
	SENDER_SRT_OPTIONS="&streamid=live/other-key"
	stream streamid-wrong 5 "--streamid $streamid" h264-aac-stereo
	check exit = 0
	check video_frames = 0
	check_log 'rejected SRT stream: stream ID "live/other-key"'
	finish
fi

printf '\n%d passed, %d failed\n' "$passed" "$failures"
((failures == 0))
