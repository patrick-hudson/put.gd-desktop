#!/usr/bin/env bash
# Run the fault harness over a range of seeds, each on a fresh small filesystem and a new run folder.
#   faultrun.sh BUILD OUT FIRST LAST STEPS DEST CLASSES [extra action_fuzz.py options]
# e.g. faultrun.sh build/asan /tmp/fr1 1 20 200 http fs,net,res
# OUT holds runs/f-<dest>-<seed>, sfs/ (the small filesystem for config, data, state, cache and archive) and srt/ (the runtime
# directory). Keep OUT short: the session socket lives at OUT/srt/putgd/session.sock (with res) or
# OUT/runs/f-DEST-SEED/r/rt/putgd/session.sock, and a socket path has at most 107 bytes. The small filesystem is 1300m (SFS_SIZE):
# put.gd keeps 1 GiB of its data disk free and refuses uploads below that.
# A finding stops that seed (its folder is kept, with a best-effort copy of what was left on the small filesystems), the loop goes
# on, and the exit status is the number of findings. A mount or unmount that fails stops the whole run (status 100). On any exit,
# Ctrl+C included, the script unmounts the small filesystems it mounted itself, and nothing else.
set -uo pipefail
build=$1; out=$2; first=$3; last=$4; steps=$5; dest=$6; classes=$7; shift 7
here=$(cd "$(dirname "$0")" && pwd)
out=$(realpath -m -- "$out")
findings=0
mounted=()   # the small filesystems this invocation mounted and has not unmounted yet

unmount() {  # unmount one of ours; smallfs.sh checks the mount is the one it made
  local fs=$1 kept=()
  bash "$here/smallfs.sh" umount "$fs" || return 1
  for m in "${mounted[@]}"; do [ "$m" = "$fs" ] || kept+=("$m"); done
  mounted=("${kept[@]}")
}
mount_small() {
  bash "$here/smallfs.sh" mount "$1" "$2" > /dev/null || { echo "faultrun: mounting $1 failed: stopping" >&2; exit 100; }
  mounted+=("$1")
}
cleanup() {
  local status=$?
  trap - EXIT INT TERM
  for fs in "${mounted[@]}"; do
    bash "$here/smallfs.sh" umount "$fs" || { echo "faultrun: could not unmount $fs" >&2; status=100; }
  done
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$out/runs"
for seed in $(seq "$first" "$last"); do
  run="$out/runs/f-$dest-$seed"
  [ -e "$run" ] && { echo "skip $seed: $run exists"; continue; }
  mount_small "$out/sfs" "${SFS_SIZE:-1300m}"
  extra=()
  case ",$classes," in *,res,*) mount_small "$out/srt" 4m; extra=(--small-rt "$out/srt");; esac
  echo "== seed $seed $(date -u +%H:%M:%S)"
  dbus-run-session -- python3 -I "$here/../action_fuzz.py" --build "$build" --seed "$seed" --steps "$steps" --destination "$dest" \
    --run-dir "$run" faults --fault-classes "$classes" --small-fs "$out/sfs" "${extra[@]}" "$@" > "$out/runs/f-$dest-$seed.out" 2>&1
  status=$?
  tail -2 "$out/runs/f-$dest-$seed.out"
  [ "$status" -ge 128 ] && exit "$status"   # the harness was interrupted: stop, and clean up
  if [ "$status" -ne 0 ]; then
    findings=$((findings + 1))
    # The small filesystems are remade for the next seed: keep what a finding left on them (best effort; the filler is not copied).
    for fs in sfs srt; do
      if [ -d "$run" ] && mountpoint -q "$out/$fs"; then
        mkdir -p "$run/$fs-left" && tar -C "$out/$fs" --exclude=./.filler -cf - . 2> /dev/null | tar -C "$run/$fs-left" -xf - 2> /dev/null || true
      fi
    done
  fi
  for fs in "${mounted[@]}"; do
    unmount "$fs" || { echo "faultrun: unmounting $fs failed: stopping" >&2; exit 100; }
  done
done
exit "$findings"
