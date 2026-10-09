#!/bin/sh
set -eu
cd /home/patrick/work/put.gd/work/u11/native/host/source
out=/home/patrick/work/put.gd/work/u11/native/host
sdk=/home/patrick/work/put.gd/build
trap 'code=$?; printf "%s\n" "$code" > "$out/dev-exit"' EXIT
cmake --preset dev -DCMAKE_PREFIX_PATH="$sdk/sdk" -DPUTGD_AWS_RUNTIME_DIR="$sdk/sdk/lib" -DPUTGD_AWS_SOURCE_DIR="$sdk/dependencies/aws-sdk-cpp" -DPUTGD_LAB_FIXTURE=ON -DCMAKE_INSTALL_PREFIX=/usr -DPUTGD_SOURCE_COMMIT=97ca3a521714e2717cedc1139976130627bfb8a8 -DPUTGD_SOURCE_TREE=888460466ff1d240bd8e3c61189f36f407fb3485 -DPUTGD_SOURCE_COMMIT_COUNT=41 > "$out/dev-configure.log" 2>&1
cmake --build --preset dev > "$out/dev-build.log" 2>&1
ctest --preset dev --output-junit "$out/dev.xml" > "$out/dev.log" 2>&1
