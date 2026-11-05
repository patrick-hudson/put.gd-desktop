#!/bin/bash
# calver-appimage-lab.sh (#373 lab acceptance): lab AppImages, built as
# package.yml's appimage job builds them, in one pinned ubuntu:24.04 podman
# container on f44, for the signer's lab prefix
# (PUTGD_APPIMAGE_UPDATE_BASE=https://pkg.put.gd/lab/). Lab commits use a fixed
# identity and date, so CalVer N is the same commit as the native lab packages;
# every tag is local to this clone and never pushed.
#   calver-appimage-lab.sh deps          apt, Qt 6.8.3 (aqt), SDK, AppImage deps, Proton helper
#   calver-appimage-lab.sh legacy BASE   a pre-#373 nightly v0.1.1-nightly.COUNT: one lab commit on BASE
#   calver-appimage-lab.sh calver HEAD   CalVer N and N+1: one and two lab commits on HEAD
#   calver-appimage-lab.sh build-tag TAG  rebuild an existing lab tag
set -euo pipefail
LAB=/home/patrick/work/put.gd-calver373-applab
NAME=putgd-calver373-applab
IMAGE=docker.io/library/ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
mkdir -p "$LAB"
cd "$LAB"
[ -d src/.git ] || git clone -q https://github.com/patrick-hudson/put.gd-desktop.git src
if ! podman container exists "$NAME"; then
  podman run -d --name "$NAME" -v "$LAB:/lab:Z" -w /lab/src "$IMAGE" sleep infinity >/dev/null
fi
run() { podman exec -w /lab/src -e DEBIAN_FRONTEND=noninteractive -e QT_ROOT_DIR=/lab/qt/6.8.3/gcc_64 "$NAME" bash -c "$1"; }
LABENV='GIT_AUTHOR_NAME=lab GIT_AUTHOR_EMAIL=lab@example.invalid GIT_COMMITTER_NAME=lab GIT_COMMITTER_EMAIL=lab@example.invalid'
build() {  # build HEAD with tag $1, copy the image and .zsync to /lab/out/$1
  run "tag=$1 && { git rev-parse -q --verify refs/tags/\$tag >/dev/null || git tag \$tag; } &&
       rm -rf build/appimage-stage build/appimage-output &&
       RELEASE_TAG=\$tag PUTGD_APPIMAGE_UPDATE_BASE=https://pkg.put.gd/lab/ PUTGD_BUILD_JOBS=2 nice -n 19 bash tools/releases/build-appimage.sh &&
       mkdir -p /lab/out/\$tag && cp build/appimage-output/putgd-\${tag#v}-x86_64.AppImage build/appimage-output/putgd-\${tag#v}-x86_64.AppImage.zsync /lab/out/\$tag/ &&
       echo built \$tag \$(git rev-parse HEAD) \$(sha256sum /lab/out/\$tag/putgd-\${tag#v}-x86_64.AppImage | cut -c1-64) | tee -a /lab/out/built.txt"
}
calver_tag() { run 'python3 -c "import subprocess, sys; sys.path.insert(0, \"tools/releases\")
from release_identity import calendar_base
t = int(subprocess.check_output([\"git\", \"show\", \"-s\", \"--format=%ct\", \"HEAD\"]))
n = int(subprocess.check_output([\"git\", \"rev-list\", \"--count\", \"HEAD\"]))
print(\"v\" + calendar_base(t, n) + \"-nightly\")"'; }
case "$1" in
deps)
  run 'apt-get update -q && bash tools/releases/install-native-deps.sh'
  run 'apt-get install -y -q patchelf libgtk-3-dev libsecret-1-dev qt6-base-private-dev libx11-xcb-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-render-util0-dev libxcb-sync-dev
       apt-get install -y -q nasm libva-dev libvpx-dev libwebp-dev libopus-dev libffmpeg-nvenc-dev libx264-dev libx265-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-pipewire gstreamer1.0-pulseaudio gstreamer1.0-vaapi
       printf "Types: deb-src\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates noble-security\nComponents: main restricted universe multiverse\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n" > /etc/apt/sources.list.d/putgd-appimage-src.sources
       apt-get update -q && apt-get install -y -q --no-install-recommends weston python3-venv
       git config --global --add safe.directory /lab/src'
  run '[ -x /lab/qt/6.8.3/gcc_64/bin/qmake ] || { python3 -m venv /lab/aqt && /lab/aqt/bin/pip install -q aqtinstall==3.3.0 && /lab/aqt/bin/aqt install-qt linux desktop 6.8.3 linux_gcc_64 -m qtimageformats -O /lab/qt; }'
  run '[ -d build/dependencies/aws-sdk-cpp ] || bash tools/ci/sdk-source.sh'
  run '[ -f build/sdk-cache.json ] || PUTGD_SDK_JOBS=2 nice -n 19 bash tools/releases/provision-sdk.sh'
  run 'nice -n 19 python3 tools/releases/provision-appimage.py --qt-root "$QT_ROOT_DIR"'
  run 'python3 tools/proton/provision_runtime.py'
  ;;
legacy)
  run "git fetch -q origin nightly && git checkout -q --detach $2 &&
       $LABENV GIT_AUTHOR_DATE=2026-10-22T11:00:00Z GIT_COMMITTER_DATE=2026-10-22T11:00:00Z git commit -q --allow-empty -m 'lab: #373 pre-calendar nightly (never pushed)'"
  build "v0.1.1-nightly.$(run 'git rev-list --count HEAD')"
  ;;
calver)
  run "git fetch -q origin claude/calver-373-03743c && git checkout -q --detach $2 &&
       $LABENV GIT_AUTHOR_DATE=2026-10-22T12:00:00Z GIT_COMMITTER_DATE=2026-10-22T12:00:00Z git commit -q --allow-empty -m 'lab: #373 CalVer nightly N (never pushed)'"
  build "$(calver_tag)"
  run "$LABENV GIT_AUTHOR_DATE=2026-10-22T12:10:00Z GIT_COMMITTER_DATE=2026-10-22T12:10:00Z git commit -q --allow-empty -m 'lab: #373 CalVer nightly N+1 (never pushed)'"
  build "$(calver_tag)"
  ;;
build-tag)  # rebuild an existing lab tag
  run "git checkout -q --detach refs/tags/$2"
  build "$2"
  ;;
*) echo "usage: $0 deps | legacy BASE | calver HEAD | build-tag TAG" >&2; exit 2 ;;
esac
