#!/bin/sh
# runall.sh DIR TRIALS: every scenario x signal x tree, 3 at a time, niced.
cd "$1"
for tree in core head; do for sc in click-guest click-physical drag held wheel browser-exit browser-setup fixture-setup runner-cleanup; do for sig in ALRM TERM HUP INT; do
  echo "$tree $sc $sig"
done; done; done | xargs -P 3 -L 1 sh -c 'nice -n 10 timeout 1200 python3 sweep.py "$1" "$2" "$3" '"$2"' 2>/dev/null | tail -1 > "results/$1-$2-$3.json"' _
