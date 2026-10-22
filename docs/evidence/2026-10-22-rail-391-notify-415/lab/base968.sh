#!/bin/bash
# base968.sh PRRUN NAME INPUT : quit the PR run, then the same steps on 968 (recorder stays on from PRRUN): setup, start, 2 trials, 90 s memory.
set -u
p=$1 n=$2 input=$3
root=$HOME/work/put.gd-i415; cd "$root" || exit 2; export AFA_ROOT=$root
A="python3 -I tool/afa.py"
python3 - $p <<'PY'
import sys; sys.path.insert(0, 'tool'); import afa
print('pr quit', afa.quit_resident(afa.Run(sys.argv[1]))['apprunGone'])
PY
$A cleanup $p | python3 -c 'import json,sys;print("pr cleanup", json.load(sys.stdin).get("after"))'
$A setup $n --appimage "$root/images/putgd-26.1022.968-nightly-x86_64.AppImage" --vnc-port 5900 --input $input > setup-$n.json || { echo setup-failed; exit 4; }
$A start $n > runs/$n/start.json || { echo start-failed; exit 5; }
python3 -I tool/i326.py synth $n > /dev/null
python3 - $n <<'PY'
import sys; sys.path.insert(0, 'tool'); import afa
print('notifications on', afa.set_notifications(afa.Run(sys.argv[1]), True))
PY
bash tool/batch415.sh $n putgd 0.2 2 base
sleep 6; $A memory $n --seconds 90 > runs/$n/memory.json 2>&1
python3 -c "import json;m=json.load(open('runs/$n/memory.json'));print('$n', {k:m.get(k) for k in ('finalRssMiB','peakRssMiB','pssMiB','pass','load')})"
