#!/bin/bash
# 200 runs of the proton_transport flood row for each binary, with CPU hogs alongside (#236).
# Usage: stress236.sh BUILD_DIR OUT HOGS [CPU]
#   BUILD_DIR holds test_proton_transport (after) and test_proton_transport.before.
#   With CPU set, the hogs and every test run are pinned to that CPU (taskset -c),
#   which models a small busy runner without raising the host's load past ~6.
set -u
build=$1 out=$2 hogs=$3 cpu=${4:-}
pin=(); [ -n "$cpu" ] && pin=(taskset -c "$cpu")
mkdir -p "$out"; cd "$build"
pids=()
for i in $(seq 1 "$hogs"); do "${pin[@]}" python3 -c 'while True: pass' & pids+=($!); done
echo "${pids[@]}" > "$out/hog.pids"
sleep 20
for variant in before after; do
  bin=./test_proton_transport; [ $variant = before ] && bin=./test_proton_transport.before
  : > "$out/$variant.tsv"
  for n in $(seq 1 200); do
    load=$(cut -d' ' -f1 /proc/loadavg)
    res=$("${pin[@]}" $bin invalidOrUnboundedProgressIsUnknown:flood 2>&1)
    st=PASS; echo "$res" | grep -q '^FAIL' && st=FAIL
    ms=$(echo "$res" | sed -n 's/.*blacklisted, \([0-9]*\)ms/\1/p')
    code=$(echo "$res" | sed -n 's/.*Actual   (r.error.code) *: "\(.*\)"/\1/p')
    printf '%s\t%s\t%s\t%s\t%s\n' "$n" "$st" "$ms" "$load" "$code" >> "$out/$variant.tsv"
  done
done
kill "${pids[@]}"
