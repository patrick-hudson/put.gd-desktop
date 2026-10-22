#!/bin/bash
# batch415.sh NAME KIND HOVER COUNT TAG : COUNT trials, 14 s apart, one summary line each; JSON in runs/NAME/trials/.
set -u
n=$1 kind=$2 hover=$3 count=$4 tag=$5 settle=${6:-2.0} at=${7:-}
root=$HOME/work/put.gd-i415; cd "$root" || exit 2; export AFA_ROOT=$root
mkdir -p runs/$n/trials
for i in $(seq $count); do
  l=$n-$tag-$i
  python3 -I tool/trial415.py $kind $n --hover $hover --settle $settle ${at:+--at $at} --label $l > runs/$n/trials/$l.json 2> runs/$n/trials/$l.err
  python3 -c "import json;d=json.load(open('runs/$n/trials/$l.json'));print('$l', d['at'], 'press+%ss'%d.get('pressAfterShownSeconds'), 'clicked', d.get('clicked'), 'invoked', d['actionInvoked'], 'members', d['dbusMembers'], 'after', [c['box'] for c in d.get('bannerAfter',{}).get('components',[])], 'urls', len(d['openedUrls']))" 2>&1 || tail -3 runs/$n/trials/$l.err
  sleep 14
done
