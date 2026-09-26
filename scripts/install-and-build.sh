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
	libsrt-openssl-dev
)

printf 'Refreshing package indexes...\n'
if ! "${SUDO[@]}" apt-get update; then
	printf 'Warning: apt-get update failed; trying the currently available package indexes.\n' >&2
fi

printf 'Installing build dependencies...\n'
"${SUDO[@]}" apt-get install -y "${packages[@]}"

SRTLA_SOURCE="$ROOT_DIR/.deps/srtla"
SRTLA_BUILD="$ROOT_DIR/build_x86_64/srtla-helper"
SRTLA_INSTALL="$ROOT_DIR/build_x86_64/runtime"
SRTLA_REPOSITORY="https://github.com/irlserver/srtla.git"
SRTLA_REVISION="b8359bc80ce99ac4fc8f603cfa455cdd0a0a4241"

if [[ ! -e "$SRTLA_SOURCE" ]]; then
	printf 'Fetching the SRTLA helper source at %s...\n' "$SRTLA_REVISION"
	mkdir -p "$(dirname -- "$SRTLA_SOURCE")"
	git clone --recurse-submodules "$SRTLA_REPOSITORY" "$SRTLA_SOURCE"
	git -C "$SRTLA_SOURCE" checkout --detach "$SRTLA_REVISION"
	git -C "$SRTLA_SOURCE" submodule update --init --recursive
elif [[ ! -d "$SRTLA_SOURCE/.git" ]]; then
	printf 'Error: %s already exists but is not an SRTLA git checkout; refusing to overwrite it.\n' \
		"$SRTLA_SOURCE" >&2
	exit 1
else
	checkout_revision="$(git -C "$SRTLA_SOURCE" rev-parse HEAD)"
	if [[ "$checkout_revision" != "$SRTLA_REVISION" ]]; then
		printf 'Error: existing SRTLA checkout is at %s; expected pinned revision %s.\n' \
			"$checkout_revision" "$SRTLA_REVISION" >&2
		printf 'Move the checkout aside or check out the pinned revision, then retry.\n' >&2
		exit 1
	fi
	printf 'Using existing SRTLA source checkout at %s.\n' "$SRTLA_SOURCE"
fi

printf 'Building srtla_rec...\n'
cmake -S "$SRTLA_SOURCE" -B "$SRTLA_BUILD" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$SRTLA_INSTALL"
cmake --build "$SRTLA_BUILD" --target srtla_rec

helper="$SRTLA_INSTALL/bin/srtla_rec"
install -D -m 0755 "$SRTLA_BUILD/srtla_rec" "$helper"

printf 'Building the OBS plugin...\n'
"$ROOT_DIR/scripts/build.sh"

printf '\nSetup complete.\n'
printf 'Plugin: %s\n' "$ROOT_DIR/build_x86_64/obs-srtla-source.so"
printf 'srtla_rec: %s\n' "$helper"
printf 'Enter the full srtla_rec path above in the OBS source properties.\n'
printf 'The upstream SRTLA helper is licensed under AGPL-3.0; see its LICENSE file in %s.\n' \
	"$SRTLA_SOURCE"
