#!/bin/bash
# stage.sh: on a builder, nightly 1058 (Qt 6.8.3) and its GNOME extension into ~/work/put.gd-qt418/afa/images, verified; appimageupdatetool.
set -eu
cd ~/work/put.gd-qt418/afa/images
R=https://github.com/patrick-hudson/put.gd-desktop/releases/download/v26.1023.1058-nightly
curl -fsSL -o SHA256SUMS.1058 $R/SHA256SUMS
for f in putgd-26.1023.1058-nightly-x86_64.AppImage putgd-gnome50-26.1023.1058-nightly.zip; do
  curl -fsSL -O $R/$f; grep " $f\$" SHA256SUMS.1058 | sha256sum -c -
done
chmod 755 putgd-26.1023.1058-nightly-x86_64.AppImage
curl -fsSL -o appimageupdatetool-x86_64.AppImage https://github.com/AppImageCommunity/AppImageUpdate/releases/download/continuous/appimageupdatetool-x86_64.AppImage
chmod 755 appimageupdatetool-x86_64.AppImage
sha256sum appimageupdatetool-x86_64.AppImage
