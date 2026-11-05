#!/bin/bash
# Revision of phase2_o.sh's probe stage (#579 reviews 4372113427, 4372122109). Every stage fails fast;
# binaries are copied only after a verified build of the intended revision, into new directories.
# usage: phase2_o2.sh SRC HARNESS OUTROOT REV...   (OUTROOT must not exist)
set -euo pipefail
src=$1 harness=$2 outroot=$3; shift 3
[[ -f $src/tools/diagnostics/check_socket_paths.py && -f $harness/probe-control.patch ]] || { echo "bad roots" >&2; exit 2; }
[[ ! -e $outroot ]] || { echo "OUTROOT exists: $outroot" >&2; exit 2; }
mkdir -m 700 "$outroot"; log=$outroot/build.log
cd "$src"
start_ref=$(git symbolic-ref -q --short HEAD || git rev-parse HEAD)
[[ -z $(git status --porcelain) ]] || { echo "source tree not clean" >&2; exit 3; }
restore(){ git checkout -q -- tests/performance/operations_upload_probe.cpp 2>/dev/null || true; git checkout -q "$start_ref" 2>/dev/null || true; }
trap restore EXIT
for rev in "$@"; do
 tag=${rev:0:8}; dest=$outroot/probe-$tag
 git checkout -q "$rev"
 [[ $(git rev-parse HEAD) == "$rev" ]] || { echo "checkout mismatch $rev" >&2; exit 4; }
 git apply "$harness/probe-control.patch"
 git diff --quiet -- tests/performance/operations_upload_probe.cpp && { echo "patch not applied" >&2; exit 5; }
 stamp=$outroot/.stamp-$tag; touch "$stamp"
 nice -n 10 cmake --build --preset dev -j3 --target operations_upload_probe putgd-s3-worker >> "$log" 2>&1
 for f in build/dev/operations_upload_probe build/dev/putgd-s3-worker; do [[ $f -nt $stamp ]] || { echo "stale $f for $rev" >&2; exit 6; }; done
 strings build/dev/putgd-s3-worker | grep -q "g${rev:0:12}" || { echo "worker identity is not $rev" >&2; exit 7; }
 mkdir -m 700 "$dest"
 cp build/dev/operations_upload_probe build/dev/putgd-s3-worker "$dest/"
 git diff -- tests/performance/operations_upload_probe.cpp > "$dest/applied.patch"
 (cd "$dest" && sha256sum operations_upload_probe putgd-s3-worker applied.patch > SHA256SUMS)
 git checkout -q -- tests/performance/operations_upload_probe.cpp
 echo "$(date -u +%FT%TZ) built $rev" >> "$log"
done
echo "$(date -u +%FT%TZ) probe-builds-done" >> "$log"
