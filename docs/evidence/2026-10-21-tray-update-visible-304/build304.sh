#!/bin/bash
# build304.sh SDK TARGET WITH_TEST(0|1) [AWS_RUNTIME_DIR]: stable-tagged (local, unpushed v0.1.1) build of the #304 fix.
set -euo pipefail
SDK=$1 TARGET=$2 WITH_TEST=$3 RUNTIME=${4:-}
ROOT=/home/patrick/work/put.gd-304 SHA=3ae57f682433e847a9e46232e3fa68927cc48a88
extra=(); [ -n "$RUNTIME" ] && extra=(-DPUTGD_AWS_RUNTIME_DIR="$RUNTIME")
git clone -q --no-checkout -b claude/tray-update-visible-304 "$ROOT/put.gd-304.bundle" "$ROOT/src"
git -C "$ROOT/src" checkout -q --detach "$SHA"; git -C "$ROOT/src" tag v0.1.1 "$SHA"
mkdir -p "$ROOT/src/build"; ln -sfn "$SDK" "$ROOT/src/build/sdk"; ln -sfn "$(dirname "$SDK")/dependencies" "$ROOT/src/build/dependencies"
test -z "$(git -C "$ROOT/src" status --porcelain)"
targets=(putgd putgd-ui putgd-cli); [ "$WITH_TEST" = 1 ] && targets+=(test_native_tray)
(cd "$ROOT/src" && cmake --preset release -DBUILD_TESTING=$([ "$WITH_TEST" = 1 ] && echo ON || echo OFF) -DPUTGD_RELEASE_TAG=v0.1.1 -DPUTGD_PACKAGE_TARGET="$TARGET" "${extra[@]}" > "$ROOT/configure.log" 2>&1)
(cd "$ROOT/src" && nice -n 10 cmake --build build/release -j3 --target "${targets[@]}" > "$ROOT/build.log" 2>&1)
mkdir -p "$ROOT/bin-stable"; cp -a "$ROOT"/src/build/release/{putgd,putgd-ui,putgd-cli} "$ROOT/bin-stable/"
"$ROOT/bin-stable/putgd" --version-json | tr -d '\n '; echo; "$ROOT/bin-stable/putgd-cli" --update-info
if [ "$WITH_TEST" = 1 ]; then (cd "$ROOT/src/build/release" && ctest -R native_tray_integration --output-on-failure > "$ROOT/test.log" 2>&1; echo "ctest exit $?"; tail -3 "$ROOT/test.log"); fi
echo BUILD304-DONE
