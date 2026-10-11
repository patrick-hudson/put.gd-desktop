#!/bin/sh
# Render source identity and paths into an owned disposable guest directory.
set -eu
cd "/home/lab/putgd-u11-97ca3a521714/source"
for preset in dev release; do
 cmake --preset "$preset" -DCMAKE_PREFIX_PATH=/home/lab/putgd-memory-build/sdk -DPUTGD_AWS_RUNTIME_DIR=/home/lab/putgd-memory-build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/lab/putgd-memory-build/dependencies/aws-sdk-cpp -DPUTGD_LAB_FIXTURE=ON -DCMAKE_INSTALL_PREFIX=/usr -DPUTGD_SOURCE_COMMIT=97ca3a521714e2717cedc1139976130627bfb8a8 -DPUTGD_SOURCE_TREE=888460466ff1d240bd8e3c61189f36f407fb3485 -DPUTGD_SOURCE_COMMIT_COUNT=41
 cmake --build --preset "$preset"
 if [ "$preset" = dev ]; then ctest --preset dev --output-junit "/home/lab/putgd-u11-97ca3a521714/dev.xml"; fi
done
cpack --config build/release/CPackConfig.cmake -G DEB -B "/home/lab/putgd-u11-97ca3a521714/packages"
