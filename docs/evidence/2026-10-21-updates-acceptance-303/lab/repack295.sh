#!/bin/bash
# repack295.sh: re-wrap CI AppImages N and N+1 with lab-origin update information (#295).
# Same tools and options as tools/releases/portable_appimage.py: the pinned appimagetool 1.9.1,
# type2 runtime 20251108, mksquashfs -b 16384, and its bundled zsyncmake 0.6.2.
set -euo pipefail
ROOT=/home/patrick/work/put.gd-303/appimage; ORIGIN=http://127.0.0.1:8765
cd "$ROOT"; mkdir -p tools out
get() { [ -f "tools/$1" ] || curl -fsSL -o "tools/$1" "$2"; echo "$3  tools/$1" | sha256sum -c -; chmod +x "tools/$1"; }
get appimagetool-x86_64.AppImage https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
get runtime-x86_64 https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
if [ ! -f tools/zsyncmake ]; then (cd tools && ./appimagetool-x86_64.AppImage --appimage-extract usr/bin/zsyncmake >/dev/null && cp squashfs-root/usr/bin/zsyncmake . && rm -rf squashfs-root); fi
echo "4fa6642a3bca4d1ba038b3eeadc249ab5fabc252a9973bdb86321544f9f7e098  tools/zsyncmake" | sha256sum -c -
for src in n/*.AppImage n1/*.AppImage; do
  name=$(basename "$src"); work=$(mktemp -d "$ROOT/x.XXXXXX")
  (cd "$work" && "$ROOT/$src" --appimage-extract >/dev/null)
  (cd out && ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 ../tools/appimagetool-x86_64.AppImage --runtime-file ../tools/runtime-x86_64 \
     --mksquashfs-opt=-b --mksquashfs-opt=16384 -u "zsync|$ORIGIN/appimage.zsync" "$work/squashfs-root" "$name" >/dev/null 2>&1)
  rm -rf "$work"; rm -f "out/$name.zsync"
  (cd out && ../tools/zsyncmake -u "$ORIGIN/$name" -o "$name.zsync" "$name" >/dev/null)
  python3 -c "import sys; sys.path.insert(0,'/home/patrick/work/put.gd-303/src-head/tools/releases'); import appimage_update as u; print(u.read_update_information('out/$name')); f=u.verify_zsync('out/$name.zsync','out/$name','$name','$ORIGIN/$name'); print(f['URL'], f['Length'], f['SHA-1'])"
done
sha256sum out/*.AppImage
