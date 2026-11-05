#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:$PATH LANG=C.UTF-8 LC_ALL=C.UTF-8
cd /home/user/work/put.gd/src
test "$(git rev-parse HEAD)" = 6ad9f957e6544f30c5e45c08af2306db77a9ff64
test -z "$(git status --porcelain --untracked-files=no)"
uptime
df -h .
df -i .
python3 -c 'import os,json; s=os.statvfs('"'"'/home/user/work/put.gd/src'"'"'); print(json.dumps(dict(availableBytes=s.f_bavail*s.f_frsize,availableInodes=s.f_favail))); assert s.f_bavail*s.f_frsize >= 14*1024**3 and s.f_favail > 1024'
dpkg-query -W -f='${binary:Package}=${Version}\n' build-essential ccache git ca-certificates passwd util-linux qemu-utils python3 nodejs npm cmake ninja-build pkg-config qt6-base-dev qt6-base-dev-tools libqt6sql6-sqlite qt6-wayland-dev qtkeychain-qt6-dev libxcb1-dev libxcb-keysyms1-dev libxcb-util-dev libxcb-image0-dev libxcb-randr0-dev libxcb-xfixes0-dev libxcb-damage0-dev libxcb-shape0-dev libxcb-xinput-dev libxcb-xtest0-dev xvfb xauth dbus-daemon libxcursor-dev libxkbcommon-dev libxkbcommon-x11-dev libcurl4-openssl-dev libssl-dev zlib1g-dev libwayland-dev wayland-protocols libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libavcodec-dev libavformat-dev libavfilter-dev libavutil-dev libswscale-dev libswresample-dev gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav
cmake --preset dev -DPUTGD_BUNDLE_PROTON=ON -DPUTGD_TEST_TIMEOUT_SCALE=3 -DPUTGD_AWS_SOURCE_DIR="$PWD/build/sdk/share/putgd-ci/aws-sdk-source-licenses" -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
nice -n 10 cmake --build --preset dev --parallel 4
build/dev/putgd-cli --version-json
sudo -n setfacl -m u:putgd-ci:--x /home/user/work/put.gd /home/user/work /home/user
test_tmp=$(mktemp -d /tmp/pgs.XXXXXX)
sudo -n python3 tools/ci/ctest_preflight.py --build build/dev --tmp "$test_tmp" --user putgd-ci
sudo -n chown -R putgd-ci:putgd-ci build/dev "$test_tmp"
sudo -n install -d -m 700 -o putgd-ci -g putgd-ci "$test_tmp"/{config,data,state,cache,runtime}
trap 'sudo -n chown -R user:user build/dev' EXIT
sudo -n runuser -u putgd-ci -- env -u DISPLAY -u XAUTHORITY -u WAYLAND_DISPLAY PUTGD_TEST_TIME_SCALE=3 LANG=C.UTF-8 LC_ALL=C.UTF-8 TMPDIR="$test_tmp" XDG_CONFIG_HOME="$test_tmp/config" XDG_DATA_HOME="$test_tmp/data" XDG_STATE_HOME="$test_tmp/state" XDG_CACHE_HOME="$test_tmp/cache" XDG_RUNTIME_DIR="$test_tmp/runtime" python3 tools/ci/ctest_retry.py --ledger "$PWD/build/dev/snapshot-flakes.json" --junit "$PWD/build/dev/snapshot-tests.xml" -- --preset dev --parallel 8 --resource-spec-file "$PWD/tools/ci/ctest-resources.json"
