#!/bin/sh
set -u
cd /home/patrick/.codex/worktrees/ui-process-split/put.gd
out="$PWD/work/u11/integrated"
putgd_tsan_deps=/home/patrick/work/put.gd/work/sanitizer-diagnosis/deps-tsan
setarch x86_64 -R env QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR=/usr/share/fonts/truetype/dejavu PATH="$putgd_tsan_deps/bin:$PATH" LD_LIBRARY_PATH="$putgd_tsan_deps/lib" TSAN_OPTIONS="halt_on_error=1:suppressions=$PWD/tests/sanitizer/tsan.supp" timeout 180 build/tsan-instrumented-qt/test_gui_settings -o "$out/tsan-settings-bounded.xml",junitxml -o "$out/tsan-settings-bounded.log",txt
status=$?
printf '%s\n' "$status" > "$out/tsan-settings-bounded-exit"
exit "$status"
