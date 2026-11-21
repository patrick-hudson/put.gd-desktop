#!/bin/bash
# tmatrix.sh FILE : timing subset runs under ~/work/r1805/TIMING.lock; labels get a t- prefix
cd ~/work/r1805
H=$(cat host)
if [ -e TIMING.lock ]; then echo "lock held: $(cat TIMING.lock)"; exit 3; fi
echo "coordinator tesseract timing subset since $(date -u +%FT%TZ)" > TIMING.lock
trap 'rm -f ~/work/r1805/TIMING.lock' EXIT
export BENCH_EXTRA="--ids $(cat tools/timing-ids.txt)"
while read -r line; do
  [ -z "$line" ] && continue; case $line in \#*) continue;; esac
  label=t-${line%% *}; rest=${line#* }
  envs=${rest%%--*}; args=${rest#*--}
  [ -f runs/$H/$label/summary.json ] && continue
  { uptime; ps -eo pcpu,comm --sort=-pcpu | sed -n 2,4p; } > runs/$H/$label.load
  env $envs bash tools/run.sh $label $args
  uptime >> runs/$H/$label.load
done < "$1"
echo MATRIX_DONE > runs/$H/tmatrix-$(basename $1).done
