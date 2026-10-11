#!/bin/sh
set -eu
cd /home/patrick/.codex/worktrees/ui-process-split/put.gd
out="$PWD/work/u11/final"
putgd_tsan_root=/home/patrick/work/put.gd/work/sanitizer-diagnosis
putgd_tsan_deps="$putgd_tsan_root/deps-tsan"
trap 'code=$?; printf "%s\n" "$code" > "$out/tsan-exit"' EXIT
setarch x86_64 -R cmake -S . -B build/tsan-instrumented-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPUTGD_SANITIZER=thread -DQt6_DIR="$putgd_tsan_root/qtbase-tsan-build/lib/cmake/Qt6" -DQt6Keychain_DIR="$putgd_tsan_deps/lib/cmake/Qt6Keychain" -DCMAKE_PREFIX_PATH="$putgd_tsan_root/qtbase-tsan-build/lib/cmake;/home/patrick/work/put.gd/build/sdk" -DPUTGD_AWS_RUNTIME_DIR=/home/patrick/work/put.gd/build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/patrick/work/put.gd/build/dependencies/aws-sdk-cpp > "$out/tsan-configure.log" 2>&1
setarch x86_64 -R cmake --build build/tsan-instrumented-qt --parallel 2 --target test_cli test_local_ipc test_ui_native_lifecycle test_gui_application test_ui_process test_ui_split_services test_gui_settings test_gui_history_view test_gui_operations_view test_gui_tray test_gui_version test_ui_control test_session_client test_ui_endpoints test_x11 test_kde_ownership > "$out/tsan-build.log" 2>&1
setarch x86_64 -R env QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR=/usr/share/fonts/truetype/dejavu PATH="$putgd_tsan_deps/bin:$PATH" LD_LIBRARY_PATH="$putgd_tsan_deps/lib" TSAN_OPTIONS="halt_on_error=1:suppressions=$PWD/tests/sanitizer/tsan.supp" ctest --test-dir build/tsan-instrumented-qt --output-on-failure -R '^(cli_integration|local_ipc_contract|gui_.*|ui_.*_integration|session_client_integration|x11_isolated|kde_ownership_integration)$' --output-junit "$out/tsan.xml" > "$out/tsan.log" 2>&1
