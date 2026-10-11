#!/bin/sh
set -eu
cd /home/patrick/.codex/worktrees/ui-process-split/put.gd
out="$PWD/work/u11/final"
trap 'code=$?; printf "%s\n" "$code" > "$out/asan-exit"' EXIT
cmake --preset asan -DCMAKE_PREFIX_PATH=/home/patrick/work/put.gd/build/sdk > "$out/asan-configure.log" 2>&1
cmake --build --preset asan --target test_cli test_local_ipc test_ui_native_lifecycle test_gui_application test_ui_process test_ui_split_services test_gui_settings test_gui_history_view test_gui_operations_view test_gui_tray test_gui_version test_ui_control test_session_client test_ui_endpoints test_x11 test_kde_ownership > "$out/asan-build.log" 2>&1
ctest --preset asan -R '^(cli_integration|local_ipc_contract|gui_.*|ui_.*_integration|session_client_integration|x11_isolated|kde_ownership_integration)$' --output-junit "$out/asan.xml" > "$out/asan.log" 2>&1
