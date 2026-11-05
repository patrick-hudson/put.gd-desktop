#!/bin/bash
# build303.sh SDK TARGET WANT_BASE(0|1) [AWS_RUNTIME_DIR]
# Builds put.gd binaries for #303 acceptance in ~/work/put.gd-303.
set -euo pipefail
SDK=$1 TARGET=$2 WANT_BASE=$3 RUNTIME=${4:-}
ROOT=/home/patrick/work/put.gd-303
HEAD_SHA=71f241b8f5d35b2022b0e27efae6f7276679fdc5 BASE_SHA=85582d6484181821bc654122b67d25befeda13a2
extra=(); [ -n "$RUNTIME" ] && extra=(-DPUTGD_AWS_RUNTIME_DIR="$RUNTIME")
prep() { # dir sha tags...
  local dir=$1 sha=$2; shift 2
  [ -d "$dir" ] || git clone -q --no-checkout -b bundle-303 "$ROOT/put.gd-303.bundle" "$dir"
  git -C "$dir" checkout -q --detach "$sha"
  for t in "$@"; do git -C "$dir" tag -f "$t" "$sha" >/dev/null; done
  mkdir -p "$dir/build"; ln -sfn "$SDK" "$dir/build/sdk"
  ln -sfn "$(dirname "$SDK")/dependencies" "$dir/build/dependencies"
  test -z "$(git -C "$dir" status --porcelain)"
}
build() { # dir tag target outdir
  local dir=$1 tag=$2 target=$3 out=$4
  (cd "$dir" && cmake --preset release -DBUILD_TESTING=OFF -DPUTGD_RELEASE_TAG="$tag" -DPUTGD_PACKAGE_TARGET="$target" "${extra[@]}" >"$out.configure.log" 2>&1)
  (cd "$dir" && nice -n 10 cmake --build build/release -j3 --target putgd putgd-ui putgd-cli >"$out.build.log" 2>&1)
  mkdir -p "$out"; cp -a "$dir"/build/release/putgd "$dir"/build/release/putgd-ui "$dir"/build/release/putgd-cli "$out"/
  "$out/putgd" --version-json | tr -d '\n '; echo; "$out/putgd-cli" --update-info 2>/dev/null || true
}
if [ "$WANT_BASE" = 1 ]; then
  prep "$ROOT/src-base" "$BASE_SHA" v0.1.1-nightly.781
  build "$ROOT/src-base" v0.1.1-nightly.781 "$TARGET" "$ROOT/bin-base"
  prep "$ROOT/src-head" "$HEAD_SHA" v0.1.1-nightly.795 v0.1.1
  build "$ROOT/src-head" v0.1.1-nightly.795 "$TARGET" "$ROOT/bin-nightly"
else
  prep "$ROOT/src-head" "$HEAD_SHA" v0.1.1-nightly.795 v0.1.1
fi
build "$ROOT/src-head" v0.1.1 "$TARGET" "$ROOT/bin-stable"
echo BUILD303-DONE
