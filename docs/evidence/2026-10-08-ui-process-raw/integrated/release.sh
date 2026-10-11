#!/bin/sh
set -eu
cd /home/patrick/.codex/worktrees/ui-process-split/put.gd
out="$PWD/work/u11/integrated"
trap 'code=$?; printf "%s\n" "$code" > "$out/release-exit"' EXIT
cmake --preset release -DCMAKE_PREFIX_PATH=/home/patrick/work/put.gd/build/sdk -DPUTGD_AWS_RUNTIME_DIR=/home/patrick/work/put.gd/build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/patrick/work/put.gd/build/dependencies/aws-sdk-cpp -DPUTGD_LAB_FIXTURE=ON > "$out/release-configure.log" 2>&1
cmake --build --preset release --target putgd putgd-ui putgd-cli putgd-desktop-fixture > "$out/release-build.log" 2>&1
