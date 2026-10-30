#!/bin/sh
# Stage probes only inside a disposable guest/sandbox, never in a golden.
set -eu
if [ "${1:-}" != --disposable ]; then
  echo 'usage: provision-probes.sh --disposable (never run on goldens)' >&2
  exit 2
fi
sudo -n apt-get update
sudo -n apt-get install -y --no-install-recommends \
  python3 python3-pil python3-pyatspi python3-dbus python3-pyqt6 dbus-bin libglib2.0-bin libx11-6 \
  tesseract-ocr xdotool x11-utils x11-xserver-utils xclip wl-clipboard
python3 -c 'import pyatspi, dbus; from PyQt6.QtGui import QGuiApplication'
for tool in tesseract dbus-monitor gdbus xdotool xprop xwininfo xrandr xclip wl-paste wl-copy; do
  command -v "$tool" >/dev/null
done
