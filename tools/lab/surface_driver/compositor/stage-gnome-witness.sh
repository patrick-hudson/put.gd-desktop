#!/bin/sh
# Run INSIDE a disposable GNOME lab guest (never a real desktop or a golden):
#   sh stage-gnome-witness.sh DIR   # DIR holds extension.js and metadata.json
# Installs the lab-only read-only witness for this user and enables it. GNOME
# Shell loads a newly installed extension only at session start: follow with
#   python3 tools/lab/guest_reboot.py RUN
# then `gnome-extensions enable putgd-lab-witness@put.gd` if it is not ACTIVE.
set -eu
uuid=putgd-lab-witness@put.gd
target="$HOME/.local/share/gnome-shell/extensions/$uuid"
mkdir -p "$target"
cp "$1/extension.js" "$1/metadata.json" "$target/"
gnome-extensions enable "$uuid" 2>/dev/null || true
gnome-extensions info "$uuid" | grep -E 'Path|State' || true
