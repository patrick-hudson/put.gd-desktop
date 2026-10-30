#!/bin/bash
set -euo pipefail
cd /home/user/snapshot-package-ubuntu26.04/source
python3 -c 'import os; s=os.statvfs("."); print(s.f_bavail*s.f_frsize); assert s.f_bavail*s.f_frsize>=10*1024**3'
git restore -- src/ui/SettingsWindow.cpp tests/gui/test_settings.cpp
git fetch /home/user/840-final-review.bundle HEAD
git checkout --detach FETCH_HEAD
test "$(git rev-parse HEAD)" = be8bc6b13f28afd03c835211f98f608909b84b6f
test -z "$(git status --porcelain --untracked-files=no)"
mkdir -m700 /home/user/840-native-final-review
sudo -n docker run --rm --name s840-native26-final-review -v "$PWD:/src" -v /home/user/840-native-final-review:/out -w /src putgd840-catalog-complete bash -c '
set -euo pipefail
export PATH=/usr/bin:/bin:$PATH CCACHE_DIR=/src/build/ccache LANG=C.UTF-8 LC_ALL=C.UTF-8
git config --global --add safe.directory /src
cmake --preset release -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DPUTGD_BUNDLE_PROTON=ON -DCMAKE_INSTALL_PREFIX=/usr -DPUTGD_PACKAGE_TARGET=ubuntu26.04
nice -n 10 cmake --build --preset release --parallel 4
python3 tools/releases/native.py --build build/release --format DEB --output /out/packages --proton-manifest build/dependencies/proton-runtime/manifest.json --require-proton --require-kde --package-target ubuntu26.04
cp build/release/build-identity/build-info.json /out/
sha256sum /out/packages/*.deb
'
