#!/bin/sh
# fetch_assets.sh LABEL TAG...: the published release files a host needs, checked against each release's SHA256SUMS,
# into /home/patrick/work/put.gd-ua/assets/TAG/; plus appimageupdatetool's continuous build (digest recorded).
set -eu
label=$1; shift
base=/home/patrick/work/put.gd-ua/assets
for tag in "$@"; do
  v=${tag#v}; d=$base/$tag; mkdir -p "$d"; cd "$d"
  url=https://github.com/patrick-hudson/put.gd-desktop/releases/download/$tag
  curl -fsSL -o SHA256SUMS "$url/SHA256SUMS"
  ext=deb; [ "$label" = fedora44 ] && ext=rpm
  for f in putgd-$v-x86_64.AppImage putgd-$v-$label-x86_64.$ext; do
    [ -f "$f" ] || curl -fsSL -o "$f" "$url/$f"
    grep " \*\?$f\$" SHA256SUMS | sed 's/ \*/  /' | sha256sum -c -
  done
  chmod 755 putgd-$v-x86_64.AppImage
done
cd $base
[ -f appimageupdatetool-x86_64.AppImage ] || curl -fsSL -o appimageupdatetool-x86_64.AppImage \
  https://github.com/AppImageCommunity/AppImageUpdate/releases/download/continuous/appimageupdatetool-x86_64.AppImage
chmod 755 appimageupdatetool-x86_64.AppImage
sha256sum appimageupdatetool-x86_64.AppImage
