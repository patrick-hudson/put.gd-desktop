#!/bin/bash
# appimage-lab-build.sh (#344 lab acceptance): build the lab AppImages for the
# channel switch from the PR head, as package.yml's appimage job builds, in a
# long-lived pinned ubuntu:24.04 podman container on f44:
#   stable  v0.1.1               (lab commit L1)
#   nightly v0.1.1-nightly.9101  (empty commits on L1 up to that count)
#   nightly v0.1.1-nightly.9102
# L1 is the PR head plus one local commit that replaces
# packaging/linux/repo/keys/putgd-stable.minisign.pub with the lab-only stable
# key, so the lab images trust the lab stable feed; the nightly key stays the real
# one (the signer's lab mode signs lab nightlies with it). Tags and lab commits
# stay in this clone and are never pushed. The images embed the signer's lab
# layout (PUTGD_APPIMAGE_UPDATE_BASE=https://pkg.put.gd/lab/).
#   appimage-lab-build.sh init HEAD LABPUB   clone, lab commits, container, apt deps
#   appimage-lab-build.sh provision          copy provisioned Qt/SDK/AppImage deps from FROM (read only)
#   appimage-lab-build.sh build TAG          build one tag
set -euo pipefail
LAB=/home/patrick/work/put.gd-i344lab
NAME=putgd-i344lab
IMAGE=docker.io/library/ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
BRANCH=claude/appimage-channel-344
FROM=${FROM:-/home/patrick/work/put.gd-u348lab}
mkdir -p "$LAB"
cd "$LAB"
run() { podman exec -w /lab/src -e DEBIAN_FRONTEND=noninteractive -e QT_ROOT_DIR=/lab/qt/6.8.3/gcc_64 "$NAME" bash -c "$1"; }
case "$1" in
init)
  head=$2 labpub=$3
  [ -d src/.git ] || git clone -q --branch "$BRANCH" https://github.com/patrick-hudson/put.gd-desktop.git src
  git -C src fetch -q origin "$BRANCH"
  git -C src checkout -q -B lab344 "$head"
  git -C src -c user.name='put.gd lab' -c user.email=lab@put.gd.invalid rev-parse HEAD >/dev/null
  cp "$labpub" src/packaging/linux/repo/keys/putgd-stable.minisign.pub
  git -C src -c user.name='put.gd lab' -c user.email=lab@put.gd.invalid commit -q -am 'lab: lab-only stable minisign key (#344 acceptance, never pushed)'
  l1=$(git -C src rev-parse HEAD)
  git -C src tag -f v0.1.1 "$l1" >/dev/null
  # Empty commits (same tree) up to the nightly numbers the signer's lab scope uses.
  tree=$(git -C src rev-parse "$l1^{tree}") parent=$l1 count=$(git -C src rev-list --count "$l1")
  while [ "$count" -lt 9102 ]; do
    parent=$(GIT_AUTHOR_NAME='put.gd lab' GIT_AUTHOR_EMAIL=lab@put.gd.invalid GIT_COMMITTER_NAME='put.gd lab' GIT_COMMITTER_EMAIL=lab@put.gd.invalid \
      git -C src commit-tree "$tree" -p "$parent" -m "lab: empty commit $((count + 1)) (#344)")
    count=$((count + 1))
    [ "$count" -eq 9101 ] && git -C src tag -f v0.1.1-nightly.9101 "$parent" >/dev/null
    [ "$count" -eq 9102 ] && git -C src tag -f v0.1.1-nightly.9102 "$parent" >/dev/null
  done
  git -C src log --oneline -1 v0.1.1; git -C src log --oneline -1 v0.1.1-nightly.9101; git -C src log --oneline -1 v0.1.1-nightly.9102
  if ! podman container exists "$NAME"; then
    podman run -d --name "$NAME" -v "$LAB:/lab:Z" -w /lab/src "$IMAGE" sleep infinity >/dev/null
  fi
  run 'apt-get update -q && bash tools/releases/install-native-deps.sh'
  run 'apt-get install -y -q patchelf libgtk-3-dev libsecret-1-dev qt6-base-private-dev libx11-xcb-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-render-util0-dev libxcb-sync-dev
       apt-get install -y -q nasm libva-dev libvpx-dev libwebp-dev libopus-dev libffmpeg-nvenc-dev libx264-dev libx265-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-pipewire gstreamer1.0-pulseaudio gstreamer1.0-vaapi
       printf "Types: deb-src\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates noble-security\nComponents: main restricted universe multiverse\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n" > /etc/apt/sources.list.d/putgd-appimage-src.sources
       apt-get update -q && apt-get install -y -q --no-install-recommends weston python3-venv
       git config --global --add safe.directory /lab/src'
  ;;
provision)
  # The same pinned inputs another lab tree already provisioned; copied, never changed there.
  [ -d qt ] || cp -a --reflink=auto "$FROM/qt" qt
  for d in sdk aws dependencies appimage-deps appimage-tools appimage-ecm appimage-ffmpeg appimage-layershell appimage-qtkeychain sdk-cache.json; do
    [ -e "$FROM/src/build/$d" ] && [ ! -e "src/build/$d" ] && mkdir -p src/build && cp -a --reflink=auto "$FROM/src/build/$d" "src/build/$d"
  done
  ls src/build
  ;;
build)
  tag=$2 ver=${2#v}
  run "git checkout -q --detach $tag && rm -rf build/appimage-stage build/appimage-output build/appimage-diagnostics &&
       RELEASE_TAG=$tag PUTGD_APPIMAGE_UPDATE_BASE=https://pkg.put.gd/lab/ PUTGD_BUILD_JOBS=4 nice -n 10 bash tools/releases/build-appimage.sh &&
       mkdir -p /lab/out/$tag && cp build/appimage-output/putgd-$ver-x86_64.AppImage build/appimage-output/putgd-$ver-x86_64.AppImage.zsync /lab/out/$tag/ &&
       cd /lab/out/$tag && sha256sum putgd-$ver-x86_64.AppImage putgd-$ver-x86_64.AppImage.zsync > SHA256SUMS && cat SHA256SUMS && echo built $tag"
  ;;
*) echo "usage: $0 init HEAD LABPUB | provision | build TAG" >&2; exit 2 ;;
esac
