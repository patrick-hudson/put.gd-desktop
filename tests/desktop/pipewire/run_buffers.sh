#!/bin/bash
# Checks that a GStreamer pipewiresrc plugin can link to a compositor-like
# screen cast on this host's PipeWire, without a desktop session (#137).
#
#   tests/desktop/pipewire/run_buffers.sh PLUGIN_DIR kwin|mutter [pipewiresrc properties...]
#
# PLUGIN_DIR holds the pipewire, coreelements, videorate and videoconvertscale
# plugins to test, for example links into an extracted AppImage's
# usr/lib/gstreamer-1.0. It starts a private PipeWire daemon (no session
# manager, no devices) in a new runtime directory, the screencast_like_src
# source and `gst-launch-1.0 pipewiresrc ...` as the recorder builds it, links
# them with pw-link and prints PASS (frames reached the sink) or FAIL with the
# daemon's link error. Needs gcc, the libpipewire-0.3 headers, pipewire,
# pw-link and gst-launch-1.0. Exit status 0 on PASS, 1 on FAIL.
set -u
here=$(cd "$(dirname "$0")" && pwd)
[ $# -ge 2 ] || { sed -n 2,15p "$0"; exit 64; }
plugins=$(cd "$1" && pwd) mode=$2; shift 2
gst= src= pw=
w=$(mktemp -d /tmp/pwbuf.XXXXXX)
trap 'kill $gst $src $pw 2>/dev/null; wait 2>/dev/null; rm -rf "$w"' EXIT
gcc -O2 -Wall -o "$w/screencast_like_src" "$here/screencast_like_src.c" $(pkg-config --cflags --libs libpipewire-0.3) || exit 2
export XDG_RUNTIME_DIR=$w PIPEWIRE_RUNTIME_DIR=$w PIPEWIRE_REMOTE=pipewire-0
pipewire -c "$here/private-pipewire.conf" >"$w/pw.log" 2>&1 & pw=$!
for _ in $(seq 50); do [ -S "$w/pipewire-0" ] && break; sleep 0.1; done
timeout 15 "$w/screencast_like_src" "$mode" 5 >"$w/src.log" 2>&1 & src=$!
for _ in $(seq 50); do grep -q '^node ' "$w/src.log" && break; sleep 0.1; done
node=$(awk '/^node /{print $2; exit}' "$w/src.log")
GST_PLUGIN_SYSTEM_PATH=$plugins GST_PLUGIN_PATH= GST_REGISTRY=$w/registry.bin \
  timeout 9 gst-launch-1.0 -v pipewiresrc path="$node" keepalive-time=1000 resend-last=true "$@" \
  ! videorate ! video/x-raw,framerate=30/1 ! videoconvert ! fakesink silent=false >"$w/gst.log" 2>&1 & gst=$!
for _ in $(seq 50); do in=$(pw-link -i 2>/dev/null | head -1); [ -n "$in" ] && break; sleep 0.1; done
out=$(pw-link -o 2>/dev/null | head -1)
pw-link "$out" "$in" || echo "pw-link $out -> $in failed"
wait $gst 2>/dev/null
wait $src 2>/dev/null
frames=$(grep -c 'chain' "$w/gst.log")
echo "pipewire $(pipewire --version | awk '/Linked with/{print $NF}'), source $mode, properties: ${*:-none}"
echo "buffers allocated: $(grep -c '^buffers' "$w/src.log"), frames at the sink: $frames"
if [ "$frames" -gt 0 ]; then echo PASS; exit 0; fi
grep -h 'error alloc buffers\|pw.link.*error' "$w/pw.log" | head -3
echo FAIL; exit 1
