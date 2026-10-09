set -eu
cd /home/lab/putgd-u11-97ca3a521714/source
cmake --build --preset release --target putgd putgd-cli putgd-ui putgd_s3_module putgd-desktop-fixture
cpack --config build/release/CPackConfig.cmake -G DEB -B /home/lab/putgd-u11-97ca3a521714/packages-recovery
