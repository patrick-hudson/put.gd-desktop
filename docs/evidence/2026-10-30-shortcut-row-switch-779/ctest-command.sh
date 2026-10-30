#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:$PATH
cd /home/user/work/put.gd/src
mkdir -p /home/user/779/ci-ram
sudo chown putgd-ci:putgd-ci /home/user/779/ci-ram
sudo chown -R putgd-ci:putgd-ci build/dev
mountroot=$(cat /home/user/779/tmpfs-path.txt)
run=$(sudo runuser -u putgd-ci -- mktemp -d "$mountroot/r.XXXXXX")
sudo runuser -u putgd-ci -- mkdir -m 700 "$run/runtime" "$run/config" "$run/data" "$run/state" "$run/cache" "$run/tmp"
sudo python3 tools/ci/ctest_preflight.py --build build/dev --tmp "$run/tmp" --user putgd-ci
sudo chown -R putgd-ci:putgd-ci build/dev
sudo runuser -u putgd-ci -- env -u DISPLAY -u XAUTHORITY -u WAYLAND_DISPLAY QT_QPA_PLATFORM=offscreen PUTGD_TEST_TIME_SCALE=3 LANG=C.UTF-8 LC_ALL=C.UTF-8 TMPDIR="$run/tmp" XDG_RUNTIME_DIR="$run/runtime" XDG_CONFIG_HOME="$run/config" XDG_DATA_HOME="$run/data" XDG_STATE_HOME="$run/state" XDG_CACHE_HOME="$run/cache" sh -ec 'id -u > build/dev/ctest-observed-uid; exec python3 tools/ci/ctest_retry.py --ledger /home/user/779/ci-ram/flake-ledger.json --junit /home/user/779/ci-ram/ctest.xml -- --preset dev --parallel 12 --resource-spec-file "$PWD/tools/ci/ctest-resources.json"' > /home/user/779/ctest-ram-console.log 2>&1
sudo chown -R user:user build/dev /home/user/779/ci-ram
cp /home/user/779/ctest-ram-console.log /home/user/779/ci-ram/ctest.log
cp build/dev/Testing/Temporary/LastTest.log /home/user/779/ci-ram/LastTest.log
cp build/dev/ci-test-context.json build/dev/socket-preflight.json build/dev/registered-tests.json /home/user/779/ci-ram/
sudo chown -R user:user build/dev /home/user/779/ci-ram
tail -10 /home/user/779/ci-ram/ctest.log
