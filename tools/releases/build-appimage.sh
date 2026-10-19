#!/usr/bin/env bash
set -euo pipefail
: "${QT_ROOT_DIR:?Official Qt 6.8.3 root is required}"
release_tag=${RELEASE_TAG:-}
if [ "${GITHUB_EVENT_NAME:-}" = 'pull_request' ]; then
  release_tag=
fi
# The AppImage bundles its own runtime, including putgd-record's GStreamer
# plugins and the pinned FFmpeg from build/appimage-deps (packaging/appimage/media.json).
cmake --preset release -DPUTGD_BUNDLE_PROTON=ON -DBUILD_TESTING=OFF -DPUTGD_LAB_FIXTURE=OFF -DPUTGD_PHYSICAL_BENCHMARKS=OFF -DPUTGD_RECORDING=ON \
  -DPUTGD_RELEASE_TAG="$release_tag" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_PREFIX_PATH="$PWD/build/appimage-deps;$QT_ROOT_DIR;$PWD/build/sdk"
cmake --build --preset release --parallel "${PUTGD_BUILD_JOBS:-2}"
# The helper must link the pinned FFmpeg, never the builder's distro libraries.
if ! readelf -d build/release/putgd-record | grep -q 'libavcodec\.so\.61'; then
  echo 'putgd-record is not linked against the pinned FFmpeg in build/appimage-deps' >&2; exit 1
fi
stage="$PWD/build/appimage-stage"
if [ -e "$stage" ]; then echo 'AppImage staging directory must be new' >&2; exit 1; fi
# FFmpeg's libraries carry no RUNPATH, so their own dependencies (libswresample
# under libavcodec) resolve from the prefix only through the loader path while
# the install step checks each executable's identity.
LD_LIBRARY_PATH="$PWD/build/appimage-deps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" DESTDIR="$stage" cmake --install build/release
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
  --license-source build/dependencies/appimage/layershell \
  --license-source build/dependencies/appimage/ffmpeg
# The image is a GPL-2.0-or-later combined work (bundled x264/x265 and a GPL
# FFmpeg); its corresponding source is a release asset of its own.
python3 tools/releases/appimage_media_sources.py --receipt build/appimage-output/appimage-build.json \
  --ffmpeg-source build/dependencies/appimage/ffmpeg --output-dir build/appimage-output \
  --timestamp "$(git show -s --format=%ct HEAD)"
