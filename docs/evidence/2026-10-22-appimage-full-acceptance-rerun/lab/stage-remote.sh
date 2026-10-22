#!/bin/bash
# Runs on a builder: download N (942), N-1 (925), the GNOME extension and appimageupdatetool into ~/work/put.gd-afa2/images, verify.
set -eu
cd ~/work/put.gd-afa2/images
R=https://github.com/patrick-hudson/put.gd-desktop/releases/download
for t in 942 925; do
  curl -fsSL -o SHA256SUMS.$t $R/v0.1.1-nightly.$t/SHA256SUMS
  curl -fsSL -O $R/v0.1.1-nightly.$t/putgd-0.1.1-nightly.$t-x86_64.AppImage
  chmod 755 putgd-0.1.1-nightly.$t-x86_64.AppImage
  grep " putgd-0.1.1-nightly.$t-x86_64.AppImage\$" SHA256SUMS.$t | sha256sum -c -
done
curl -fsSL -O $R/v0.1.1-nightly.942/putgd-gnome50-0.1.1-nightly.942.zip
grep " putgd-gnome50-0.1.1-nightly.942.zip\$" SHA256SUMS.942 | sha256sum -c -
curl -fsSL -o appimageupdatetool-x86_64.AppImage https://github.com/AppImageCommunity/AppImageUpdate/releases/download/continuous/appimageupdatetool-x86_64.AppImage
chmod 755 appimageupdatetool-x86_64.AppImage
sha256sum appimageupdatetool-x86_64.AppImage putgd-0.1.1-nightly.942-x86_64.AppImage
