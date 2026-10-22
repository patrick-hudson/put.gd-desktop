#!/bin/bash
# lab_build.sh (Linux update profile, lab stable): lab-key builds of N+1's commit
# for the stable legs (U6 populated stable, U10, U14 stable), until a real stable
# release exists. On top of BASE (N+1's commit):
#   1. a lab-key commit: packaging/linux/repo/keys/putgd-stable.{asc,gpg,minisign.pub}
#      become the lab stable key's public halves from PUB, so every lab package and
#      image trusts the lab stable key, never the real one;
#   2. five empty commits, each tagged locally (never pushed) and built in turn:
#      nightly L1 (10-19), stable S1 (10-18, older than L1), stable S2 (10-20),
#      nightly L2 (10-21) and stable S3 (10-21 18:00, the newest).
# Fixed identities and dates, so every host makes the same commits and versions.
# Packages are built natively on their own host, as #386's lab386-build.sh did;
# the AppImage in a pinned ubuntu:24.04 podman container, as #373's
# calver-appimage-lab.sh did (PUTGD_APPIMAGE_UPDATE_BASE=https://pkg.put.gd/lab/).
#   lab_build.sh commits BASE PUB            clone and make the lab commits (each host)
#   lab_build.sh native LABEL SDK [AWS_RUNTIME_DIR]   the five packages for LABEL
#   lab_build.sh appimage-deps               container, apt, Qt 6.8.3, SDK, AppImage deps (f44)
#   lab_build.sh appimage                    the five AppImages (f44)
set -euo pipefail
ROOT=/home/patrick/work/put.gd-ua/lab
OUT=$ROOT/out
NAME=putgd-ua-applab
IMAGE=docker.io/library/ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
TAGS="L1:2026-10-19T12:00:00Z:nightly S1:2026-10-18T12:00:00Z:stable S2:2026-10-20T12:00:00Z:stable L2:2026-10-21T12:00:00Z:nightly S3:2026-10-21T18:00:00Z:stable"
mkdir -p "$OUT"
cd "$ROOT"
labenv() { env GIT_AUTHOR_NAME=lab GIT_AUTHOR_EMAIL=lab@example.invalid GIT_COMMITTER_NAME=lab GIT_COMMITTER_EMAIL=lab@example.invalid \
  GIT_AUTHOR_DATE="$1" GIT_COMMITTER_DATE="$1" "${@:2}"; }
