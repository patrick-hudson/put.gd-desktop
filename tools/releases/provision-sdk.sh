#!/usr/bin/env bash
set -euo pipefail
# The caller first verifies this exact source with tools/verify_dependencies.py.
cmake -S build/dependencies/aws-sdk-cpp -B build/aws -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/build/sdk" \
  -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_ONLY=s3 -DBUILD_SHARED_LIBS=ON \
  -DENABLE_TESTING=OFF -DAUTORUN_UNIT_TESTS=OFF -DENABLE_UNITY_BUILD=ON -DUSE_OPENSSL=ON
cmake --build build/aws --parallel "${PUTGD_SDK_JOBS:-2}"
cmake --install build/aws
