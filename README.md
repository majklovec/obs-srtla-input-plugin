# OBS SRTLA Source

An OBS input source that receives bonded SRTLA streams and outputs decoded
video and audio to OBS. It builds for Linux and Windows (x64).

## Architecture

Everything runs inside the OBS process; the plugin does not start any external
program.

- **Embedded SRTLA receiver.** The receiver from
  [irlserver/srtla](https://github.com/irlserver/srtla) (pinned revision
  `b8359bc`) is compiled into the plugin as a static library, together with
  header-only spdlog. Its event loop runs on a plugin thread, and its log
  output goes to the OBS log with the prefix `[obs-srtla-source] srtla:`.
  Upstream targets Linux (BSD sockets, `epoll`, `recvmmsg`); the patch in
  `cmake/patches/srtla-portable-sockets.patch`, applied when the sources are
  fetched, routes its socket calls through `src/compat/srtla_platform.h`,
  which maps to Winsock plus [wepoll](https://github.com/piscisaureus/wepoll)
  on Windows and to the native calls on Linux.
- **libsrt.** On Linux, Haivision libsrt (release `v1.5.7`) is built from
  source as a static library and linked into the plugin, together with static
  OpenSSL `libcrypto` for SRT encryption support. Their symbols are kept
  private to the plugin, so they do not clash with the system libsrt that OBS's
  own FFmpeg may load into the same process. On Windows the plugin links
  against the `srt.dll` that OBS Studio itself ships (from obs-deps); DLLs
  have separate symbol namespaces, so no clash is possible there. libsrt
  messages appear in the OBS log with the prefix `[obs-srtla-source] libsrt:`.
- **SRT + decode pipeline.** Each source opens a libsrt listener on an
  ephemeral loopback port. The embedded receiver accepts the bonded SRTLA
  links on the configured UDP port and forwards the reassembled SRT traffic
  to that listener. FFmpeg reads the SRT payload as MPEG-TS, demuxes it,
  decodes H.264/AAC, converts video to NV12 and audio to planar float, and
  sends timestamped frames to OBS.

At runtime the plugin needs only libraries that OBS itself already provides:
libobs, FFmpeg (`libavformat`, `libavcodec`, `libavutil`, `libswresample` and
`libswscale`), the C/C++ runtime and, on Windows, OBS's `srt.dll`. It does not
need a system libsrt, libssl/libcrypto, or `srtla_rec`.

## Build on Linux

On Ubuntu or Pop!_OS, run the setup script. It installs the compiler, CMake,
Ninja, git and the OBS, FFmpeg and OpenSSL development packages, then builds
the plugin:

```sh
./scripts/install-and-build.sh
```

The script requires `sudo` for system package installation. On the first
configure, CMake downloads the pinned SRTLA, spdlog and libsrt sources into
`build_x86_64` (`_deps/` and `srt_external-prefix/`), so network access is
needed once. For offline builds of SRTLA and spdlog, pass local checkouts
with `-DFETCHCONTENT_SOURCE_DIR_SRTLA=/path/to/srtla` and
`-DFETCHCONTENT_SOURCE_DIR_SPDLOG=/path/to/spdlog`. libsrt is always fetched
from its git repository at the `SRT_GIT_TAG` release.

To build the plugin again after setup, use:

```sh
./scripts/build.sh
```

The plugin is written to `build_x86_64/obs-srtla-source.so`. This single
module contains the SRTLA receiver and libsrt. To confirm that it is
self-contained, check that no libsrt, libssl or libcrypto library appears in
its dependencies:

```sh
readelf -d build_x86_64/obs-srtla-source.so | grep NEEDED
```

The preset builds in `Release` mode and strips the module after linking, so
the output is a production binary of about 6 MB with no debug info. To keep
debug symbols, configure with `-DSTRIP_PLUGIN=OFF`, or use a
`RelWithDebInfo` build, which is never stripped.

## Build on Windows

Prerequisites (64-bit Windows 10/11):

- Visual Studio 2022 or the Build Tools for Visual Studio 2022 with the
  **Desktop development with C++** workload (MSVC v143 and a Windows 10 SDK
  10.0.20348 or newer).
- CMake 3.28 or newer (`winget install Kitware.CMake`; the copy bundled with
  Visual Studio is also picked up).
- Git (`winget install Git.Git`).
- NSIS 3 for the installer (`winget install NSIS.NSIS`).

Then, from a PowerShell prompt in the repository:

```powershell
.\scripts\build-windows.ps1 -Installer
```

The script checks the prerequisites, runs `cmake --preset windows-x64`,
builds the `Release` configuration and the `installer` target. The first
configure downloads into `.deps\` the pre-built OBS dependencies (obs-deps,
which provide FFmpeg and libsrt), the Qt package the OBS build system
expects, and the OBS Studio sources listed in `buildspec.json`, and builds
libobs from them (SHA-256 checked; a few hundred MB and a few minutes, once).
The SRTLA, spdlog and wepoll sources are fetched with Git as on Linux.

Outputs:

- `build_x64\Release\obs-srtla-source.dll` — the plugin module.
- `release\obs-srtla-source-<version>-windows-x64-installer.exe` — the NSIS
  installer (with `-Installer`).

The same steps without the script:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
cmake --build build_x64 --config Release --target installer
```

`cmake --install build_x64 --config Release --prefix <dir>` stages the
plugin laid out like an OBS Studio directory (`obs-plugins\64bit\` and
`data\obs-plugins\obs-srtla-source\`), which is what the installer packages.
Debug symbols are an opt-in component: add `--component DebugSymbols`.

### Install into OBS on Windows

Run the installer. It requires administrator rights, refuses to continue while
`obs64.exe` is running, and proposes the OBS Studio folder recorded in the
registry by the OBS installer (default `C:\Program Files\obs-studio`); any
folder containing `bin\64bit\obs64.exe` can be selected, including a portable
OBS. It installs:

- `<OBS>\obs-plugins\64bit\obs-srtla-source.dll`
- `<OBS>\data\obs-plugins\obs-srtla-source\locale\en-US.ini`
- `<OBS>\data\obs-plugins\obs-srtla-source\licenses\*.txt` (third-party
  license notices) and the uninstaller.

It registers under *Apps & features* as "OBS SRTLA Source (OBS Studio
plugin)"; `/S` performs a silent install (`/D=<OBS dir>` overrides the
folder). To remove the plugin use *Apps & features* or run the uninstaller in
the plugin data folder.

Without the installer, `.\scripts\build-windows.ps1 -InstallToObs` (from an
elevated prompt, OBS closed) copies the build output into the OBS folder
found in the registry, or into `-ObsDir <folder>`.

The receiver listens on a UDP port; allow `obs64.exe` through the Windows
firewall for inbound UDP on that port, or the SRTLA sender cannot reach it.

## Install into OBS on Linux

These steps are for a native Linux OBS installation. Flatpak and Snap builds
are sandboxed and need a package-specific plugin installation method.

After building, install the module and its locale file into the per-user OBS
plugin directory:

```sh
PLUGIN_DIR="$HOME/.config/obs-studio/plugins/obs-srtla-source"
install -Dm755 build_x86_64/obs-srtla-source.so \
  "$PLUGIN_DIR/bin/64bit/obs-srtla-source.so"
install -Dm644 data/locale/en-US.ini \
  "$PLUGIN_DIR/data/locale/en-US.ini"
```

Close and restart OBS after installing or updating the plugin. To remove this
user-local installation, close OBS and remove
`~/.config/obs-studio/plugins/obs-srtla-source`.

## Check that the plugin loaded

1. In OBS, open **Help → Log Files → View Current Log**.
2. Search the current log for `obs-srtla-source`. A successful load includes
   `plugin loaded`; a load failure is reported with a module-loading error.
3. In the **Sources** dock, click **+** and check that **SRTLA Source** appears
   in the input sources list. Add it to a scene to open its properties.
4. Set **SRTLA Listen Port (UDP)** to the port your SRTLA sender targets, and
   make sure that UDP port is reachable (firewall, port forwarding).
5. Check the OBS log again after adding the source. It should contain
   `listening for SRTLA on UDP port <port>`. If the port is already in use,
   the log shows `could not bind SRTLA UDP port` instead.
6. Start the SRTLA sender. The log should show `srtla: ... Group registered`
   and `Connection registration` for each bonded link. A single
   `Connection registration failed: No group found` line right before this
   is normal, because senders probe with REG2 before registering. Confirm
   decoded audio meters or video in the preview.

If OBS does not load the module, first make sure OBS is fully restarted and
that the plugin was built for the same architecture and compatible native OBS
installation. On Linux, inspect unresolved shared-library dependencies with:

```sh
ldd build_x86_64/obs-srtla-source.so
```

Every listed dependency should resolve to a library path rather than `not
found`. On Windows the module depends on `obs.dll`, the `av*-*.dll`/`sw*-*.dll`
FFmpeg libraries and `srt.dll` from OBS's `bin\64bit` folder; the plugin must
be built against the same OBS major version that loads it (`buildspec.json`
pins the OBS sources used for libobs).

## Source settings

- **SRTLA Listen Port (UDP):** port exposed to the SRTLA sender; defaults to
  `5000`. Each SRTLA source needs its own port.

Changing the port stops the current receiver and starts it again. The plugin
stops its receiver and worker threads when the source is removed.

## License

The plugin statically embeds irlserver/srtla, which is licensed under the
GNU Affero General Public License v3.0. The combined plugin binary is therefore
covered by AGPL-3.0; OBS Studio's GPL-2.0-or-later licensing permits this
combination. If you distribute the plugin, or offer it to users over a
network, you must also provide the corresponding source code of the plugin
and of the embedded SRTLA revision under AGPL-3.0 terms. The upstream license
text is fetched with the sources at
`build_x86_64/_deps/srtla-src/LICENSE`.

On Linux the plugin also statically embeds Haivision libsrt (MPL-2.0, license
at `build_x86_64/srt_external-prefix/src/srt_external/LICENSE`) and OpenSSL 3
`libcrypto` (Apache-2.0). On Windows it statically embeds wepoll
(BSD-2-Clause) and links dynamically to OBS's libsrt. All of these licenses
are compatible with distributing the combined binary under AGPL-3.0. When
distributing it, include their license notices and make the libsrt source for
the embedded release available. `cmake --install` and the Windows installer
place the srtla, spdlog, libsrt and (Windows) wepoll license texts in the
plugin's data directory under `licenses/`.
