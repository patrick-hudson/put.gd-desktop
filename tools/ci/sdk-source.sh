#!/usr/bin/env bash
# Clone the pinned official AWS SDK source and verify every commit against the lock.
# Only the S3 client is built and no client is regenerated, so the other ~440
# generated service clients (165,000 files) and the code generator's API models
# (333 MB) stay out of the working tree. Licence and notice files, the SDK core
# and every CRT submodule are checked out as usual.
set -euo pipefail
python=${PYTHON:-python3}
release=$("$python" -c 'import json; print(json.load(open("cmake/dependency-lock.json"))["awsSdk"]["release"])')
source=build/dependencies/aws-sdk-cpp
git clone --depth 1 --branch "$release" --filter=blob:none --no-checkout \
  https://github.com/aws/aws-sdk-cpp.git "$source"
git -C "$source" sparse-checkout set --no-cone \
  '/*' '!/generated/*' '/generated/src/' '!/generated/src/*' '/generated/src/aws-cpp-sdk-s3/' '!/tools/code-generation/'
git -C "$source" -c advice.detachedHead=false checkout "$release"
git -C "$source" submodule update --init --recursive --depth 1 --jobs "$(nproc)"
"$python" tools/verify_dependencies.py "$source"
