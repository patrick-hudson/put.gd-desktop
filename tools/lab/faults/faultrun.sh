#!/usr/bin/env bash
# Run the fault harness over a range of seeds, each in a new run folder.
#   faultrun.sh BUILD OUT FIRST LAST STEPS DEST CLASSES [extra action_fuzz.py options]
# e.g. faultrun.sh build/asan /tmp/fr1 1 20 200 http fs,net,res
# OUT holds runs/f-<dest>-<seed>. Keep OUT short: the session socket lives at OUT/runs/f-DEST-SEED/r/rt/putgd/session.sock, and a
# socket path has at most 107 bytes. Nothing is mounted: a full disk is a shim rule (ENOSPC, EDQUOT).
# A finding stops that seed (its folder is kept), the loop goes on, and the exit status is the number of findings.
# Each harness runs in a session (and process group) of its own, under setsid. SIGINT or SIGTERM to this script (Ctrl+C, or a kill of this
# script alone) is forwarded to that group, also when it arrives while the harness is being launched; the script then waits until every
# process of the group is gone (not only dbus-run-session), up to GROUP_WAIT seconds, and exits with 128 + the signal. It never kills the
# group itself: the harness stops its own resident and receiver. A signal sent to the harness's group directly is handled the same way.
set -uo pipefail
child=''; pending=''; GROUP_WAIT=${GROUP_WAIT:-300}
group_gone() {  # wait until the harness's process group is empty; its ID cannot be reused while any process of it is left
  local end=$((SECONDS + GROUP_WAIT))
  while kill -0 -- "-$child" 2> /dev/null; do
    if [ "$SECONDS" -ge "$end" ]; then echo "faultrun: processes of the harness group $child still run after $GROUP_WAIT s" >&2; return 1; fi
    sleep 0.5
  done
}
forward() {
  local sig=$1 code=$2
  if [ -z "$child" ]; then pending="$sig $code"; return; fi   # still launching: forwarded as soon as the group exists
  trap '' INT TERM
  kill -s "$sig" -- "-$child" 2> /dev/null
  group_gone
  wait "$child" 2> /dev/null
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
  [ -n "$pending" ] && exit "${pending#* }"
  echo "== seed $seed $(date -u +%H:%M:%S)"
  # A background job is not a group leader, so setsid execs in place: $! is the harness's session and process group ID.
  setsid dbus-run-session -- python3 -I "$here/../action_fuzz.py" --build "$build" --seed "$seed" --steps "$steps" --destination "$dest" \
    --run-dir "$run" faults --fault-classes "$classes" "$@" > "$out/runs/f-$dest-$seed.out" 2>&1 < /dev/null &
  child=$!
  [ -n "$pending" ] && forward $pending   # a signal that came while the harness was being launched
  wait "$child"
  status=$?
  [ -n "$pending" ] && forward $pending
  group_gone || status=125
  child=''
  tail -2 "$out/runs/f-$dest-$seed.out"
  [ "$status" -ge 128 ] && exit "$status"
  [ "$status" -ne 0 ] && findings=$((findings + 1))
done
exit "$findings"
