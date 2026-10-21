#!/bin/bash
# repack.sh SRC.AppImage: re-wrap one CI AppImage with lab-origin update information (#306/#307),
# like the #295 repack: the pinned appimagetool 1.9.1, type2 runtime 20251108, mksquashfs -b 16384
# and its bundled zsyncmake 0.6.2. Nothing else changes; the AppDir root is listed to show #306.
set -euo pipefail
ROOT=/home/patrick/work/put.gd-r12aiu/appimage ORIGIN=http://127.0.0.1:8765 SRC=$1
cd "$ROOT"; mkdir -p tools out
get() { [ -f "tools/$1" ] || curl -fsSL -o "tools/$1" "$2"; echo "$3  tools/$1" | sha256sum -c - >/dev/null; chmod +x "tools/$1"; }
get appimagetool-x86_64.AppImage https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
get runtime-x86_64 https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
if [ ! -f tools/zsyncmake ]; then (cd tools && APPIMAGE_EXTRACT_AND_RUN=1 ./appimagetool-x86_64.AppImage --appimage-extract usr/bin/zsyncmake >/dev/null && cp squashfs-root/usr/bin/zsyncmake . && rm -rf squashfs-root); fi
echo "4fa6642a3bca4d1ba038b3eeadc249ab5fabc252a9973bdb86321544f9f7e098  tools/zsyncmake" | sha256sum -c - >/dev/null
name=$(basename "$SRC"); work=$(mktemp -d "$ROOT/x.XXXXXX")
(cd "$work" && "$SRC" --appimage-extract >/dev/null)
echo "AppDir root desktop entries of $name:"; (cd "$work/squashfs-root" && ls -l -- *.desktop)
(cd out && ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 ../tools/appimagetool-x86_64.AppImage --runtime-file ../tools/runtime-x86_64 \
   --mksquashfs-opt=-b --mksquashfs-opt=16384 -u "zsync|$ORIGIN/appimage.zsync" "$work/squashfs-root" "$name" >/dev/null 2>&1)
rm -rf "$work"; rm -f "out/$name.zsync"
(cd out && ../tools/zsyncmake -u "$ORIGIN/$name" -o "$name.zsync" "$name" >/dev/null)
python3 -c "import sys; sys.path.insert(0,'$ROOT/releases'); import appimage_update as u; print(u.read_update_information('out/$name')); f=u.verify_zsync('out/$name.zsync','out/$name','$name','$ORIGIN/$name'); print(f['URL'], f['Length'], f['SHA-1'])"
sha256sum "out/$name"
