#!/bin/sh
# calver-labbuild.sh (#373 lab acceptance): a native release package of the
# CalVer lab nightly N, built on its target host as package.yml builds it, for
# the signer's lab prefix (https://pkg.put.gd/lab). N is one empty lab commit on
# the PR head, made with a fixed identity and date so every host gets the same
# commit; its tag is local and never pushed.
#   calver-labbuild.sh HEAD LABEL SDK [AWS_RUNTIME_DIR]
set -eu
head=$1; label=$2; sdk=$3; runtime=${4:-}
root=$HOME/work/put.gd-calver373-lab
out=$root/out
mkdir -p "$out"
[ -d "$root/src/.git" ] || git clone -q --branch claude/calver-373-03743c https://github.com/patrick-hudson/put.gd-desktop.git "$root/src"
cd "$root/src"
git fetch -q origin claude/calver-373-03743c
git checkout -q --detach "$head"
GIT_AUTHOR_NAME=lab GIT_AUTHOR_EMAIL=lab@example.invalid GIT_AUTHOR_DATE=2026-10-22T12:00:00Z \
GIT_COMMITTER_NAME=lab GIT_COMMITTER_EMAIL=lab@example.invalid GIT_COMMITTER_DATE=2026-10-22T12:00:00Z \
  git commit -q --allow-empty -m "lab: #373 CalVer nightly N (never pushed)"
base=$(python3 -c 'import sys; sys.path.insert(0, "tools/releases"); import subprocess
from release_identity import calendar_base
t = int(subprocess.check_output(["git", "show", "-s", "--format=%ct", "HEAD"]))
n = int(subprocess.check_output(["git", "rev-list", "--count", "HEAD"]))
print(calendar_base(t, n))')
tag=v$base-nightly
git tag -f "$tag" >/dev/null
mkdir -p build
ln -sfn "$sdk" build/sdk
ln -sfn "$(dirname "$sdk")/dependencies" build/dependencies
format=DEB; ext=deb; [ "$label" = fedora44 ] && { format=RPM; ext=rpm; }
extra=""; [ -n "$runtime" ] && extra="-DPUTGD_AWS_RUNTIME_DIR=$runtime"
nice -n 19 cmake --preset release -DBUILD_TESTING=OFF -DPUTGD_LAB_FIXTURE=OFF -DPUTGD_PHYSICAL_BENCHMARKS=OFF \
  -DPUTGD_RELEASE_TAG="$tag" -DPUTGD_BUNDLE_PROTON=OFF -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_PREFIX_PATH="$sdk" \
  -DPUTGD_PACKAGE_TARGET="$label" -DPUTGD_PACKAGE_REPOSITORY_URL=https://pkg.put.gd/lab $extra > "$out/configure.log" 2>&1
nice -n 19 cmake --build --preset release --parallel 2 > "$out/build.log" 2>&1
rm -rf build/release/_CPack_Packages
(cd build/release && nice -n 19 cpack -G "$format" > "$out/cpack.log" 2>&1)
package=$(ls -t build/release/putgd[-_]*."$ext" | head -1)
name=putgd-${tag#v}-$label-x86_64.$ext
cp "$package" "$out/$name"
rm -f "$package"
echo "$tag $(git rev-parse HEAD) $name $(sha256sum "$out/$name" | cut -c1-64)" > "$out/built.txt"
cat "$out/built.txt"
