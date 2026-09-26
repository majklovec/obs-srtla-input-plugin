#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ ! -r /etc/os-release ]] ||
	! grep -Eq '^(ID=(ubuntu|pop)|ID_LIKE=.*(ubuntu|debian))' /etc/os-release; then
	printf 'Error: this setup script supports Ubuntu and Pop!_OS only.\n' >&2
	exit 1
fi

if [[ "$(id -u)" -eq 0 ]]; then
	SUDO=()
elif command -v sudo >/dev/null 2>&1; then
	SUDO=(sudo)
else
	printf 'Error: install sudo or run this script as root to install packages.\n' >&2
	exit 1
fi

packages=(
	build-essential
	cmake
	git
	ninja-build
	pkg-config
	libobs-dev
	libavformat-dev
	libavcodec-dev
	libavutil-dev
	libswresample-dev
	libswscale-dev
	libssl-dev
)

printf 'Refreshing package indexes...\n'
if ! "${SUDO[@]}" apt-get update; then
	printf 'Warning: apt-get update failed; trying the currently available package indexes.\n' >&2
fi

printf 'Installing build dependencies...\n'
"${SUDO[@]}" apt-get install -y "${packages[@]}"

printf 'Building the OBS plugin...\n'
"$ROOT_DIR/scripts/build.sh"

printf '\nSetup complete.\n'
printf 'Plugin: %s\n' "$ROOT_DIR/build_x86_64/obs-srtla-source.so"
printf 'The SRTLA receiver and libsrt are compiled into the plugin; no srtla_rec helper or system libsrt is needed.\n'
printf 'The embedded irlserver/srtla code is AGPL-3.0; see README.md (License).\n'
