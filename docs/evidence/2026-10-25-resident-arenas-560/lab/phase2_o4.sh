#!/bin/bash
# Revision of phase2_o3.sh (#579 Greptile 4372226982): builds the S3 module too and installs each revision
# into its own private install-relative layout, so the relocated worker resolves its own module.
# Layout per revision: OUTROOT/rev-TAG/<bindir>/{operations_upload_probe,putgd-s3-worker} and
# OUTROOT/rev-TAG/<libdir>/putgd/libputgd-s3.so, with bindir/libdir taken from the build's CMakeCache, the same
# values that define PUTGD_S3_INSTALL_MODULE_RELATIVE. Every stage fails fast; nothing is copied unless the
# checkout, patch, fresh outputs, worker revision and embedded relative module path all verify.
# usage: phase2_o4.sh SRC HARNESS OUTROOT REV...   (OUTROOT must not exist)
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
cache=build/dev/CMakeCache.txt
bindir=$(sed -n 's/^CMAKE_INSTALL_BINDIR:PATH=//p' "$cache"); libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:PATH=//p' "$cache")
[[ -n $bindir && -n $libdir ]] || { echo "install dirs unknown" >&2; exit 2; }
relative=$(python3 -c 'import os,sys;print(os.path.relpath(os.path.join(sys.argv[2],"putgd","libputgd-s3.so"),sys.argv[1]))' "$bindir" "$libdir")
for rev in "$@"; do
 tag=${rev:0:8}; dest=$outroot/rev-$tag
 git checkout -q "$rev"
 [[ $(git rev-parse HEAD) == "$rev" ]] || { echo "checkout mismatch $rev" >&2; exit 4; }
 git apply "$harness/probe-control.patch"
 git diff --quiet -- tests/performance/operations_upload_probe.cpp && { echo "patch not applied" >&2; exit 5; }
 stamp=$outroot/.stamp-$tag; touch "$stamp"
 nice -n 10 cmake --build --preset dev -j3 --target operations_upload_probe putgd-s3-worker putgd_s3_module >> "$log" 2>&1
 for f in build/dev/operations_upload_probe build/dev/putgd-s3-worker; do [[ $f -nt $stamp ]] || { echo "stale $f for $rev" >&2; exit 6; }; done
 [[ -f build/dev/libputgd-s3.so ]] || { echo "module missing for $rev" >&2; exit 8; }
 # Capture strings to owned files: under pipefail, grep -q exiting early can SIGPIPE strings.
 strings build/dev/putgd-s3-worker > "$outroot/.strings-$tag"
 grep -qF "g${rev:0:12}" "$outroot/.strings-$tag" || { echo "worker identity is not $rev" >&2; exit 7; }
 grep -qxF "$relative" "$outroot/.strings-$tag" || { echo "worker module lookup is not $relative" >&2; exit 9; }
 mkdir -m 700 "$dest"; mkdir -p "$dest/$bindir" "$dest/$libdir/putgd"
 cp build/dev/operations_upload_probe build/dev/putgd-s3-worker "$dest/$bindir/"
 cp build/dev/libputgd-s3.so "$dest/$libdir/putgd/"
 [[ -f $dest/$bindir/$relative ]] || { echo "module not at the configured relative path" >&2; exit 10; }
 git diff -- tests/performance/operations_upload_probe.cpp > "$dest/applied.patch"
 printf '{"revision":"%s","bindir":"%s","libdir":"%s","moduleRelative":"%s"}\n' "$rev" "$bindir" "$libdir" "$relative" > "$dest/layout.json"
 (cd "$dest" && sha256sum "$bindir/operations_upload_probe" "$bindir/putgd-s3-worker" "$libdir/putgd/libputgd-s3.so" applied.patch layout.json > SHA256SUMS)
 git checkout -q -- tests/performance/operations_upload_probe.cpp
 echo "$(date -u +%FT%TZ) built $rev" >> "$log"
done
echo "$(date -u +%FT%TZ) probe-builds-done" >> "$log"
