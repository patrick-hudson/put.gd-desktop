#!/usr/bin/env bash
set -euo pipefail
repo=${1:?Pass the put.gd checkout path}
output=${2:?Pass a fresh absolute evidence directory}
mkdir -p "$output"
cd "$repo"
cmake --build --preset dev --target test_gui_settings > "$output/build.log" 2>&1
QT_QPA_PLATFORM=offscreen build/dev/test_gui_settings \
 transientSetupInspectionRecoversWithoutReplayingAcceptedWork \
 expiredSetupResultDoesNotStarveLaterAcceptedWork \
 destinationTestButtonStaysDisabledUntilEveryAcceptedTestFinishes \
 -o "$output/focused.log",txt -o "$output/focused.xml",junitxml
ctest --preset dev -R '^gui_settings$' --output-junit "$output/settings-suite.xml" > "$output/settings-suite.log" 2>&1
