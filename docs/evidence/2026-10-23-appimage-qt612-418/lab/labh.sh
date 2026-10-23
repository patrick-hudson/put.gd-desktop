#!/bin/bash
# labh.sh HOST INPUT VNCPORT : #418 H's update parts and J on the PR head's lab nightly (the CI build is untagged and
# doesn't check): A identity, start, #348 first open, #349 clean status, #344 channel options and stable/nightly,
# Settings' theme and Check now against the live app.put.gd feed, appimageupdatetool --check-for-update; then quit and clean.
set -u
h=$1 input=$2 vnc=$3; n=$h-labh
root=$HOME/work/put.gd-qt418/afa; cd "$root" || exit 2; export AFA_ROOT=$root
img=$root/images/putgd-26.1023.1068-nightly-x86_64.AppImage
A="python3 -I tool/afa.py"; R="python3 -I tool/rerun.py"; t() { date -u +%FT%TZ; }
d=runs/$n
echo "$(t) H/J on the lab nightly (run $n) start" >> timeline-$h.txt
$A identity "$img" images/SHA256SUMS.lab v26.1023.1068-nightly --save $root/labh-identity-$h.json > /dev/null
if [ "$input" = uinput ] && [ ! -S /run/user/$(id -u)/putgd-lab-input.sock ]; then
  sudo -n python3 -I tool/host_input.py serve --socket /run/user/$(id -u)/putgd-lab-input.sock --user patrick > input-$n.log 2>&1 &
  echo $! > input-$n.pid; for _ in $(seq 20); do [ -S /run/user/$(id -u)/putgd-lab-input.sock ] && break; sleep .5; done
fi
$A setup $n --appimage "$img" --vnc-port $vnc --input $input > /dev/null || exit 4
mv $root/labh-identity-$h.json $d/identity.json
$A allow-full $n on > /dev/null; $A start $n > $d/start.json || exit 5
$R updates-first-open $n --label $n-348-first-open > $d/updates-first-open.json
$R status-clean $n --label start > $d/status-clean-start.json
$R channel-options $n --label $n-channel > $d/channel-options.json
$A settings $n > $d/settings.json
$R status-clean $n --label settings > $d/status-clean-settings.json
$R channel $n --to stable --label $n-344 > $d/channel-stable.json
$R channel $n --to nightly --label $n-344 > $d/channel-nightly.json
$R update-check $n --tool images/appimageupdatetool-x86_64.AppImage --label lab $( [ $h = f44 ] && echo --ssl-cert-file /etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem ) > $d/J.json
python3 -I -c "import sys; sys.path.insert(0,'tool'); import afa; print(afa.quit_resident(afa.Run('$n')))" > $d/quit.txt 2>&1
$A cleanup $n > $d/cleanup.json
echo "$(t) H/J on the lab nightly done; run quit and cleaned" >> timeline-$h.txt
python3 -I tool/verdict.py $d | python3 -c "
import json,sys; d=json.load(sys.stdin)
for k,v in d.items():
  if k.startswith('H'): print(k, json.dumps(v,default=str)[:300])"
python3 -c "import json;d=json.load(open('$d/J.json'));print('J exit',d['exit'],d['stdout'][-300:])"
