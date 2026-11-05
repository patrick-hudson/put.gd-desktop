#!/bin/bash
# ak2b.sh NAME : item I. Launch at login on, a real logout and display-manager autologin (relogin.sh), then what started.
set -u
n=$1; root=$HOME/work/put.gd-qt418/afa; cd "$root" || exit 2; export AFA_ROOT=$root
d=runs/$n; t() { date -u +%FT%TZ; }
python3 -I tool/afa.py login $n enable > $d/login-enable.json
echo "$(t) I: launch at login enabled" >> timeline-$n.txt
echo "$(t) I: real logout/login:" >> timeline-$n.txt
bash relogin.sh $n 2>&1 | sed 's/^/  /' >> timeline-$n.txt
sleep 45
python3 -I tool/afa.py login $n check > $d/login-check.json
echo "$(t) I: login check done" >> timeline-$n.txt
python3 -c "import json;d=json.load(open('$d/login-check.json'));print('$n fromAppImage',d['fromAppImage'],[(p['comm'],p['pid'],p['APPIMAGE']) for p in d['processes']])"
