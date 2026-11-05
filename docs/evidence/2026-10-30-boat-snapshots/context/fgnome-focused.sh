#!/bin/bash
set -euo pipefail
cd /home/user/work/put.gd/src
test_tmp=$(mktemp -d "${PUTGD_CI_TMPDIR:-/dev/shm}/p.XXXXXX")
sudo -n python3 tools/ci/ctest_preflight.py --build build/dev --tmp "$test_tmp" --user putgd-ci
sudo -n chown -R putgd-ci:putgd-ci build/dev "$test_tmp"
sudo -n install -d -m 700 -o putgd-ci -g putgd-ci "$test_tmp"/{config,data,state,cache,runtime}
trap 'sudo -n chown -R user:user build/dev' EXIT
sudo -n runuser -u putgd-ci -- env -u DISPLAY -u XAUTHORITY -u WAYLAND_DISPLAY PUTGD_TEST_TIME_SCALE=3 LANG=C.UTF-8 LC_ALL=C.UTF-8 TMPDIR="$test_tmp" XDG_CONFIG_HOME="$test_tmp/config" XDG_DATA_HOME="$test_tmp/data" XDG_STATE_HOME="$test_tmp/state" XDG_CACHE_HOME="$test_tmp/cache" XDG_RUNTIME_DIR="$test_tmp/runtime" ctest --preset dev --parallel 12 --resource-spec-file "$PWD/tools/ci/ctest-resources.json" -R '^(editor_job_integration|x11_isolated)$' --output-junit "$PWD/build/dev/focused-tests.xml"
