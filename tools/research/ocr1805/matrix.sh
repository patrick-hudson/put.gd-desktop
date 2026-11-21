#!/bin/bash
# matrix.sh FILE : each line "LABEL [ENV=V ...] -- ARGS"; runs sequentially, skips done labels
cd ~/work/r1805
H=$(cat host)
while read -r line; do
  [ -z "$line" ] && continue; case $line in \#*) continue;; esac
  label=${line%% *}; rest=${line#* }
  envs=${rest%%--*}; args=${rest#*--}
  [ -f runs/$H/$label/summary.json ] && continue
  env $envs bash tools/run.sh $label $args
done < "$1"
echo MATRIX_DONE > runs/$H/matrix-$(basename $1).done
