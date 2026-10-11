set -eu
cd /home/lab/putgd-current-a3d70dc
cmake --build --preset release --target putgd putgd-cli putgd_s3_module putgd-desktop-fixture
cpack --config build/release/CPackConfig.cmake -G DEB -B build/packages
sha256sum build/release/putgd build/release/putgd-cli build/release/libputgd-s3.so build/packages/*.deb
