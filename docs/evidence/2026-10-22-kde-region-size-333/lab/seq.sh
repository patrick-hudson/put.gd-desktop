#!/bin/sh
# usage: seq.sh RUN SPEC...   SPEC = x0,y0,x1,y1:map:label(- for no frame)
run=$1; shift
cd ~/work/put.gd-333/probe
for spec in "$@"; do
  drag=${spec%%:*}; rest=${spec#*:}; map=${rest%%:*}; label=${rest#*:}
  f=""; [ "$label" != - ] && f="--frame $label"
  python3 -I region333.py region "$run" --drag "$drag" --map "$map" --label "$map" --size "${SIZE:-1920,1080}" $f | python3 -c 'import json,sys;d=json.load(sys.stdin);o=d["operation"];print(d["map"],d["drag"],"expected",d["expected"],"png",d["savedFile"]["pngSize"],"op",o["state"],"hist",bool(d["history"]),"sha256 agree",d["sha256"]["agree"],"PASS" if d["pass"] else "FAIL")'
  sleep 1
done
