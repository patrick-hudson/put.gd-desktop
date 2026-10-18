#!/usr/bin/env bash
set -euo pipefail
: "${QT_ROOT_DIR:?Official Qt 6.8.3 root is required}"
release_tag=${RELEASE_TAG:-}
if [ "${GITHUB_EVENT_NAME:-}" = 'pull_request' ]; then
  release_tag=
fi
cmake --preset release -DPUTGD_BUNDLE_PROTON=ON -DBUILD_TESTING=OFF -DPUTGD_LAB_FIXTURE=OFF -DPUTGD_PHYSICAL_BENCHMARKS=OFF \
  -DPUTGD_RELEASE_TAG="$release_tag" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_PREFIX_PATH="$PWD/build/appimage-deps;$QT_ROOT_DIR;$PWD/build/sdk"
cmake --build --preset release --parallel "${PUTGD_BUILD_JOBS:-2}"
stage="$PWD/build/appimage-stage"
if [ -e "$stage" ]; then echo 'AppImage staging directory must be new' >&2; exit 1; fi
DESTDIR="$stage" cmake --install build/release
version=$(python3 -c 'import json; print(json.load(open("build/release/build-identity/build-info.json"))["version"])')
python3 tools/releases/portable_appimage.py --stage "$stage" --output-dir "$PWD/build/appimage-output" \
  --require-proton --proton-manifest build/dependencies/proton-runtime/manifest.json \
  --tools-dir "$PWD/build/appimage-tools" --qt-root "$QT_ROOT_DIR" \
  --dependency-prefix "$PWD/build/appimage-deps" --version "$version" \
  --license-source build/dependencies/appimage/qtbase \
  --license-source build/dependencies/appimage/qtwayland \
  --license-source build/dependencies/appimage/qtdeclarative \
  --license-source build/dependencies/appimage/qtsvg \
  --license-source build/dependencies/appimage/qtimageformats \
  --license-source build/dependencies/appimage/qtkeychain \
  --license-source build/dependencies/appimage/layershell
