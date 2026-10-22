#!/bin/bash
# up2.sh NAME INPUT IMAGE : input agent (uinput), setup, stand-in URL recorder ON before the resident starts, start, synthetic capture, notifications on.
set -u
n=$1 input=$2 image=$3
root=$HOME/work/put.gd-i415; cd "$root" || exit 2; export AFA_ROOT=$root
A="python3 -I tool/afa.py"
if [ "$input" = uinput ] && [ ! -S /run/user/$(id -u)/putgd-lab-input.sock ]; then
  sudo -n python3 -I tool/host_input.py serve --socket /run/user/$(id -u)/putgd-lab-input.sock --user patrick > input-$n.log 2>&1 &
  echo $! > input-$n.pid; for _ in $(seq 20); do [ -S /run/user/$(id -u)/putgd-lab-input.sock ] && break; sleep .5; done
fi
$A setup $n --appimage "$root/images/$image" --vnc-port 5900 --input $input > setup-$n.json || { echo setup-failed; exit 4; }
python3 -I tool/i326.py handler-on $n | python3 -c 'import json,sys;d=json.load(sys.stdin);print("recorder", d["after"]["https"], d["after"]["http"])'
d=runs/$n; $A start $n > $d/start.json || { echo start-failed; exit 5; }
python3 -I tool/i326.py synth $n > /dev/null
python3 - $n <<'PY'
import sys; sys.path.insert(0, 'tool'); import afa
run = afa.Run(sys.argv[1]); print('notifications on', afa.set_notifications(run, True))
PY
python3 -c "import json;s=json.load(open('$d/start.json'));print('started', s['diagnostics'].get('build',{}).get('version') if isinstance(s['diagnostics'].get('build'),dict) else s['diagnostics'].get('build'), s['credentials'])"
