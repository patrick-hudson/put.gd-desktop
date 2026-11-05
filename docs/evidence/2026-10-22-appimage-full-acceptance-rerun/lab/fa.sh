#!/bin/bash
# fa.sh NAME INPUT VNCPORT FFROOT : real-browser item F on a physical desktop (phase A: up to the browser frame).
set -u
# 5th argument: the S3 prefix folder (desktop name)
n=$1 input=$2 vnc=$3 ffroot=$4
root=$HOME/work/put.gd-afa2; cd "$root" || exit 2; export AFA_ROOT=$root
A="python3 -I tool/afa.py"; R="python3 -I tool/rerun.py"
img=$root/images/putgd-0.1.1-nightly.942-x86_64.AppImage
if [ "$input" = uinput ] && [ ! -S /run/user/$(id -u)/putgd-lab-input.sock ]; then
  sudo -n python3 -I tool/host_input.py serve --socket /run/user/$(id -u)/putgd-lab-input.sock --user patrick > input-$n.log 2>&1 &
  echo $! > input-$n.pid; for _ in $(seq 20); do [ -S /run/user/$(id -u)/putgd-lab-input.sock ] && break; sleep .5; done
fi
python3 -I tool/ffprofile.py on "$ffroot" $root/ff-$n.json || exit 3
$A setup $n --appimage "$img" --vnc-port $vnc --input $input > /dev/null || exit 4
d=runs/$n; $A allow-full $n on > /dev/null; $A start $n > $d/start.json || exit 5
python3 -I tool/i326.py synth $n > /dev/null
python3 -I tool/s3creds.py 2 | $R live-s3 $n --prefix acceptance/afa2/$5/ --acl public-read | python3 -c "import json,sys; d=json.load(sys.stdin); print('profile', d['profile']['aclMode'], d['stored'], d.get('credentials'))"
$A notify $n --label $n-F-notify > $d/notify.json
$R browser-shot $n --label $n-F-browser --wait 15 --since 60 > $d/browser.json
python3 - $d <<'PY'
import json, sys
d = sys.argv[1]; n = json.load(open(f'{d}/notify.json')); b = json.load(open(f'{d}/browser.json')); nn = n['notification']
print('upload', n['operation']['state'], [i['link'] for i in n['operation']['items']], 'invoked', nn.get('actionInvoked'), 'clicked', nn.get('clicked'))
print('browser', [(p['pid'], p['comm'], p['cmd'][:120]) for p in b['browserProcesses']][:4], b['opened'])
PY
