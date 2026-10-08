set -eu
build_root=/home/lab/putgd-current-a3d70dc
mkdir "$build_root"
cd "$build_root"
echo 'ec179214a662010c7f6d21a2cc5fe29479423ff6a6e9292433ef35b0aebd3fab  /home/lab/putgd-current-a3d70dc.tar' | sha256sum -c -
tar -xf /home/lab/putgd-current-a3d70dc.tar
mkdir -p evidence
python3 tools/verify_dependencies.py /home/lab/putgd-native/build/dependencies/aws-sdk-cpp > evidence/dependency-verification.json
cmake --preset dev -DCMAKE_PREFIX_PATH=/home/lab/putgd-native/build/sdk -DPUTGD_AWS_RUNTIME_DIR=/home/lab/putgd-native/build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/lab/putgd-native/build/dependencies/aws-sdk-cpp -DPUTGD_LAB_FIXTURE=ON > evidence/configure-dev.log 2>&1
cmake --build --preset dev > evidence/build-dev.log 2>&1
ctest --preset dev --output-junit "$build_root/evidence/ctest-dev.xml" > evidence/ctest-dev.log 2>&1
cmake --preset release -DCMAKE_PREFIX_PATH=/home/lab/putgd-native/build/sdk -DPUTGD_AWS_RUNTIME_DIR=/home/lab/putgd-native/build/sdk/lib -DPUTGD_AWS_SOURCE_DIR=/home/lab/putgd-native/build/dependencies/aws-sdk-cpp -DPUTGD_LAB_FIXTURE=ON > evidence/configure-release.log 2>&1
cmake --build --preset release > evidence/build-release.log 2>&1
cpack --config build/release/CPackConfig.cmake -G DEB -B build/packages > evidence/package.log 2>&1
sha256sum build/dev/putgd build/dev/putgd-cli build/dev/libputgd-s3.so build/release/putgd build/release/putgd-cli build/release/libputgd-s3.so build/packages/*.deb integrations/gnome/extension.js > evidence/sha256s.txt
printf '%s\n' complete > evidence/status
