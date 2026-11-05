#!/bin/bash
# Frozen #555 memory scenario (cold 300 s / History 30 s / closed 300 s) on the already-installed exact CI package.
set -eu
label=$1 deb=$2 sha=$3 version=$4 rev=$5
lab=/home/patrick/work/i560o
drv=$lab/driver
work=$lab/$label
out=/tmp/p560$label
umask 077
mkdir "$work"
exec >"$work/runner.log" 2>&1
cin=$(pgrep -x -u "$(id -u)" cinnamon | head -1)
while IFS= read -r -d '' kv; do
 case "$kv" in HOME=*|USER=*|LOGNAME=*|PATH=*|LANG=*|DISPLAY=*|XAUTHORITY=*|DBUS_SESSION_BUS_ADDRESS=*|XDG_RUNTIME_DIR=*|XDG_CURRENT_DESKTOP=*|XDG_SESSION_DESKTOP=*|XDG_SESSION_TYPE=*|XDG_SESSION_ID=*|DESKTOP_SESSION=*) export "$kv";; esac
done < /proc/$cin/environ
env | grep -E '^(MALLOC|GLIBC_TUNABLES|LD_PRELOAD)' > "$work/allocator-env.txt" || true
cat /sys/kernel/mm/transparent_hugepage/enabled > "$work/thp.txt"
python3 "$drv/tools/diagnostics/check_socket_paths.py" --socket "$out/runtime/putgd/session.sock" > "$work/socket-preflight.json"
uptime > "$work/load.txt"
echo "$sha  $deb" | sha256sum -c > "$work/package-check.txt"
/usr/bin/putgd --version-json > "$work/installed-version.json"
cd "$work"
set +e
python3 "$drv/tests/performance/physical_campaign.py" memory --output "$out" --package "$deb" \
 --package-sha256 "$sha" \
 --version "$version" --source-revision "$rev" \
 --driver-revision 00ab777f2413a1fbd7670a7259dc614eced896e6 --desktop-session "$XDG_SESSION_ID" \
 --lock-session t3-i560-opus-ci1 --vnc-port 5900 \
 --native-driver "$lab/native495/native_acceptance.py" --native-driver-revision 72afd00bf2dbdaaa7e4bd3f847dc54f1da715f0e \
 --native-driver-sha256 347934c226f99cb0e509d5466c5df8116896e6bfac07d75fdfa4f4c2d06ca053 \
 --open-seconds 30 > "$work/driver.out" 2> "$work/driver.err"
echo "driverExit=$?" > "$work/driver-exit.txt"
