#!/bin/bash
# up415.sh NAME INPUT : input agent (uinput), setup + start the 968 resident in a private profile, synthetic capture, notifications on.
set -u
n=$1 input=$2
root=$HOME/work/put.gd-i415; cd "$root" || exit 2; export AFA_ROOT=$root
A="python3 -I tool/afa.py"
img=$root/images/putgd-26.1022.968-nightly-x86_64.AppImage
if [ "$input" = uinput ] && [ ! -S /run/user/$(id -u)/putgd-lab-input.sock ]; then
  sudo -n python3 -I tool/host_input.py serve --socket /run/user/$(id -u)/putgd-lab-input.sock --user patrick > input-$n.log 2>&1 &
  echo $! > input-$n.pid; for _ in $(seq 20); do [ -S /run/user/$(id -u)/putgd-lab-input.sock ] && break; sleep .5; done
fi
$A setup $n --appimage "$img" --vnc-port 5900 --input $input > setup-$n.json || { echo setup-failed; exit 4; }
d=runs/$n; $A start $n > $d/start.json || { echo start-failed; exit 5; }
python3 -I tool/i326.py synth $n > /dev/null
python3 - $n <<'PY'
import sys; sys.path.insert(0, 'tool'); import afa
run = afa.Run(sys.argv[1]); print('notifications on', afa.set_notifications(run, True))
PY
python3 -c "import json;s=json.load(open('$d/start.json'));print('started', s['startSeconds'], s['credentials'], [(p['comm'],p['qtPlatformPlugins']) for p in s['processes']])"
