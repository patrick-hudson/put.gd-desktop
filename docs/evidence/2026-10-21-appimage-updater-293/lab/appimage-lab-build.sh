#!/bin/bash
# appimage-lab-build.sh (#293 phase 3 lab acceptance): build tagged lab nightly
# AppImages N and N+1 from the PR branch, as package.yml's appimage job does, in
# one long-lived pinned ubuntu:24.04 podman container on a builder. Tags are local
# to this clone and never pushed. The images embed the signer's lab layout
# (PUTGD_APPIMAGE_UPDATE_BASE=https://pkg.put.gd/lab/).
#   appimage-lab-build.sh deps        apt, Qt 6.8.3 (aqt), SDK, AppImage deps, Proton helper
#   appimage-lab-build.sh build REF   tag REF's commit with its nightly number and build it
set -euo pipefail
LAB=/home/patrick/work/put.gd-i293lab
NAME=putgd-i293lab
IMAGE=docker.io/library/ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
BRANCH=claude/appimage-updater-293
cd "$LAB"
[ -d src/.git ] || git clone --branch "$BRANCH" https://github.com/patrick-hudson/put.gd-desktop.git src
if ! podman container exists "$NAME"; then
  podman run -d --name "$NAME" -v "$LAB:/lab:Z" -w /lab/src "$IMAGE" sleep infinity >/dev/null
fi
run() { podman exec -w /lab/src -e DEBIAN_FRONTEND=noninteractive -e QT_ROOT_DIR=/lab/qt/6.8.3/gcc_64 "$NAME" bash -c "$1"; }
case "$1" in
deps)
  run 'apt-get update -q && bash tools/releases/install-native-deps.sh'
  run 'apt-get install -y -q patchelf libgtk-3-dev libsecret-1-dev qt6-base-private-dev libx11-xcb-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-render-util0-dev libxcb-sync-dev
       apt-get install -y -q nasm libva-dev libvpx-dev libwebp-dev libopus-dev libffmpeg-nvenc-dev libx264-dev libx265-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-pipewire gstreamer1.0-pulseaudio gstreamer1.0-vaapi
       printf "Types: deb-src\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates noble-security\nComponents: main restricted universe multiverse\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n" > /etc/apt/sources.list.d/putgd-appimage-src.sources
       apt-get update -q && apt-get install -y -q --no-install-recommends weston python3-venv
       git config --global --add safe.directory /lab/src && git config --global user.name "put.gd lab" && git config --global user.email lab@put.gd.invalid'
  run '[ -x /lab/qt/6.8.3/gcc_64/bin/qmake ] || { python3 -m venv /lab/aqt && /lab/aqt/bin/pip install -q aqtinstall==3.3.0 && /lab/aqt/bin/aqt install-qt linux desktop 6.8.3 linux_gcc_64 -m qtimageformats -O /lab/qt; }'
  run '[ -d build/dependencies/aws-sdk-cpp ] || bash tools/ci/sdk-source.sh'
  run '[ -f build/sdk-cache.json ] || PUTGD_SDK_JOBS=4 nice -n 10 bash tools/releases/provision-sdk.sh'
  run 'nice -n 10 python3 tools/releases/provision-appimage.py --qt-root "$QT_ROOT_DIR"'
  run 'python3 tools/proton/provision_runtime.py'
  ;;
build)
  ref=$2
  run "git fetch -q origin $BRANCH && git checkout -q --detach $ref && count=\$(git rev-list --count HEAD) && tag=v0.1.1-nightly.\$count &&
       { git rev-parse -q --verify refs/tags/\$tag >/dev/null || git tag \$tag; } &&
       RELEASE_TAG=\$tag PUTGD_APPIMAGE_UPDATE_BASE=https://pkg.put.gd/lab/ PUTGD_BUILD_JOBS=4 nice -n 10 bash tools/releases/build-appimage.sh &&
       mkdir -p /lab/out/\$tag && cp build/appimage-output/putgd-\${tag#v}-x86_64.AppImage build/appimage-output/putgd-\${tag#v}-x86_64.AppImage.zsync /lab/out/\$tag/ &&
       echo built \$tag"
  ;;
*) echo "usage: $0 deps | build REF" >&2; exit 2 ;;
esac
