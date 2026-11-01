#!/usr/bin/env bash
# Standard distro packages inside disposable release-builder containers.
# tools/ci/install-packages.sh retries mirror failures and, with PUTGD_PKG_CACHE
# set, falls back to the last good package snapshot (#368).
set -euo pipefail
. /etc/os-release
install="$(dirname "$0")/../ci/install-packages.sh"
case "$ID" in
  ubuntu|debian)
    # Debian 13 builds against its own Qt 6.8 and FFmpeg 7.1 (libavcodec61); the Ubuntu DEBs cannot install there.
    packages=(build-essential ccache git ca-certificates python3 nodejs npm cmake ninja-build pkg-config tar gzip xz-utils binutils file dpkg-dev qt6-base-dev qt6-svg-dev qt6-base-dev-tools libqt6sql6-sqlite qt6-wayland-dev qtkeychain-qt6-dev libxcb1-dev libxcb-keysyms1-dev libxcb-util-dev libxcb-image0-dev libxcb-randr0-dev libxcb-xfixes0-dev libxcb-damage0-dev libxcb-shape0-dev libxcb-xinput-dev libxcursor-dev libxkbcommon-dev libxkbcommon-x11-dev libcurl4-openssl-dev libssl-dev zlib1g-dev libwayland-dev wayland-protocols libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libavcodec-dev libavformat-dev libavfilter-dev libavutil-dev libswscale-dev libswresample-dev)
    if [ "$ID:$VERSION_ID" = 'ubuntu:26.04' ] || [ "$ID:$VERSION_ID" = 'debian:13' ]; then
      packages+=(liblayershellqtinterface-dev)
    fi
    # File-manager plugin headers (#1105, src/filemanager). Peony 4.20 on Ubuntu 26.04 is Qt 6; Peony 4.0 on Ubuntu 24.04 and Debian 13 is Qt 5.
    packages+=(libglib2.0-dev libnautilus-extension-dev libcaja-extension-dev libgtk-3-dev libpeony-dev)
    if [ "$ID:$VERSION_ID" = 'ubuntu:24.04' ] || [ "$ID:$VERSION_ID" = 'debian:13' ]; then
      packages+=(qtbase5-dev qtbase5-dev-tools)
    fi
    bash "$install" "${packages[@]}"
    ;;
  fedora)
    bash "$install" gcc-c++ ccache git ca-certificates python3 nodejs npm cmake ninja-build pkgconf-pkg-config tar gzip xz binutils file rpm-build cpio qt6-qtbase-devel qt6-qtsvg-devel qt6-qtbase-private-devel qt6-qtwayland-devel qtkeychain-qt6-devel layer-shell-qt-devel libxcb-devel xcb-util-keysyms-devel xcb-util-devel xcb-util-image-devel libXcursor-devel libxkbcommon-devel libxkbcommon-x11-devel libcurl-devel openssl-devel zlib-devel wayland-devel wayland-protocols-devel gstreamer1-devel gstreamer1-plugins-base-devel ffmpeg-free-devel \
      glib2-devel nautilus-devel caja-devel gtk3-devel
    ;;
  *) echo "Unsupported native release builder: $ID" >&2; exit 1 ;;
esac
