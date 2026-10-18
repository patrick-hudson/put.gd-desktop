#!/usr/bin/env bash
# Windows: build pinned QtKeychain and the official AWS SDK into build/sdk.
set -euo pipefail
jobs=${PUTGD_SDK_JOBS:-2}
bash tools/ci/sdk-source.sh
git clone https://github.com/frankosterfeld/qtkeychain.git build/dependencies/qtkeychain
git -C build/dependencies/qtkeychain checkout 8eac9889775ca45c106d077bb0d3744754a1570a
cmake -S build/dependencies/qtkeychain -B build/keychain -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/sdk" -DBUILD_WITH_QT6=ON -DBUILD_TEST_APPLICATION=OFF -DBUILD_TRANSLATIONS=OFF
cmake --build build/keychain --parallel "$jobs"
cmake --install build/keychain
cmake -S build/dependencies/aws-sdk-cpp -B build/aws -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/sdk" -DBUILD_ONLY=s3 -DBUILD_SHARED_LIBS=ON -DENABLE_TESTING=OFF \
  -DAUTORUN_UNIT_TESTS=OFF -DENABLE_UNITY_BUILD=ON
cmake --build build/aws --parallel "$jobs"
cmake --install build/aws
