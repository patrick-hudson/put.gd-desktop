#!/bin/bash
set -eu
lab=/home/patrick/work/i561
drv=$lab/driver
work=$lab/slow3
out=/tmp/p561s3
deb=$lab/putgd_26.1025.1260~dev+g8c4923d06b95_amd64.deb
umask 077
mkdir "$work"
exec >"$work/runner.log" 2>&1
cin=$(pgrep -x -u "$(id -u)" cinnamon | head -1)
while IFS= read -r -d '' kv; do
 case "$kv" in HOME=*|USER=*|LOGNAME=*|PATH=*|LANG=*|DISPLAY=*|XAUTHORITY=*|DBUS_SESSION_BUS_ADDRESS=*|XDG_RUNTIME_DIR=*|XDG_CURRENT_DESKTOP=*|XDG_SESSION_DESKTOP=*|XDG_SESSION_TYPE=*|XDG_SESSION_ID=*|DESKTOP_SESSION=*) export "$kv";; esac
done < /proc/$cin/environ
python3 "$drv/tools/diagnostics/check_socket_paths.py" --socket "$out/runtime/putgd/session.sock" > "$work/socket-preflight.json"
uptime > "$work/load.txt"
sudo -n apt-get install -y "$deb" gir1.2-gtk-4.0 >"$work/install.log" 2>&1
mkdir "$work/fixture"
cd "$work/fixture"
python3 "$drv/tests/fixtures/s3_server.py" >"$work/fixture.port" 2>"$work/fixture.err" &
fixture=$!
trap 'kill -TERM "$fixture" 2>/dev/null || true; wait "$fixture" || true' EXIT
for _ in $(seq 100); do [ -s "$work/fixture.port" ] && break; sleep .1; done
port=$(head -1 "$work/fixture.port")
printf 'fixturePid=%s session=%s\n' "$fixture" "$XDG_SESSION_ID"
cd "$work"
python3 "$drv/tests/performance/physical_campaign.py" slow --output "$out" --package "$deb" \
 --package-sha256 d403d030fa804f98af8072d78495a5b205e602245fce29ca2cf63f863ed1ae48 \
 --version 26.1025.1260-dev+g8c4923d06b95 --source-revision 8c4923d06b9591dde7d39049fccd4fb50a2a8725 \
 --driver-revision 00ab777f2413a1fbd7670a7259dc614eced896e6 --desktop-session "$XDG_SESSION_ID" \
 --lock-session codex-561-m74-8c --vnc-port 5900 \
 --native-driver "$lab/native495/native_acceptance.py" --native-driver-revision 72afd00bf2dbdaaa7e4bd3f847dc54f1da715f0e \
 --native-driver-sha256 347934c226f99cb0e509d5466c5df8116896e6bfac07d75fdfa4f4c2d06ca053 \
 --endpoint "http://127.0.0.1:$port" --fixture-pid "$fixture" --fixture-source "$drv/tests/fixtures/s3_server.py" \
 --enable-accessibility --settle-seconds 300 > "$work/driver.out" 2> "$work/driver.err"
