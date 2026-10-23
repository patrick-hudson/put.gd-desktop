#!/bin/bash
# cycle.sh HOST LABEL OUTDIR: one logout/login cycle for #490 through r490.py on HOST.
# Records: the resident and session before, the logout, when the old resident
# exited, the next login's fresh resident, a desktop capture, and the old
# resident's log records. Signals nothing.
set -u
host=$1 label=$2 out=$3
mkdir -p "$out"
r() { ssh -n -o BatchMode=yes "$host" "cd ~/work/put.gd-490 && R490_CLI=\${R490_CLI:-$CLI} python3 r490.py $*"; }
stamp() { date -u +%FT%TZ; }
CLI=${CLI:-/usr/bin/putgd-cli}
r state > "$out/$label-1-before.json"
old=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); s=d["graphicalSession"]; p=[x for x in d["residents"] if x["exe"].split("/")[-1].removesuffix(" (deleted)")=="putgd" and x["session"] in (s, None)]; print(p[0]["pid"] if len(p)==1 else "")' "$out/$label-1-before.json")
oldsession=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["graphicalSession"])' "$out/$label-1-before.json")
[ -n "$old" ] || { echo "no single resident in the graphical session before $label"; exit 1; }
echo "$(stamp) $label old resident $old in session $oldsession" | tee -a "$out/$label-timeline.txt"
r logout > "$out/$label-2-logout.json"
t0=$(date +%s.%N)
gone=
for i in $(seq 1 120); do
  if ! ssh -n -o BatchMode=yes "$host" "test -d /proc/$old"; then gone=$(date +%s.%N); break; fi
  sleep 0.5
done
state=$(ssh -n -o BatchMode=yes "$host" "loginctl show-session $oldsession -p State --value 2>&1")
if [ -n "$gone" ]; then
  echo "$(stamp) $label old resident $old gone $(python3 -c "print(round($gone-$t0,1))") s after logout; session $oldsession: $state" | tee -a "$out/$label-timeline.txt"
else
  echo "$(stamp) $label FAIL old resident $old still running 60 s after logout; session $oldsession: $state" | tee -a "$out/$label-timeline.txt"
fi
ssh -n -o BatchMode=yes "$host" "pgrep -u \$(id -u) -x putgd -a; true" > "$out/$label-3-after-logout-processes.txt"
r log "$old" > "$out/$label-3-old-resident-log.json"
sleep 3
r relogin > "$out/$label-4-relogin.json"
echo "$(stamp) $label relogin requested" | tee -a "$out/$label-timeline.txt"
r wait-resident "$old" 180 > "$out/$label-5-fresh.json"
r state > "$out/$label-6-after.json"
new=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(" ".join(str(p["pid"])+"@"+str(p["session"]) for p in d.get("fresh",[])))' "$out/$label-5-fresh.json")
echo "$(stamp) $label fresh resident(s): ${new:-none}" | tee -a "$out/$label-timeline.txt"
sleep 8
r capture > "$out/$label-7-capture.json"
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(sys.argv[2], "capture", d.get("state"), d.get("error"), d.get("items"))' "$out/$label-7-capture.json" "$(stamp) $label" | tee -a "$out/$label-timeline.txt"
[ "${FRAME:-1}" = 1 ] && { r frame "results/$label-8-after-capture.png" > "$out/$label-8-frame.json"; scp -q "$host:work/put.gd-490/results/$label-8-after-capture.png" "$out/"; }
