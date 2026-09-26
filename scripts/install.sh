PLUGIN_DIR="$HOME/.config/obs-studio/plugins/obs-srtla-source"
install -Dm755 build_x86_64/obs-srtla-source.so "$PLUGIN_DIR/bin/64bit/obs-srtla-source.so"
install -Dm644 data/locale/en-US.ini "$PLUGIN_DIR/data/locale/en-US.ini"
install -Dm644 data/locale/cs-CZ.ini "$PLUGIN_DIR/data/locale/cs-CZ.ini"
