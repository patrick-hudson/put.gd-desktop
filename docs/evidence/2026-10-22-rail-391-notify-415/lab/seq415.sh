#!/bin/bash
# seq415.sh NAME TAG : the acceptance's order before F: notifications off, region-size --coords large and small, notifications on, one trial.
set -u
n=$1 tag=$2
root=$HOME/work/put.gd-i415; cd "$root" || exit 2; export AFA_ROOT=$root
notif() { python3 - $n $1 <<'PY'
import sys; sys.path.insert(0, 'tool'); import afa
print('notifications', sys.argv[2], afa.set_notifications(afa.Run(sys.argv[1]), sys.argv[2] == 'on'))
PY
}
notif off
python3 -I tool/rerun.py region-size $n --drag 400,200,1520,880 --coords > runs/$n/trials/$n-$tag-coords-large.json 2>&1; echo coords-large $?
python3 -I tool/rerun.py region-size $n --drag 900,500,980,560 --coords > runs/$n/trials/$n-$tag-coords-small.json 2>&1; echo coords-small $?
notif on
bash tool/batch415.sh $n putgd 0.2 1 $tag
