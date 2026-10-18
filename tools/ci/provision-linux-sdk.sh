#!/usr/bin/env bash
# Hosted Linux test jobs: build the pinned SDK prefix and keep the license files
# that Linux packaging configuration reads, so a cache hit needs no SDK checkout.
# Release package builders still clone and verify the full source every run.
set -euo pipefail
bash tools/ci/sdk-source.sh
bash tools/releases/provision-sdk.sh
source=build/dependencies/aws-sdk-cpp
licenses=build/sdk/share/putgd-ci/aws-sdk-source-licenses
(cd "$source" && find . -name .git -prune -o -type f -name LICENSE -print) | while IFS= read -r path; do
  install -D -m 0644 "$source/$path" "$licenses/$path"
done
test -f "$licenses/LICENSE"
