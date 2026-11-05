#!/bin/bash
# u348mem.sh HOSTTAG: idle RSS of lab 9000 (base, without #348/#349) then lab 9001 (PR head), same private
# profile shape, --background as at login, 30 s settle, 180 s windows before and after an update.check.
cd "${UPD_ROOT:-$HOME/work/put.gd-u348acc}" || exit 1
U="python3 -I tool/upd293.py"
for spec in mb:9000 mh:9001; do
  IFS=: read tag number <<< "$spec"; n=$1-$tag
  $U setup $n --appimage images/putgd-0.1.1-nightly.$number-x86_64.AppImage --scenario b --vnc-port 0 > /dev/null || { echo "setup $n failed"; exit 1; }
  $U mem $n --action check > runs/$n/mem.json
  mkdir -p results/$n && cp runs/$n/mem.json results/$n/
  python3 - $n <<PY
import json,sys
n=sys.argv[1]; m=json.load(open(f"runs/{n}/mem.json"))["mem"]
for w in ("A","B"):
    x=m[w]; print(n, m["version"], m["session"], w, x["window"], "| RSS median", x["rssMedianMiB"], "min", x["rssMinMiB"], "max", x["rssMaxMiB"], "| Pss", x["pssMiB"], "| libs", x["tlsNetworkLibraries"], "| load", [round(v,2) for v in x["load"]])
PY
  $U cleanup $n > /dev/null
done
