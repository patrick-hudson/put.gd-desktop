#!/bin/bash
# fqt.sh HOST INPUT VNCPORT MODE FFROOT : #418 real-browser item F, phase A (up to the browser frame).
#   MODE ffprofile: the desktop's own Firefox, with a temporary profile made its default (profiles.ini/installs.ini, FFROOT = its root)
#   MODE ffdefault: Firefox --profile <temporary> made the http(s) handler (mimeapps.list), FFROOT = where the profile goes
# The live upload goes public-read to put.gd/acceptance/qt418/HOST/ with key set 2 from ~/Documents/creds.txt (stdin only).
set -u
h=$1 input=$2 vnc=$3 mode=$4 ffroot=$5; n=$h-F
root=$HOME/work/put.gd-qt418/afa; cd "$root" || exit 2; export AFA_ROOT=$root
img=$root/images/putgd-26.1023.1068-dev+g6a325a722afd-x86_64.AppImage
t() { date -u +%FT%TZ; }
A="python3 -I tool/afa.py"; R="python3 -I tool/rerun.py"
pgrep -u "$USER" -x 'firefox|firefox-bin|GeckoMain' > /dev/null && { echo "a Firefox is running; refusing" >&2; exit 3; }
if [ "$input" = uinput ] && [ ! -S /run/user/$(id -u)/putgd-lab-input.sock ]; then
  sudo -n python3 -I tool/host_input.py serve --socket /run/user/$(id -u)/putgd-lab-input.sock --user patrick > input-$n.log 2>&1 &
  echo $! > input-$n.pid; for _ in $(seq 20); do [ -S /run/user/$(id -u)/putgd-lab-input.sock ] && break; sleep .5; done
fi
[ -d "$ffroot" ] && find "$ffroot" -maxdepth 3 > ff-before-$n.txt
mkdir -p "$ffroot"; state=$root/ff-$n.json
python3 -I tool/$mode.py on "$ffroot" "$state" > ff-on-$n.json || exit 3
echo "$(t) F: $mode on ($(cat ff-on-$n.json | head -c 300))" >> timeline-$h.txt
export AFA_F_GUARD=$state
$A setup $n --appimage "$img" --vnc-port $vnc --input $input > /dev/null || exit 4
d=runs/$n; $A allow-full $n on > /dev/null; $A start $n > $d/start.json || exit 5
python3 -I tool/i326.py synth $n > /dev/null
python3 -I tool/s3creds.py 2 | $R live-s3 $n --prefix acceptance/qt418/$h/ --acl public-read | python3 -c "import json,sys; d=json.load(sys.stdin); print('profile', d['profile']['aclMode'], d['stored'], d.get('credentials'))"
echo "$(t) F: run $n, live S3 public-read to put.gd/acceptance/qt418/$h/, banner click ($input)" >> timeline-$h.txt
$A notify $n --label $n-notify > $d/notify.json 2> $d/notify.err
$R browser-shot $n --label $n-browser --wait 15 --since 60 > $d/browser.json
python3 - $d <<'PY'
import json, sys
d = sys.argv[1]; n = json.load(open(f'{d}/notify.json')); b = json.load(open(f'{d}/browser.json')); nn = n['notification']
print('upload', n['operation']['state'], [i['link'] for i in n['operation']['items']], 'invoked', nn.get('actionInvoked'), 'clicked', nn.get('clicked'))
print('browser', [(p['pid'], p['comm'], p['cmd'][:120]) for p in b['browserProcesses']][:4], b['opened'], b['full'])
PY
