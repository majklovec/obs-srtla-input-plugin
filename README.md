# OBS SRTLA Source

An OBS input source that receives bonded SRTLA streams and outputs decoded
video and audio to OBS.

## Runtime components

- `srtla_rec` from the IRLServer SRTLA receiver project. The executable path
  can be set in the source properties; by default it is searched for as
  `srtla_rec` on `PATH`. The receiver is a separate process, not linked into
  the plugin.
- FFmpeg development libraries (`libavformat`, `libavcodec`, `libavutil`,
  `libswresample`, and `libswscale`), built with MPEG-TS, H.264 and AAC
  support.
- libsrt development libraries and OBS Studio's libobs development package.

The plugin opens a local SRT listener on an ephemeral loopback port, then
starts `srtla_rec` with that port as its downstream SRT target. The receiver
listens on the configured SRTLA UDP port. FFmpeg reads the resulting SRT
payload as MPEG-TS, demuxes it, decodes H.264/AAC, converts video to NV12 and
audio to planar float, and submits timestamped frames to OBS.

The upstream SRTLA receiver does not provide the C callback API described in
the original design notes. Keeping it as a separate process avoids a private
fork and keeps its AGPL-3.0 code out of this plugin.

## Build

On Ubuntu or Pop!_OS, run the setup script to install the compiler, OBS/FFmpeg/libsrt development packages, build the upstream `srtla_rec` helper, and build the plugin:

```sh
./scripts/install-and-build.sh
```

The script requires `sudo` for system package installation. It puts the SRTLA
checkout under `.deps/srtla`, builds the helper at the pinned upstream
revision, and places the executable at
`build_x86_64/runtime/bin/srtla_rec`. The upstream helper is AGPL-3.0.

To build the plugin again after setup, use:

```sh
./scripts/build.sh
```

The plugin itself is written to
`build_x86_64/obs-srtla-source.so`. Enter the helper's full path in the OBS
source properties, or make `srtla_rec` available on `PATH`.

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
4. Set **srtla_rec Executable** to the full path printed by
   `install-and-build.sh` if the helper is not on OBS's `PATH`, then confirm
   **SRTLA Listen Port (UDP)** is available to the sender.
5. Check the OBS log again after adding the source. The plugin should report
   that it is listening on the configured UDP port. If `srtla_rec` cannot be
   launched or exits, the log reports that separately from plugin loading.
6. Start the SRTLA sender. A successfully loaded plugin only confirms that
   OBS found the module; receiving and decoding media requires the helper to
   run and the sender to connect. Confirm decoded audio meters or video in
   the preview.

If OBS does not load the module, first make sure OBS is fully restarted and
that the plugin was built for the same architecture and compatible native OBS
installation. On Linux, inspect unresolved shared-library dependencies with:

```sh
ldd build_x86_64/obs-srtla-source.so
```

Every listed dependency should resolve to a library path rather than `not
found`.

## Source settings

- **SRTLA Listen Port (UDP):** port exposed to the SRTLA sender; defaults to
  `5000`.
- **srtla_rec Executable:** executable name or path; defaults to
  `srtla_rec`.

Changing either setting stops the current receiver and starts it again. The
plugin stops its helper process and worker thread when the source is removed.
