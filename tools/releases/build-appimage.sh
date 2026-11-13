#!/usr/bin/env bash
set -euo pipefail
: "${QT_ROOT_DIR:?The official Qt root of packaging/appimage/tools.lock.json is required}"
release_tag=${RELEASE_TAG:-}
if [ "${GITHUB_EVENT_NAME:-}" = 'pull_request' ]; then
  release_tag=
fi
# The AppImage bundles its own runtime, including putgd-record's GStreamer
# plugins and the pinned FFmpeg from build/appimage-deps (packaging/appimage/media.json).
# Its file-manager entries are per-user files, not the native packages' plugins (#1105).
cmake --preset release -DPUTGD_OCR=ON -DPUTGD_BUNDLE_PROTON=ON -DBUILD_TESTING=OFF -DPUTGD_LAB_FIXTURE=OFF -DPUTGD_PHYSICAL_BENCHMARKS=OFF -DPUTGD_RECORDING=ON \
  -DPUTGD_NAUTILUS_PLUGIN=OFF -DPUTGD_CAJA_PLUGIN=OFF -DPUTGD_PEONY_PLUGIN=OFF \
  -DPUTGD_RELEASE_TAG="$release_tag" -DPUTGD_PACKAGE_TARGET=appimage -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_PREFIX_PATH="$PWD/build/appimage-deps;$QT_ROOT_DIR;$PWD/build/sdk"
cmake --build --preset release --parallel "${PUTGD_BUILD_JOBS:-2}"
# The helper must link the pinned FFmpeg, never the builder's distro libraries.
# Each library's soname comes from the pinned build itself, so a new FFmpeg major
# needs no edit here; a library missing from the prefix or linked at another
# soname still fails. pkg-config resolves each library to a full path on the
# helper's link line, so that path must be the pinned copy too: a distro FFmpeg
# with the same sonames would otherwise pass.
ffmpeg_lib="$PWD/build/appimage-deps/lib"
record_needed=$(readelf -d build/release/putgd-record | sed -n 's/.*(NEEDED).*\[\(.*\)\]$/\1/p')
record_link=$(awk '/^build putgd-record:/ { found = 1 } found && /^  LINK_LIBRARIES = / { print; exit }' build/release/build.ninja | tr ' ' '\n')
if [ -z "$record_link" ]; then
  echo 'build/release/build.ninja has no link line for putgd-record' >&2; exit 1
fi
for library in avcodec avformat avfilter avutil swscale swresample; do
  soname=$(readelf -d "$ffmpeg_lib/lib$library.so" 2>/dev/null | sed -n 's/.*(SONAME).*\[\(.*\)\]$/\1/p' || true)
  if [ -z "$soname" ] || [ ! -f "$ffmpeg_lib/$soname" ]; then
    echo "The pinned FFmpeg in build/appimage-deps has no lib$library" >&2; exit 1
  fi
  if ! grep -qxF "$soname" <<<"$record_needed"; then
    echo "putgd-record is not linked against the pinned FFmpeg $soname in build/appimage-deps" >&2; exit 1
  fi
  link_inputs=$(grep -E "/lib$library\.so(\.[0-9.]+)?$" <<<"$record_link" || true)
  pinned=$(realpath -e "$ffmpeg_lib/lib$library.so")
  if [ -z "$link_inputs" ] || [ "$(xargs -r realpath -e <<<"$link_inputs" | sort -u)" != "$pinned" ]; then
    echo "putgd-record links lib$library from ${link_inputs:-nowhere}, not the pinned $ffmpeg_lib/lib$library.so" >&2; exit 1
  fi
done
# The update check offers the AppImage download (#293).
if ! build/release/putgd-cli --update-info | grep -q '"packageTarget":"appimage"'; then
  echo 'putgd-cli was not built for the AppImage update target' >&2; exit 1
fi
stage="$PWD/build/appimage-stage"
if [ -e "$stage" ]; then echo 'AppImage staging directory must be new' >&2; exit 1; fi
# FFmpeg's libraries carry no RUNPATH, so their own dependencies (libswresample
# under libavcodec) resolve from the prefix only through the loader path while
# the install step checks each executable's identity.
LD_LIBRARY_PATH="$PWD/build/appimage-deps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" DESTDIR="$stage" cmake --install build/release
version=$(python3 -c 'import json; print(json.load(open("build/release/build-identity/build-info.json"))["version"])')
# Tagged builds embed their channel's update information and write the
# matching .zsync (tools/releases/appimage_update.py); untagged builds embed
# nothing. PUTGD_APPIMAGE_UPDATE_ORIGIN is for lab update tests only: it points
# both at a local server, and collect.py rejects such an image.
update=()
if [ -n "${PUTGD_APPIMAGE_UPDATE_ORIGIN:-}" ]; then
  update=(--update-origin "$PUTGD_APPIMAGE_UPDATE_ORIGIN")
elif [ -n "$release_tag" ]; then
  update=(--release-tag "$release_tag")
  # Lab only: a tagged build for the signer's lab scope (#293 phase 3).
  if [ -n "${PUTGD_APPIMAGE_UPDATE_BASE:-}" ]; then
    update+=(--update-base "$PUTGD_APPIMAGE_UPDATE_BASE")
  fi
fi
python3 tools/releases/portable_appimage.py --stage "$stage" --output-dir "$PWD/build/appimage-output" \
  --require-proton --proton-manifest build/dependencies/proton-runtime/manifest.json \
  --tools-dir "$PWD/build/appimage-tools" --qt-root "$QT_ROOT_DIR" \
  --dependency-prefix "$PWD/build/appimage-deps" --version "$version" "${update[@]}" \
  --license-source build/dependencies/appimage/qtbase \
  --license-source build/dependencies/appimage/qtwayland \
  --license-source build/dependencies/appimage/qtsvg \
  --license-source build/dependencies/appimage/qtimageformats \
  --license-source build/dependencies/appimage/qttranslations \
  --license-source build/dependencies/appimage/icu \
  --license-source build/dependencies/appimage/qtkeychain \
  --license-source build/dependencies/appimage/layershell \
  --license-source build/dependencies/appimage/ffmpeg \
  --license-source build/dependencies/appimage/mimalloc
# The image's own putgd-record must run the lab diagnostics (docs/design/recording.md)
# on every export format: headless, on the extracted image, nothing captured.
images=("$PWD"/build/appimage-output/*.AppImage)
if [ "${#images[@]}" -ne 1 ] || [ ! -f "${images[0]}" ]; then echo 'Expected one built AppImage' >&2; exit 1; fi
diagnostics="$PWD/build/appimage-diagnostics"
mkdir "$diagnostics"
(cd "$diagnostics" && "${images[0]}" --appimage-extract > /dev/null)
python3 tools/releases/appimage_diagnostics.py --appdir "$diagnostics/squashfs-root" \
  --output build/appimage-output/appimage-diagnostics.json
# The built image's own notices, inventory and content (#417, #421).
python3 tools/releases/appimage_notices.py --appdir "$diagnostics/squashfs-root" \
  --output build/appimage-output/appimage-notices.json
rm -rf "$diagnostics"
# The image is a GPL-2.0-or-later combined work (bundled x264/x265 and a GPL
# FFmpeg); its corresponding source is a linked download. CI collects it in a
# separate required job so image checks do not wait on source downloads.
if [ "${PUTGD_APPIMAGE_SOURCES:-1}" = 1 ]; then
  python3 tools/releases/appimage_complete_sources.py --receipt build/appimage-output/appimage-build.json \
    --output-dir build/appimage-output \
    --timestamp "$(git show -s --format=%ct HEAD)"
fi
