#!/usr/bin/env bash
# Inside a disposable Arch container, as an unprivileged build user.
# The workflow supplies ccache as the compiler launcher, for the SDK and the app:
# Arch rolls, so an exact SDK prefix cache would rarely hit.
set -euo pipefail
jobs=$(nproc)
if [ -n "${RELEASE_TAG:-}" ]; then python tools/releases/verify_tag.py --tag "$RELEASE_TAG"; fi
ccache --zero-stats
bash tools/ci/sdk-source.sh
PUTGD_SDK_JOBS="$jobs" bash tools/releases/provision-sdk.sh
python tools/proton/provision_runtime.py
cmake --preset release -DPUTGD_OCR=ON -DBUILD_TESTING=OFF -DPUTGD_RELEASE_TAG="${RELEASE_TAG:-}" \
  -DPUTGD_BUNDLE_PROTON=ON -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
  -DPUTGD_PACKAGE_TARGET=archlinux -DPUTGD_NAUTILUS_PLUGIN=ON \
  -DPUTGD_CAJA_PLUGIN=ON -DPUTGD_PEONY_PLUGIN=OFF
cmake --build --preset release --parallel "$jobs"
ccache --show-stats
python tools/releases/arch.py
python tools/releases/debug_symbols.py stage --build build/release --installed build/release-packages/extracted --label archlinux --output build/debug-symbols
