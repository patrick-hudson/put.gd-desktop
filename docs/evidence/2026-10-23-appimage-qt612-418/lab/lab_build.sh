#!/bin/bash
# lab_build.sh (#418): one lab-tagged nightly AppImage of the PR head, for 1058 -> this build through the lab feed.
# From docs/evidence/2026-10-22-linux-update-acceptance/lab/lab_build.sh, AppImage only, Qt 6.12.0.
#   lab_build.sh deps HEAD     clone HEAD, container, apt, Qt, SDK and AppImage deps (f44)
#   lab_build.sh build HEAD    one empty lab commit on HEAD (local tag, never pushed), then the AppImage
set -euo pipefail
ROOT=/home/patrick/work/put.gd-qt418/lab
OUT=$ROOT/out
NAME=putgd-qt418-applab
IMAGE=docker.io/library/ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
QT=6.12.0
mkdir -p "$OUT"; cd "$ROOT"
run() { podman exec -w /lab/appsrc -e DEBIAN_FRONTEND=noninteractive -e QT_ROOT_DIR=/lab/qt/$QT/gcc_64 "$NAME" bash -c "$1"; }
case "$1" in
deps)
  [ -d appsrc/.git ] || git clone -q https://github.com/patrick-hudson/put.gd-desktop.git appsrc
  git -C appsrc fetch -q origin claude/appimage-qt-bump-418-9a269d nightly
  git -C appsrc checkout -q --detach "$2"
  podman container exists "$NAME" || podman run -d --name "$NAME" -v "$ROOT:/lab:Z" -w /lab/appsrc "$IMAGE" sleep infinity >/dev/null
  run 'apt-get update -q && bash tools/releases/install-native-deps.sh'
  run 'apt-get install -y -q patchelf libgtk-3-dev libsecret-1-dev qt6-base-private-dev libx11-xcb-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-render-util0-dev libxcb-sync-dev
       apt-get install -y -q nasm libva-dev libvpx-dev libwebp-dev libopus-dev libffmpeg-nvenc-dev libx264-dev libx265-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-pipewire gstreamer1.0-pulseaudio gstreamer1.0-vaapi
       printf "Types: deb-src\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates noble-security\nComponents: main restricted universe multiverse\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n" > /etc/apt/sources.list.d/putgd-appimage-src.sources
       apt-get update -q && apt-get install -y -q --no-install-recommends weston python3-venv
       git config --global --add safe.directory /lab/appsrc'
  run "[ -x /lab/qt/$QT/gcc_64/bin/qmake ] || { python3 -m venv /lab/aqt && /lab/aqt/bin/pip install -q aqtinstall==3.3.0 && /lab/aqt/bin/aqt install-qt linux desktop $QT linux_gcc_64 -m qtimageformats -O /lab/qt; }"
  run '[ -d build/dependencies/aws-sdk-cpp ] || bash tools/ci/sdk-source.sh'
  run '[ -f build/sdk-cache.json ] || PUTGD_SDK_JOBS=3 nice -n 19 bash tools/releases/provision-sdk.sh'
  run 'rm -rf build/dependencies/appimage build/appimage-ecm build/appimage-qtkeychain build/appimage-layershell build/appimage-ffmpeg build/appimage-deps && nice -n 19 python3 tools/releases/provision-appimage.py --qt-root "$QT_ROOT_DIR"'
  run 'python3 tools/proton/provision_runtime.py'
  echo DEPS-DONE
  ;;
