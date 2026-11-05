#!/bin/bash
# trial.sh MODE URLPATH: one zsync2 trial against the loopback server (#296 experiment)
set -u
MODE=$1 URLPATH=$2 D=/home/patrick/work/put.gd-r12aiu/f44-297 PORT=18318
cd $D; T=$D/trial-$MODE; rm -rf "$T"; mkdir -p "$T/apps" "$T/srv"
ln -s $D/srv/putgd-0.1.1-nightly.816-x86_64.AppImage "$T/srv/"
python3 - "$T/srv/appimage.zsync" "http://127.0.0.1:$PORT/$URLPATH" <<'PY'
import sys
data = open('/home/patrick/work/put.gd-r12aiu/f44-297/srv/gh.zsync', 'rb').read()
head, sep, rest = data.partition(b'\n\n')
lines = [b'URL: ' + sys.argv[2].encode() if l.startswith(b'URL: ') else l for l in head.split(b'\n')]
open(sys.argv[1], 'wb').write(b'\n'.join(lines) + sep + rest)
PY
cp apps/n.keep "$T/apps/putgd-0.1.1-nightly.801-x86_64.AppImage"
python3 patch_updinfo.py "$T/apps/putgd-0.1.1-nightly.801-x86_64.AppImage" "zsync|http://127.0.0.1:$PORT/appimage.zsync"
python3 rangeserver.py $PORT "$T/srv" $MODE 2> "$T/server.log" & SP=$!
sleep 1
SSL_CERT_FILE=/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem timeout 300 tools/appimageupdatetool-x86_64.AppImage --remove-old "$T/apps/putgd-0.1.1-nightly.801-x86_64.AppImage" > "$T/update.log" 2>&1
echo "mode=$MODE exit=$?"
kill $SP; wait $SP 2>/dev/null
grep -v "% done" "$T/update.log" | grep -E "zsync|Update|error|fail" | tail -6
sort "$T/server.log" | awk '{print $1, $NF, ($3 ~ /,/ ? "multi" : ($3=="range=None" ? "norange" : "single"))}' | sort | uniq -c
ls "$T/apps"; (cd "$T/apps" && sha256sum putgd-0.1.1-nightly.816-x86_64.AppImage 2>/dev/null | cut -c1-16)
