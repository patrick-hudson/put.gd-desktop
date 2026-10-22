#!/bin/sh
# lab386-build.sh (#386 lab acceptance): native release packages of the PR head
# for the signer's lab prefix (https://pkg.put.gd/lab), built on the target host
# as package.yml builds them. On top of HEAD:
#   1. a lab-key commit: packaging/linux/repo/keys/putgd-stable.{asc,gpg} become
#      the lab stable key's public halves from KEYS, so every package's stable
#      repository file trusts the lab key, never the real stable key;
#   2. four empty commits, each tagged locally (never pushed) and built in turn:
#      nightly N (10-19), stable S1 (10-18, older than N), stable S2 (10-20) and
#      nightly N1 (10-21).
# Fixed identities and dates, so m74 and f44 make the same commits and versions.
#   lab386-build.sh HEAD LABEL SDK KEYS [AWS_RUNTIME_DIR]
set -eu
head=$1; label=$2; sdk=$3; keys=$4; runtime=${5:-}
root=$HOME/work/put.gd-i386lab
out=$root/out
mkdir -p "$out"
[ -d "$root/src/.git" ] || git clone -q --branch claude/deb-rpm-channel-386 https://github.com/patrick-hudson/put.gd-desktop.git "$root/src"
cd "$root/src"
git fetch -q origin claude/deb-rpm-channel-386
git checkout -q --detach "$head"
commit() { # date message
  GIT_AUTHOR_NAME=lab GIT_AUTHOR_EMAIL=lab@example.invalid GIT_AUTHOR_DATE=$1 \
  GIT_COMMITTER_NAME=lab GIT_COMMITTER_EMAIL=lab@example.invalid GIT_COMMITTER_DATE=$1 \
    git commit -q --allow-empty -m "$2"
}
cp "$keys/putgd-stable.asc" "$keys/putgd-stable.gpg" packaging/linux/repo/keys/
git add packaging/linux/repo/keys/putgd-stable.asc packaging/linux/repo/keys/putgd-stable.gpg
commit 2026-10-19T10:00:00Z "lab: #386 lab stable key in place of the real one (never pushed)"
mkdir -p build
ln -sfn "$sdk" build/sdk
ln -sfn "$(dirname "$sdk")/dependencies" build/dependencies
format=DEB; ext=deb; [ "$label" = fedora44 ] && { format=RPM; ext=rpm; }
extra=""; [ -n "$runtime" ] && extra="-DPUTGD_AWS_RUNTIME_DIR=$runtime"
: > "$out/built.txt"
for spec in 2026-10-19T12:00:00Z:nightly 2026-10-18T12:00:00Z:stable 2026-10-20T12:00:00Z:stable 2026-10-21T12:00:00Z:nightly; do
  date=${spec%%:nightly}; date=${date%%:stable}; channel=${spec##*:}
  commit "$date" "lab: #386 $channel build (never pushed)"
  base=$(python3 -c 'import sys; sys.path.insert(0, "tools/releases"); import subprocess
from release_identity import calendar_base
t = int(subprocess.check_output(["git", "show", "-s", "--format=%ct", "HEAD"]))
n = int(subprocess.check_output(["git", "rev-list", "--count", "HEAD"]))
print(calendar_base(t, n))')
  tag=v$base; [ "$channel" = nightly ] && tag=v$base-nightly
  git tag -f "$tag" >/dev/null
  nice -n 19 cmake --preset release -DBUILD_TESTING=OFF -DPUTGD_LAB_FIXTURE=OFF -DPUTGD_PHYSICAL_BENCHMARKS=OFF \
    -DPUTGD_RELEASE_TAG="$tag" -DPUTGD_BUNDLE_PROTON=OFF -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_PREFIX_PATH="$sdk" \
    -DPUTGD_PACKAGE_TARGET="$label" -DPUTGD_PACKAGE_REPOSITORY_URL=https://pkg.put.gd/lab $extra > "$out/configure-$tag.log" 2>&1
  nice -n 19 cmake --build --preset release --parallel 3 > "$out/build-$tag.log" 2>&1
  rm -rf build/release/_CPack_Packages
  (cd build/release && nice -n 19 cpack -G "$format" > "$out/cpack-$tag.log" 2>&1)
  package=$(ls -t build/release/putgd[-_]*."$ext" | head -1)
  name=putgd-${tag#v}-$label-x86_64.$ext
  cp "$package" "$out/$name"
  rm -f "$package"
  echo "$tag $(git rev-parse HEAD) $name $(sha256sum "$out/$name" | cut -c1-64)" | tee -a "$out/built.txt"
done
