#!/bin/sh
# Stage probes only inside a disposable guest/sandbox, never in a golden.
# Never boot or modify an existing golden or replace a named boat snapshot.
#
# --disposable stages the general probe tools before sealing a NEW golden
# from a disposable guest.
# --recording-overlay stages only the recording fixture's media dependency
# (GStreamer's GTK3 sink for the visible pattern, #784) in a disposable run
# overlay. It is never sealed into a golden or snapshot; it fails closed when
# the package or the gtksink element is unavailable.
# --keyring-overlay stages the distro Secret Service daemon for private
# synthetic KDE fixtures. It never opens or configures a desktop wallet.
set -eu
if [ "${1:-}" = --recording-overlay ]; then
  sudo -n apt-get update
  sudo -n apt-get install -y --no-install-recommends \
    gstreamer1.0-gtk3 gir1.2-gstreamer-1.0 gir1.2-gtk-3.0 python3-gi
  python3 -c "import gi; gi.require_version('Gst', '1.0'); from gi.repository import Gst; Gst.init(None); assert Gst.ElementFactory.find('gtksink'), 'gtksink missing'"
  exit 0
fi
if [ "${1:-}" = --keyring-overlay ]; then
  sudo -n apt-get update
  sudo -n apt-get install -y --no-install-recommends gnome-keyring python3-gi
  command -v gnome-keyring-daemon >/dev/null
  exit 0
fi
if [ "${1:-}" != --disposable ]; then
  echo 'usage: provision-probes.sh --disposable | --recording-overlay | --keyring-overlay (disposable guests and overlays only; never run on goldens)' >&2
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
