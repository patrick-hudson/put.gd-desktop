#!/usr/bin/env bash
# Clone the pinned official AWS SDK source and verify every commit against the lock.
set -euo pipefail
python=${PYTHON:-python3}
release=$("$python" -c 'import json; print(json.load(open("cmake/dependency-lock.json"))["awsSdk"]["release"])')
git clone --depth 1 --branch "$release" --recurse-submodules --shallow-submodules \
  https://github.com/aws/aws-sdk-cpp.git build/dependencies/aws-sdk-cpp
"$python" tools/verify_dependencies.py build/dependencies/aws-sdk-cpp
