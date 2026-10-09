#!/bin/sh
set -eu
cd /home/patrick/.codex/worktrees/ui-process-split/put.gd
out=$PWD/work/u11/setup-recovery
mode=$1
trap 'code=$?; printf "%s\n" "$code" > "$out/$mode-exit"' EXIT
if [ "$mode" = asan ]; then
 cmake --build --preset asan --target test_gui_settings > "$out/asan-build.log" 2>&1
 ctest --preset asan -R '^gui_settings$' --output-junit "$out/asan.xml" > "$out/asan.log" 2>&1
else
 base=/home/patrick/work/put.gd/work/sanitizer-diagnosis
 setarch x86_64 -R cmake --build build/tsan-instrumented-qt --parallel 2 --target test_gui_settings > "$out/tsan-build.log" 2>&1
 setarch x86_64 -R env QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR=/usr/share/fonts/truetype/dejavu PATH="$base/deps-tsan/bin:$PATH" LD_LIBRARY_PATH="$base/deps-tsan/lib" TSAN_OPTIONS="halt_on_error=1:suppressions=$PWD/tests/sanitizer/tsan.supp" ctest --test-dir build/tsan-instrumented-qt --output-on-failure -R '^gui_settings$' --output-junit "$out/tsan.xml" > "$out/tsan.log" 2>&1
fi