tag_of() {  # the calendar tag HEAD gets, nightly or stable
  python3 -c 'import subprocess, sys; sys.path.insert(0, "tools/releases")
from release_identity import calendar_base
t = int(subprocess.check_output(["git", "show", "-s", "--format=%ct", "HEAD"]))
n = int(subprocess.check_output(["git", "rev-list", "--count", "HEAD"]))
print("v" + calendar_base(t, n) + ("-nightly" if sys.argv[1] == "nightly" else ""))' "$1"
}
case "$1" in
commits)
  base=$2 pub=$3
  [ -d src/.git ] || git clone -q https://github.com/patrick-hudson/put.gd-desktop.git src
  cd src
  git fetch -q origin nightly
  git checkout -q --detach "$base"
  cp "$pub/putgd-stable.asc" "$pub/putgd-stable.gpg" "$pub/putgd-stable.minisign.pub" packaging/linux/repo/keys/
  git add packaging/linux/repo/keys/putgd-stable.asc packaging/linux/repo/keys/putgd-stable.gpg packaging/linux/repo/keys/putgd-stable.minisign.pub
  labenv 2026-10-18T10:00:00Z git commit -q -m "lab: lab stable key in place of the real one (Linux update profile, never pushed)"
  : > "$OUT/tags.txt"
  for spec in $TAGS; do
    role=${spec%%:*} rest=${spec#*:}; date=${rest%:*} channel=${rest##*:}
    labenv "$date" git commit -q --allow-empty -m "lab: $role $channel build (Linux update profile, never pushed)"
    tag=$(tag_of "$channel")
    git tag -f "$tag" >/dev/null
    echo "$role $channel $tag $(git rev-parse HEAD)" | tee -a "$OUT/tags.txt"
  done
  ;;
native)
  label=$2 sdk=$3 runtime=${4:-}
  cd src
  mkdir -p build
  ln -sfn "$sdk" build/sdk
  ln -sfn "$(dirname "$sdk")/dependencies" build/dependencies
  format=DEB; ext=deb; [ "$label" = fedora44 ] && { format=RPM; ext=rpm; }
  extra=""; [ -n "$runtime" ] && extra="-DPUTGD_AWS_RUNTIME_DIR=$runtime"
  while read -r role channel tag commit; do
    git checkout -q --detach "$commit"
    nice -n 19 cmake --preset release -DBUILD_TESTING=OFF -DPUTGD_LAB_FIXTURE=OFF -DPUTGD_PHYSICAL_BENCHMARKS=OFF \
      -DPUTGD_RELEASE_TAG="$tag" -DPUTGD_BUNDLE_PROTON=OFF -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_PREFIX_PATH="$sdk" \
      -DPUTGD_PACKAGE_TARGET="$label" -DPUTGD_PACKAGE_REPOSITORY_URL=https://pkg.put.gd/lab $extra > "$OUT/configure-$label-$tag.log" 2>&1
    nice -n 19 cmake --build --preset release --parallel 3 > "$OUT/build-$label-$tag.log" 2>&1
    rm -rf build/release/_CPack_Packages
    (cd build/release && nice -n 19 cpack -G "$format" > "$OUT/cpack-$label-$tag.log" 2>&1)
    package=$(ls -t build/release/putgd[-_]*."$ext" | head -1)
    mkdir -p "$OUT/$tag"
    name=putgd-${tag#v}-$label-x86_64.$ext
    cp "$package" "$OUT/$tag/$name"; rm -f "$package"
    echo "$role $tag $commit $name $(sha256sum "$OUT/$tag/$name" | cut -c1-64)" | tee -a "$OUT/built-$label.txt"
  done < "$OUT/tags.txt"
  ;;
appimage-deps)
  # Its own clone of src (the lab tags come along), so native fedora44 builds on f44 can share src at the same time.
  [ -d appsrc/.git ] || git clone -q src appsrc
  podman container exists "$NAME" || podman run -d --name "$NAME" -v "$ROOT:/lab:Z" -w /lab/appsrc "$IMAGE" sleep infinity >/dev/null
  run() { podman exec -w /lab/appsrc -e DEBIAN_FRONTEND=noninteractive -e QT_ROOT_DIR=/lab/qt/6.8.3/gcc_64 "$NAME" bash -c "$1"; }
  run 'apt-get update -q && bash tools/releases/install-native-deps.sh'
  run 'apt-get install -y -q patchelf libgtk-3-dev libsecret-1-dev qt6-base-private-dev libx11-xcb-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-render-util0-dev libxcb-sync-dev
       apt-get install -y -q nasm libva-dev libvpx-dev libwebp-dev libopus-dev libffmpeg-nvenc-dev libx264-dev libx265-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-pipewire gstreamer1.0-pulseaudio gstreamer1.0-vaapi
       printf "Types: deb-src\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates noble-security\nComponents: main restricted universe multiverse\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n" > /etc/apt/sources.list.d/putgd-appimage-src.sources
       apt-get update -q && apt-get install -y -q --no-install-recommends weston python3-venv
       git config --global --add safe.directory /lab/appsrc'
  run '[ -x /lab/qt/6.8.3/gcc_64/bin/qmake ] || { python3 -m venv /lab/aqt && /lab/aqt/bin/pip install -q aqtinstall==3.3.0 && /lab/aqt/bin/aqt install-qt linux desktop 6.8.3 linux_gcc_64 -m qtimageformats -O /lab/qt; }'
  run '[ -d build/dependencies/aws-sdk-cpp ] || bash tools/ci/sdk-source.sh'
  run '[ -f build/sdk-cache.json ] || PUTGD_SDK_JOBS=3 nice -n 19 bash tools/releases/provision-sdk.sh'
  run 'nice -n 19 python3 tools/releases/provision-appimage.py --qt-root "$QT_ROOT_DIR"'
  run 'python3 tools/proton/provision_runtime.py'
  ;;
appimage)
  run() { podman exec -w /lab/appsrc -e DEBIAN_FRONTEND=noninteractive -e QT_ROOT_DIR=/lab/qt/6.8.3/gcc_64 "$NAME" bash -c "$1"; }
  while read -r role channel tag commit; do
    run "git checkout -q --detach $commit && rm -rf build/appimage-stage build/appimage-output &&
         RELEASE_TAG=$tag PUTGD_APPIMAGE_UPDATE_BASE=https://pkg.put.gd/lab/ PUTGD_BUILD_JOBS=3 nice -n 19 bash tools/releases/build-appimage.sh > /lab/out/appimage-$tag.log 2>&1 &&
         mkdir -p /lab/out/$tag && cp build/appimage-output/putgd-${tag#v}-x86_64.AppImage build/appimage-output/putgd-${tag#v}-x86_64.AppImage.zsync /lab/out/$tag/ &&
         echo $role $tag $commit \$(sha256sum /lab/out/$tag/putgd-${tag#v}-x86_64.AppImage | cut -c1-64) | tee -a /lab/out/built-appimage.txt"
  done < "$OUT/tags.txt"
  ;;
*) echo "usage: $0 commits BASE PUB | native LABEL SDK [AWS_RUNTIME_DIR] | appimage-deps | appimage" >&2; exit 2 ;;
esac
