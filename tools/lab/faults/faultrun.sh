#!/usr/bin/env bash
# Run the fault harness over a range of seeds, each in a new run folder.
#   faultrun.sh BUILD OUT FIRST LAST STEPS DEST CLASSES [extra action_fuzz.py options]
# e.g. faultrun.sh build/asan /tmp/fr1 1 20 200 http fs,net,res
# OUT holds runs/f-<dest>-<seed>. Keep OUT short: the session socket lives at OUT/runs/f-DEST-SEED/r/rt/putgd/session.sock, and a
# socket path has at most 107 bytes. Nothing is mounted: a full disk is a shim rule (ENOSPC, EDQUOT).
# A finding stops that seed (its folder is kept), the loop goes on, and the exit status is the number of findings.
# Each harness runs as its own process group. SIGINT or SIGTERM to this script (Ctrl+C, or a kill of this script alone) is forwarded to
# that group; the script waits for the harness to clean up and exits with 128 + the signal. A signal to the whole group reaches both.
set -uo pipefail
set -m     # a background job gets a process group of its own, which the traps below signal
child=''
forward() {
  local sig=$1 code=$2
  trap '' INT TERM
  if [ -n "$child" ]; then
    kill -s "$sig" -- "-$child" 2> /dev/null
    wait "$child"
  fi
  exit "$code"
}
trap 'forward INT 130' INT
trap 'forward TERM 143' TERM
build=$1; out=$2; first=$3; last=$4; steps=$5; dest=$6; classes=$7; shift 7
here=$(cd "$(dirname "$0")" && pwd)
findings=0
mkdir -p "$out/runs"
for seed in $(seq "$first" "$last"); do
  run="$out/runs/f-$dest-$seed"
  [ -e "$run" ] && { echo "skip $seed: $run exists"; continue; }
  echo "== seed $seed $(date -u +%H:%M:%S)"
  dbus-run-session -- python3 -I "$here/../action_fuzz.py" --build "$build" --seed "$seed" --steps "$steps" --destination "$dest" \
    --run-dir "$run" faults --fault-classes "$classes" "$@" > "$out/runs/f-$dest-$seed.out" 2>&1 < /dev/null &
  child=$!
  wait "$child"
  status=$?
  child=
  tail -2 "$out/runs/f-$dest-$seed.out"
  [ "$status" -ge 128 ] && exit "$status"
  [ "$status" -ne 0 ] && findings=$((findings + 1))
done
exit "$findings"
