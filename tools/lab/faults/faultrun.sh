#!/usr/bin/env bash
# Run the fault harness over a range of seeds, each on a fresh small filesystem and a new run folder.
#   faultrun.sh BUILD OUT FIRST LAST STEPS DEST CLASSES [extra action_fuzz.py options]
# e.g. faultrun.sh build/asan ~/work/faults-1 1 20 200 http fs,net,res
# OUT holds runs/f-<dest>-<seed>, sfs/ (the small filesystem for config, data, state, cache and archive) and srt/ (the runtime
# directory). The small filesystem is 1300m (SFS_SIZE): put.gd keeps 1 GiB of its data disk free and refuses uploads below that. A finding stops that seed (its folder is kept), the loop goes on, and the exit status is the number of findings.
set -uo pipefail
build=$1; out=$2; first=$3; last=$4; steps=$5; dest=$6; classes=$7; shift 7
here=$(cd "$(dirname "$0")" && pwd)
findings=0
mkdir -p "$out/runs"
for seed in $(seq "$first" "$last"); do
  run="$out/runs/f-$dest-$seed"
  [ -e "$run" ] && { echo "skip $seed: $run exists"; continue; }
  for fs in sfs srt; do
    mountpoint -q "$out/$fs" && bash "$here/smallfs.sh" umount "$out/$fs"
  done
  bash "$here/smallfs.sh" mount "$out/sfs" "${SFS_SIZE:-1300m}" > /dev/null
  extra=()
  case ",$classes," in *,res,*) bash "$here/smallfs.sh" mount "$out/srt" 4m > /dev/null; extra=(--small-rt "$out/srt");; esac
  echo "== seed $seed $(date -u +%H:%M:%S)"
  dbus-run-session -- python3 -I "$here/../action_fuzz.py" --build "$build" --seed "$seed" --steps "$steps" --destination "$dest" \
    --run-dir "$run" faults --fault-classes "$classes" --small-fs "$out/sfs" "${extra[@]}" "$@" > "$out/runs/f-$dest-$seed.out" 2>&1
  status=$?
  tail -2 "$out/runs/f-$dest-$seed.out"
  if [ "$status" -ne 0 ]; then
    findings=$((findings + 1))
    # The small filesystems are remade for the next seed: keep what a finding left on them.
    for fs in sfs srt; do
      mountpoint -q "$out/$fs" && cp -a "$out/$fs" "$run/$fs-left" 2> /dev/null
    done
  fi
done
exit "$findings"
