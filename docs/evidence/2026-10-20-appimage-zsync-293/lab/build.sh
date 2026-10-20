#!/usr/bin/env bash
# Runs inside the f44 ubuntu:24.04 container: one CI-equivalent AppImage build of
# the checked-out commit. Usage: build.sh LABEL (env: PUTGD_APPIMAGE_UPDATE_ORIGIN or RELEASE_TAG).
set -euxo pipefail
label=$1
cd /src/tree
export QT_ROOT_DIR=/opt/Qt/6.8.3/gcc_64
git log --oneline -1
# Only this lab's own previous outputs (build-appimage.sh requires new directories).
rm -rf build/appimage-stage build/appimage-output build/appimage-diagnostics
PUTGD_BUILD_JOBS=3 bash tools/releases/build-appimage.sh
mkdir -p "/src/out/$label"
cp build/appimage-output/*.AppImage build/appimage-output/appimage-build.json "/src/out/$label/"
cp build/appimage-output/*.AppImage.zsync "/src/out/$label/" 2>/dev/null || echo "no .zsync for $label"
echo "BUILD-DONE $label"
