set -eu
cd /home/lab/putgd-current-a3d70dc
python3 tools/verify_dependencies.py /home/lab/putgd-native/build/dependencies/aws-sdk-cpp
cmake --preset dev -DCMAKE_PREFIX_PATH=/home/lab/putgd-native/build/sdk -DPUTGD_AWS_RUNTIME_DIR=/home/lab/putgd-native/build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/lab/putgd-native/build/dependencies/aws-sdk-cpp -DPUTGD_LAB_FIXTURE=ON -DCMAKE_INSTALL_PREFIX=/usr
cmake --build --preset dev
ctest --preset dev --output-junit /home/lab/putgd-current-a3d70dc/dev-tests.xml
cmake --preset release -DCMAKE_PREFIX_PATH=/home/lab/putgd-native/build/sdk -DPUTGD_AWS_RUNTIME_DIR=/home/lab/putgd-native/build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/lab/putgd-native/build/dependencies/aws-sdk-cpp -DPUTGD_LAB_FIXTURE=ON -DCMAKE_INSTALL_PREFIX=/usr
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake -G DEB -B build/packages
sha256sum build/release/putgd build/release/putgd-cli build/release/libputgd-s3.so build/packages/*.deb
