#!/bin/bash
# Build PR 726 head on the sandbox: dev preset with the lab fixture, DEB, UI catalog.
set -u
log=~/sd726; mkdir -p $log; exec > $log/build.log 2>&1
echo "start $(date -u +%FT%TZ)"
sudo -n DEBIAN_FRONTEND=noninteractive apt-get install -y -q tesseract-ocr python3-pytest >/dev/null && echo apt-ok
tesseract --version | head -1
cd ~/work/put.gd/src
git fetch -q https://github.com/patrick-hudson/put.gd-desktop.git feat/surface-driver-core-2c && git checkout -q --detach FETCH_HEAD
git log --oneline -1; git status --short | head
export PATH=/usr/bin:$PATH
nice -n 10 cmake --preset dev -DPUTGD_LAB_FIXTURE=ON > $log/configure.log 2>&1; echo configure=$?
nice -n 10 cmake --build --preset dev -j7 > $log/compile.log 2>&1; echo build=$?
out=$(mktemp -d "$PWD/build/packages-sd726-XXXXXXXX")
(umask 022; cpack --config build/dev/CPackConfig.cmake -G DEB -B "$out" > $log/cpack.log 2>&1); echo cpack=$?
ls $out/*.deb && sha256sum $out/*.deb | tee $log/deb.sha256
rm -rf $log/catalog; mkdir -p $log/catalog
QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR="$(mktemp -d)" build/dev/putgd-ui-catalog $log/catalog > $log/catalog.log 2>&1; echo catalog=$?
python3 tools/lab/surface_map/surface_map.py check --catalog $log/catalog >> $log/catalog.log 2>&1; echo catalog_check=$?
python3 -c "import json;m=json.load(open('$log/catalog/manifest.json'));print('catalog build',m.get('build',{}).get('gitCommit'),m.get('build',{}).get('dirty'),len(m['captures']))"
cat build/dev/build-identity/build-info.json
echo "end $(date -u +%FT%TZ)"
echo DONE
