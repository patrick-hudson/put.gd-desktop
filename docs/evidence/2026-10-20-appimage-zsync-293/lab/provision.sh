#!/usr/bin/env bash
# Runs inside the ubuntu:24.04 container on f44: the CI appimage job's environment.
set -euxo pipefail
export DEBIAN_FRONTEND=noninteractive
cd /src/tree
apt-get update
apt-get install -y sudo curl python3-pip python3-venv patchelf file
bash tools/releases/install-native-deps.sh
apt-get install -y patchelf libgtk-3-dev libsecret-1-dev qt6-base-private-dev libx11-xcb-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-render-util0-dev libxcb-sync-dev
apt-get install -y nasm libva-dev libvpx-dev libwebp-dev libopus-dev libffmpeg-nvenc-dev libx264-dev libx265-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-pipewire gstreamer1.0-pulseaudio gstreamer1.0-vaapi
printf 'Types: deb-src\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates noble-security\nComponents: main restricted universe multiverse\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n' > /etc/apt/sources.list.d/putgd-appimage-src.sources
apt-get update
apt-get install -y --no-install-recommends weston dpkg-dev
python3 -m venv /opt/aqt
/opt/aqt/bin/pip install -q aqtinstall
/opt/aqt/bin/aqt install-qt linux desktop 6.8.3 linux_gcc_64 -m qtimageformats -O /opt/Qt
export QT_ROOT_DIR=/opt/Qt/6.8.3/gcc_64
bash tools/ci/sdk-source.sh
PUTGD_SDK_JOBS=3 bash tools/releases/provision-sdk.sh
python3 tools/releases/provision-appimage.py --qt-root "$QT_ROOT_DIR"
python3 tools/proton/provision_runtime.py
echo PROVISION-DONE
