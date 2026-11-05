#!/bin/sh
# Lab packages N, N+1, N+2 for #293 phase 2: local nightly tags (never pushed) on
# lab commits, built against the lab repository prefix. Usage: labbuild.sh LABEL SDK [AWS_RUNTIME_DIR]
set -eu
label=$1; sdk=$2; runtime=${3:-}
root=$HOME/work/put.gd-pkg293-lab
out=$root/out
cd "$root/src"
mkdir -p build "$out"
ln -sfn "$sdk" build/sdk
ln -sfn "$(dirname "$sdk")/dependencies" build/dependencies
format=DEB; [ "$label" = fedora44 ] && format=RPM
extra=""; [ -n "$runtime" ] && extra="-DPUTGD_AWS_RUNTIME_DIR=$runtime"
start=${START:-0}
while [ "$(git rev-list --count HEAD)" -lt "$start" ]; do git -c user.name=lab -c user.email=lab@example.invalid commit -q --allow-empty -m "lab: #293 acceptance base (never pushed)"; done
for step in 0 1 2; do
  if [ "$step" -gt 0 ]; then
    git -c user.name=lab -c user.email=lab@example.invalid commit -q --allow-empty -m "lab: #293 acceptance N+$step (never pushed)"
  fi
  count=$(git rev-list --count HEAD)
  tag=v0.1.1-nightly.$count
  git tag -f "$tag" >/dev/null
  cmake --preset release -DBUILD_TESTING=OFF -DPUTGD_LAB_FIXTURE=OFF -DPUTGD_PHYSICAL_BENCHMARKS=OFF \
    -DPUTGD_RELEASE_TAG="$tag" -DPUTGD_BUNDLE_PROTON=OFF -DCMAKE_INSTALL_PREFIX=/usr \
    -DPUTGD_PACKAGE_TARGET="$label" -DPUTGD_PACKAGE_REPOSITORY_URL=https://pkg.put.gd/lab $extra > "$out/cfg-$count.log" 2>&1
  cmake --build --preset release --parallel 4 > "$out/build-$count.log" 2>&1
  rm -rf build/release/_CPack_Packages
  (cd build/release && cpack -G "$format" > "$out/cpack-$count.log" 2>&1)
  ext=deb; [ "$format" = RPM ] && ext=rpm
  package=$(ls -t build/release/putgd[-_]*."$ext" | head -1)
  cp "$package" "$out/putgd-0.1.1-nightly.$count-$label-x86_64.$ext"
  rm -f "$package"
  echo "$tag $(sha256sum "$out/putgd-0.1.1-nightly.$count-$label-x86_64.$ext" | cut -c1-64)" >> "$out/built.txt"
done
echo done > "$out/rc"
