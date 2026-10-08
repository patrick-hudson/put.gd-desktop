set -eu
cd /home/lab/putgd-current-a3d70dc
cmake --build --preset release --target putgd putgd-cli putgd_s3_module putgd-desktop-fixture > evidence/build-release-targets.log 2>&1
cpack --config build/release/CPackConfig.cmake -G DEB -B build/packages > evidence/package.log 2>&1
sha256sum build/dev/putgd build/dev/putgd-cli build/dev/libputgd-s3.so build/release/putgd build/release/putgd-cli build/release/libputgd-s3.so build/packages/*.deb integrations/gnome/extension.js > evidence/sha256s.txt
printf '%s\n' complete > evidence/status
