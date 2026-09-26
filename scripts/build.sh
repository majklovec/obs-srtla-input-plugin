#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if ! command -v cmake >/dev/null 2>&1; then
	printf 'Error: CMake is not installed.\n' >&2
	exit 1
fi

if ! command -v ninja >/dev/null 2>&1; then
	printf 'Error: Ninja is not installed. Run scripts/install-and-build.sh first.\n' >&2
	exit 1
fi

if ! command -v pkg-config >/dev/null 2>&1; then
	printf 'Error: pkg-config is not installed. Run scripts/install-and-build.sh first.\n' >&2
	exit 1
fi

if ! cmake --version | head -n 1 | grep -Eq 'version ([4-9]\.|3\.(2[8-9]|[3-9][0-9])\.)'; then
	printf 'Error: CMake 3.28 or newer is required.\n' >&2
	exit 1
fi

missing=()
for package in libobs libavformat libavcodec libavutil libswresample libswscale srt; do
	if ! pkg-config --exists "$package"; then
		missing+=("$package")
	fi
done

if ((${#missing[@]} > 0)); then
	printf 'Error: missing development packages: %s\n' "${missing[*]}" >&2
	printf 'Run scripts/install-and-build.sh to install them.\n' >&2
	exit 1
fi

cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64

plugin="$ROOT_DIR/build_x86_64/obs-srtla-source.so"
if [[ ! -f "$plugin" ]]; then
	printf 'Error: build completed without producing %s\n' "$plugin" >&2
	exit 1
fi

printf '\nBuild complete: %s\n' "$plugin"
