#!/bin/sh
set -eu
cd /home/patrick/.codex/worktrees/ui-process-split/put.gd
out="$PWD/work/u11/review-corrected"
trap 'code=$?; printf "%s\n" "$code" > "$out/dev-exit"' EXIT
cmake --preset dev > "$out/dev-configure.log" 2>&1
cmake --build --preset dev > "$out/dev-build.log" 2>&1
ctest --preset dev --output-junit "$out/dev.xml" > "$out/dev.log" 2>&1
