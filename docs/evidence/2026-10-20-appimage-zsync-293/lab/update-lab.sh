#!/usr/bin/env bash
# Move build A to build B with appimageupdatetool through a local HTTP server.
# Runs on the f44 host (Fedora 44) or, with LAB=/src, in the Ubuntu 24.04 container.
set -uo pipefail
L=${LAB:-/home/patrick/work/put.gd-zsync}
run=$L/update-lab/run-${HOST_LABEL:-f44}
rm -rf "$run" && mkdir -p "$run/serve" "$run/client"
A=$(basename "$L"/out/A/*.AppImage); B=$(basename "$L"/out/B/*.AppImage)
cp "$L/out/B/$B" "$L/out/B/$B.zsync" "$run/serve/"
cp "$run/serve/$B.zsync" "$run/serve/appimage.zsync"   # stands in for /dl/<channel>/appimage.zsync
cp "$L/out/A/$A" "$run/client/"
python3 "$L/range_server.py" "$run/serve" 8765 > "$run/http.log" 2>&1 &   # serves Range requests like GitHub
server=$!
sleep 1
tool="$L/update-lab/tool/squashfs-root/AppRun"    # appimageupdatetool 2.0.0-alpha-1-20251018, extracted
cd "$run/client"
show() { printf '\n$ %s\n' "$*"; "$@"; echo "[exit $?]"; }
printf '# %s, %s\n' "$(. /etc/os-release; echo "$PRETTY_NAME")" "$(date -u +%FT%TZ)"
[ -n "${SSL_CERT_FILE:-}" ] && echo "# SSL_CERT_FILE=$SSL_CERT_FILE (appimageupdatetool does not find Fedora's CA bundle on its own)"
show ./"$A" --appimage-updateinformation
show sha256sum "$A" "$L/out/B/$B"
printf '\n$ head -8 serve/appimage.zsync\n'; head -8 "$run/serve/appimage.zsync"
show "$tool" --check-for-update "$PWD/$A"
show "$tool" "$PWD/$A"
show ls -l
show sha256sum ./*.AppImage
show ./"$B" --appimage-updateinformation
show "$tool" --check-for-update "$PWD/$B"
kill "$server"
printf '\n$ cat http.log\n'; cat "$run/http.log"
printf '\n# image bytes fetched by range: %s of %s\n' "$(awk '/range\(s\)/ {n += $(NF-2)} END {print n+0}' "$run/http.log")" "$(stat -c %s "$L/out/B/$B")"
