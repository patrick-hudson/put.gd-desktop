#!/bin/sh
# Run INSIDE a disposable Debian/Ubuntu/Mint guest before sealing a NEW golden.
# Never boot or modify an existing golden or replace a named boat snapshot.
set -eu
sudo -n apt-get update
sudo -n apt-get install -y --no-install-recommends \
  python3 python3-pyatspi python3-pyqt6 dbus-bin libglib2.0-bin libx11-6 \
  xdotool x11-utils x11-xserver-utils xclip wl-clipboard
python3 -c 'import pyatspi; from PyQt6.QtGui import QGuiApplication'
for tool in dbus-monitor gdbus xdotool xprop xrandr xclip wl-paste wl-copy; do
  command -v "$tool" >/dev/null
done