build)
  git -C appsrc fetch -q origin claude/appimage-qt-bump-418-9a269d
  git -C appsrc checkout -q --detach "$2"
  date=$(date -u +%FT%TZ)
  env GIT_AUTHOR_NAME=lab GIT_AUTHOR_EMAIL=lab@example.invalid GIT_COMMITTER_NAME=lab GIT_COMMITTER_EMAIL=lab@example.invalid \
    GIT_AUTHOR_DATE="$date" GIT_COMMITTER_DATE="$date" git -C appsrc commit -q --allow-empty -m "lab: #418 Qt 6.12 nightly for the lab feed (never pushed)"
  tag=$(cd appsrc && python3 -c 'import subprocess, sys; sys.path.insert(0, "tools/releases")
from release_identity import calendar_base
t = int(subprocess.check_output(["git", "show", "-s", "--format=%ct", "HEAD"]))
n = int(subprocess.check_output(["git", "rev-list", "--count", "HEAD"]))
print("v" + calendar_base(t, n) + "-nightly")')
  git -C appsrc tag -f "$tag" >/dev/null
  commit=$(git -C appsrc rev-parse HEAD)
  echo "$tag $commit (on $2)" | tee "$OUT/tag.txt"
  run "rm -rf build/release build/appimage-stage build/appimage-output build/appimage-tools build/appimage-diagnostics &&
       RELEASE_TAG=$tag PUTGD_APPIMAGE_UPDATE_BASE=https://pkg.put.gd/lab/ PUTGD_BUILD_JOBS=3 nice -n 19 bash tools/releases/build-appimage.sh > /lab/out/appimage-$tag.log 2>&1"
  mkdir -p "$OUT/$tag"
  cp appsrc/build/appimage-output/putgd-${tag#v}-x86_64.AppImage appsrc/build/appimage-output/putgd-${tag#v}-x86_64.AppImage.zsync appsrc/build/appimage-output/appimage-build.json "$OUT/$tag/"
  (cd "$OUT/$tag" && sha256sum putgd-${tag#v}-x86_64.AppImage putgd-${tag#v}-x86_64.AppImage.zsync > SHA256SUMS && cat SHA256SUMS)
  echo BUILD-DONE
  ;;
build-real)
  # 1058 -> this build through the lab feed: the image must carry the real nightly channel's update information
  # (the installer refuses an image that follows other update information), so it's built like a real nightly,
  # without PUTGD_APPIMAGE_UPDATE_BASE. Its .zsync is then rewritten to name the lab mirror copy, which the
  # signer's lab path requires. Two empty commits on HEAD (local tag, never pushed) give a version newer than
  # the lab's earlier v26.1023.1068-nightly.
  git -C appsrc fetch -q origin claude/appimage-qt-bump-418-9a269d
  git -C appsrc checkout -q --detach "$2"
  for i in 1 2; do date=$(date -u +%FT%TZ)
    env GIT_AUTHOR_NAME=lab GIT_AUTHOR_EMAIL=lab@example.invalid GIT_COMMITTER_NAME=lab GIT_COMMITTER_EMAIL=lab@example.invalid \
      GIT_AUTHOR_DATE="$date" GIT_COMMITTER_DATE="$date" git -C appsrc commit -q --allow-empty -m "lab: #418 Qt 6.12 nightly with the real update information ($i/2, never pushed)"; done
  tag=$(cd appsrc && python3 -c 'import subprocess, sys; sys.path.insert(0, "tools/releases")
from release_identity import calendar_base
t = int(subprocess.check_output(["git", "show", "-s", "--format=%ct", "HEAD"]))
n = int(subprocess.check_output(["git", "rev-list", "--count", "HEAD"]))
print("v" + calendar_base(t, n) + "-nightly")')
  git -C appsrc tag -f "$tag" >/dev/null
  echo "$tag $(git -C appsrc rev-parse HEAD) (on $2, real update information)" | tee "$OUT/tag-real.txt"
  name=putgd-${tag#v}-x86_64.AppImage
  run "rm -rf build/appimage-stage build/appimage-output build/appimage-diagnostics &&
       RELEASE_TAG=$tag PUTGD_BUILD_JOBS=3 nice -n 19 bash tools/releases/build-appimage.sh > /lab/out/appimage-$tag.log 2>&1 &&
       cd build/appimage-output && rm -f $name.zsync && ../appimage-tools/zsyncmake -u https://pkg.put.gd/lab/releases/$tag/$name -o $name.zsync $name > /dev/null"
  mkdir -p "$OUT/$tag"
  cp appsrc/build/appimage-output/$name appsrc/build/appimage-output/$name.zsync appsrc/build/appimage-output/appimage-build.json "$OUT/$tag/"
  (cd "$OUT/$tag" && sha256sum $name $name.zsync > SHA256SUMS && cat SHA256SUMS)
  echo BUILD-DONE
  ;;
*) echo "usage: $0 deps HEAD | build HEAD | build-real HEAD" >&2; exit 2 ;;
esac
