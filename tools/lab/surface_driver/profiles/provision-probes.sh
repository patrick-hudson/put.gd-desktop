#!/bin/sh
# Run INSIDE a disposable Debian/Ubuntu/Mint guest before sealing a NEW golden.
# Never boot or modify an existing golden or replace a named boat snapshot.
#
# --recording-overlay stages only the recording fixture's media dependency
# (GStreamer's GTK3 sink for the visible pattern, #784) in a disposable run
# overlay. It is never sealed into a golden or snapshot; it fails closed when
# the package or the gtksink element is unavailable.
set -eu
if [ "${1:-}" = --recording-overlay ]; then
  sudo -n apt-get update
  sudo -n apt-get install -y --no-install-recommends \
    gstreamer1.0-gtk3 gir1.2-gstreamer-1.0 gir1.2-gtk-3.0 python3-gi
  python3 -c "import gi; gi.require_version('Gst', '1.0'); from gi.repository import Gst; Gst.init(None); assert Gst.ElementFactory.find('gtksink'), 'gtksink missing'"
  exit 0
fi
sudo -n apt-get update
sudo -n apt-get install -y --no-install-recommends \
  python3 python3-pyatspi python3-dbus python3-pyqt6 dbus-bin libglib2.0-bin libx11-6 \
  xdotool x11-utils x11-xserver-utils xclip wl-clipboard
python3 -c 'import pyatspi, dbus; from PyQt6.QtGui import QGuiApplication'
for tool in dbus-monitor gdbus xdotool xprop xwininfo xrandr xclip wl-paste wl-copy; do
  command -v "$tool" >/dev/null
done
