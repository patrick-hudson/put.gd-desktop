#!/usr/bin/env bash
# Install one CI image profile's packages into the running distro container.
#
#   tools/ci/images/provision.sh PROFILE
#
# The Dockerfile beside this script runs it once per image build, so CI jobs
# start from an image that already holds everything and never touch a distro
# mirror (tools/ci/images/profiles.json, docs/release/ci.md "CI images"). Lab
# tools run it in a plain base container to reproduce a CI job without the
# registry. Run as root, from a checkout (it reads tools/ci and tools/releases).
set -euo pipefail
profile=${1:?usage: provision.sh PROFILE}
root=$(cd "$(dirname "$0")/../../.." && pwd)
install=$root/tools/ci/install-packages.sh
. /etc/os-release
export DEBIAN_FRONTEND=noninteractive PUTGD_PKG_LABEL="CI image $profile"
# After the ranked mirrors: a checked-in list of public HTTPS mirrors (#1668).
export PUTGD_APT_LAST_RESORT_MIRRORS=$root/tools/ci/apt-last-resort-mirrors.txt

# The packages the linux (24.04/26.04) and sanitizers jobs build and test with.
test_packages=(build-essential ccache git ca-certificates passwd util-linux python3 cmake ninja-build pkg-config
  libtesseract-dev libleptonica-dev libzxing-dev tesseract-ocr-eng qt6-base-dev qt6-svg-dev qt6-base-dev-tools
  libqt6sql6-sqlite qt6-wayland-dev qtkeychain-qt6-dev libxcb1-dev libxcb-keysyms1-dev libxcb-util-dev
  libxcb-image0-dev libxcb-randr0-dev libxcb-xfixes0-dev libxcb-damage0-dev libxcb-shape0-dev libxcb-xinput-dev
  libxcb-xtest0-dev xvfb xauth dbus-daemon gjs libxcursor-dev libxkbcommon-dev libxkbcommon-x11-dev
  libcurl4-openssl-dev libssl-dev zlib1g-dev libwayland-dev wayland-protocols libgstreamer1.0-dev
  libgstreamer-plugins-base1.0-dev libavcodec-dev libavformat-dev libavfilter-dev libavutil-dev libswscale-dev
  libswresample-dev)

# Ubuntu: rank the mirrors first, and put the image's own sources back afterwards.
sources_backup=
if [ "$ID" = ubuntu ]; then
  sources_backup=$(mktemp -d)
  cp -a /etc/apt/sources.list /etc/apt/sources.list.d "$sources_backup"/ 2>/dev/null || true
  bash "$root/tools/ci/ubuntu-mirrors.sh"
fi

case "$profile" in
  linux-24.04|linux-26.04)
    packages=("${test_packages[@]}" qemu-utils nodejs npm gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
      gstreamer1.0-plugins-ugly gstreamer1.0-pipewire gstreamer1.0-libav)
    if [ "$profile" = linux-26.04 ]; then
      # faketime pins the UI catalog clock; Pillow and numpy support shared image-diff tests.
      packages+=(liblayershellqtinterface-dev faketime python3-pil python3-numpy)
      # budgie_labwc_integration runs the Budgie adapter against a real headless labwc (#1275).
      packages+=(labwc wl-clipboard wlr-randr)
    fi
    bash "$install" "${packages[@]}"
    # CTest runs as this unprivileged user (tools/ci/ctest_preflight.py).
    useradd --create-home --shell /bin/bash putgd-ci
    ;;
  sanitizers)
    bash "$install" "${test_packages[@]}" clang libclang-rt-dev llvm curl liblayershellqtinterface-dev
    useradd --create-home --shell /bin/bash putgd-ci
    ;;
  native-*)
    # git and ca-certificates for actions/checkout, then the release builder's packages. ubuntu24.04 also
    # has zstd, so actions/cache writes what jobs outside any container can restore (#1652).
    if [ "$ID" = arch ]; then
      pacman -Syu --noconfirm --needed git ca-certificates
    else
      bash "$install" git ca-certificates $([ "$profile" != native-ubuntu24.04 ] || echo zstd)
    fi
    bash "$root/tools/releases/install-native-deps.sh"
    # tools/releases/build-arch.sh runs makepkg, which refuses root.
    [ "$ID" != arch ] || useradd --create-home builder
    ;;
  appimage)
    # One apt transaction: the base dependencies, tools/ci/appimage-packages.txt (patchelf, the bundled
    # libraries, the screen recording build inputs, GStreamer plugins and the system libraries the Qt
    # action would install), weston for the image's start check, and the hosted runner's tools the
    # job uses: python3-venv for aqtinstall, sudo, zstd, file, xz, curl, unzip and gpg.
    PUTGD_EXTRA_PACKAGES="$(sed 's/#.*//' "$root/tools/ci/appimage-packages.txt") weston python3-venv sudo zstd file xz-utils curl unzip gnupg"
    export PUTGD_EXTRA_PACKAGES
    bash "$root/tools/releases/install-native-deps.sh"
    # The official Qt the AppImage bundles, as jurplel/install-qt-action installed it on the hosted runner:
    # aqtinstall 3.3 and py7zr 1.1.0, the locked version, linux_gcc_64 with qtimageformats.
    # The image build passes the version (tools/ci/ci_images.py), so the rest of the lock stays out of its inputs.
    qt=${PUTGD_QT_VERSION:-$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["buildInputs"]["qt"])' "$root/packaging/appimage/tools.lock.json")}
    python3 -m venv /opt/aqt
    /opt/aqt/bin/pip install --no-cache-dir 'aqtinstall==3.3.*' 'py7zr==1.1.0'
    for attempt in 1 2 3; do
      LOG_CFG=$root/tools/ci/aqt-logging.ini /opt/aqt/bin/aqt install-qt linux desktop "$qt" linux_gcc_64 -m qtimageformats -O /opt/Qt && break
      [ "$attempt" != 3 ] || exit 1
      echo "::warning::Qt $qt install: attempt $attempt of 3 failed"
      rm -rf "/opt/Qt/$qt"
      sleep $((attempt * 30))
    done
    test -x "/opt/Qt/$qt/gcc_64/bin/qmake"
    ;;
  pipewire)
    # PipeWire 1.6, its plugin headers and a compiler for the check's probe, as the check itself lists them.
    bash "$root/tests/desktop/pipewire/ci_check.sh" --install
    ;;
  *) echo "Unknown CI image profile: $profile" >&2; exit 64 ;;
esac

if [ -n "$sources_backup" ]; then
  rm -f /etc/apt/putgd-mirrors.txt /etc/apt/putgd-mirrors.txt.usable /etc/apt/putgd-mirrors.txt.last-resort
  rm -rf /etc/apt/sources.list.d
  cp -a "$sources_backup"/. /etc/apt/
  rm -rf "$sources_backup"
fi
# Package indexes and downloads are only needed while installing.
case "$ID" in
  fedora) dnf clean all ;;
  arch) pacman -Scc --noconfirm >/dev/null ;;
  *) apt-get clean; rm -rf /var/lib/apt/lists/* /var/cache/putgd-apt ;;
esac
