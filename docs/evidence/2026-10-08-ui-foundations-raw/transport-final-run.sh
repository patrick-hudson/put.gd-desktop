#!/bin/sh
set -u
cd /home/patrick/.codex/worktrees/ui-process-split/put.gd || exit 1
out=$PWD/work/u11/transport-final
(set -e
cmake --preset dev > "$out/dev-configure.log" 2>&1
cmake --build --preset dev --target test_session_client test_ui_control > "$out/dev-build.log" 2>&1
ctest --preset dev -R '^(session_client|ui_control)_integration$' --output-junit "$out/dev.xml" > "$out/dev.log" 2>&1
cmake --preset asan -DCMAKE_PREFIX_PATH=/home/patrick/work/put.gd/build/sdk -DPUTGD_AWS_RUNTIME_DIR=/home/patrick/work/put.gd/build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/patrick/work/put.gd/build/dependencies/aws-sdk-cpp > "$out/asan-configure.log" 2>&1
cmake --build --preset asan --target test_session_client test_ui_control > "$out/asan-build.log" 2>&1
ctest --preset asan -R '^(session_client|ui_control)_integration$' --output-junit "$out/asan.xml" > "$out/asan.log" 2>&1
) > "$out/launcher.log" 2>&1
status=$?
printf '%s\n' "$status" > "$out/exit-code"
exit "$status"
