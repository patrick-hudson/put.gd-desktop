#!/usr/bin/env bash
set -euo pipefail
# Run in a checkout inside the already-running GNOME Wayland test overlay.
# Native Qt connects to Wayland but creates no windows; the Shell peer lives
# solely on a disposable D-Bus session. This is transport, not desktop proof.
: "${SDK_PREFIX:?Set SDK_PREFIX to the existing official AWS SDK installation}"
: "${SDK_SOURCE:?Set SDK_SOURCE to its pinned source checkout with LICENSE}"
: "${XDG_RUNTIME_DIR:?Run as the logged-in Wayland user}"
: "${WAYLAND_DISPLAY:?Set the native Wayland display socket name}"
evidence_dir="work/u11/gnome-recovery/replay"
mkdir -p "$evidence_dir"
cmake --preset dev -DCMAKE_PREFIX_PATH="$SDK_PREFIX" -DPUTGD_AWS_RUNTIME_DIR="$SDK_PREFIX/lib" -DPUTGD_AWS_SOURCE_DIR="$SDK_SOURCE" > "$evidence_dir/configure.log" 2>&1
cmake --build --preset dev --target test_gnome_bridge > "$evidence_dir/build.log" 2>&1
QT_QPA_PLATFORM=wayland dbus-run-session -- build/dev/test_gnome_bridge --adapter-transport -o "$evidence_dir/adapter.xml",junitxml -o -,txt > "$evidence_dir/adapter.log" 2>&1
ctest --preset dev -R '^gnome_bridge_contract$' --output-junit "$PWD/$evidence_dir/existing.xml" > "$evidence_dir/existing.log" 2>&1
sha256sum src/platform/gnome/GnomeAdapter.cpp tests/integration/test_gnome_bridge.cpp build/dev/test_gnome_bridge > "$evidence_dir/source-binary.sha256"
# Separately, with the existing host Node installation:
# node tests/desktop/test_gnome_ownership.mjs
