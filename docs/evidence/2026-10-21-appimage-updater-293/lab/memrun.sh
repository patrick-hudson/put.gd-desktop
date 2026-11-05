#!/bin/bash
# memrun.sh HOSTTAG: base (nightly 874, update.check) then head (lab 886, update.install), same private profile shape.
cd /home/patrick/work/put.gd-i293acc
U="python3 -I tool/upd293.py"
for spec in "mb:putgd-0.1.1-nightly.874-x86_64.AppImage:check" "mh:putgd-0.1.1-nightly.886-x86_64.AppImage:install"; do
  IFS=: read tag image action <<< "$spec"; n=$1-$tag
  $U setup $n --appimage images/$image --scenario b --vnc-port 0 > /dev/null || { echo "setup $n failed"; exit 1; }
  $U mem $n --action $action > runs/$n/mem.json
  python3 - $n <<PY
import json,sys
n=sys.argv[1]; m=json.load(open(f"runs/{n}/mem.json"))["mem"]
for w in ("A","B"):
    x=m[w]; print(n, m["version"], m["session"], w, x["window"], "| RSS median", x["rssMedianMiB"], "min", x["rssMinMiB"], "max", x["rssMaxMiB"], "| Pss", x["pssMiB"], "| libs", x["tlsNetworkLibraries"], "| load", [round(v,2) for v in x["load"]])
print("  ", m["action"])
PY
  $U cleanup $n > /dev/null
done
