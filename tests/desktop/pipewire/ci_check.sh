#!/bin/bash
# CI check (#144): the AppImage's GStreamer pipewiresrc, with the properties
# the recorder sets, must link to KWin- and Mutter-like screen casts on a
# PipeWire daemon that enforces 1.5.81's buffer-range rule (#137).
#
#   tests/desktop/pipewire/ci_check.sh APPDIR
#
# APPDIR is an extracted AppImage (squashfs-root). Runs in a container with
# PipeWire >= 1.5.81 (Ubuntu 26.04); with --install it first installs the
# distro packages it needs. Needs no session manager, devices or D-Bus.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
if [ "${1:-}" = --install ]; then
    shift
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends pipewire-bin gstreamer1.0-tools libpipewire-0.3-dev \
        gcc g++ pkg-config qt6-base-dev
fi
[ $# -eq 1 ] || { sed -n 2,10p "$0"; exit 64; }
plugins=$(cd "$1/usr/lib/gstreamer-1.0" && pwd)
[ -f "$plugins/libgstpipewire.so" ] || { echo "no libgstpipewire.so in $plugins"; exit 2; }
echo "AppImage plugin: $(sha256sum "$plugins/libgstpipewire.so")"

daemon=$(pipewire --version | awk '/Linked with/{print $NF}')
oldest=$(printf '%s\n1.5.81\n' "$daemon" | sort -V | head -1)
[ "$oldest" = 1.5.81 ] || { echo "PipeWire $daemon is older than 1.5.81; it cannot see #137"; exit 2; }

w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
g++ -std=c++20 -fPIC -O1 -I"$root/src" -o "$w/recorder_properties" "$here/recorder_properties.cpp" \
    "$root/src/record/WaylandAcquisition.cpp" $(pkg-config --cflags --libs Qt6Core)
read -r -a properties <<<"$("$w/recorder_properties")"
echo "Recorder's pipewiresrc properties: ${properties[*]}"

status=0
for mode in kwin mutter; do
    echo "== $mode, recorder properties"
    bash "$here/run_buffers.sh" "$plugins" "$mode" "${properties[@]}" || status=1
done
# Control: a plugin that asks for at least 8 buffers must be refused by the
# KWin-like source's 2..4, or this daemon does not enforce the rule.
echo "== kwin, min-buffers=8 (must fail)"
if bash "$here/run_buffers.sh" "$plugins" kwin min-buffers=8; then
    echo "The control linked: this daemon does not refuse impossible buffer ranges"
    status=1
fi
[ $status -eq 0 ] && echo "PipeWire $daemon: the AppImage's pipewiresrc links with the recorder's properties"
exit $status
