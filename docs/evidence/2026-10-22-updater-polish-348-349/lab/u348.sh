#!/bin/bash
# u348.sh HOSTTAG VNCPORT (#348/#349 lab acceptance, after #293 phase 3's scenarios.sh):
#   before: lab 9000 (nightly 33c768ec, without the fixes) -> lab 9002 by zsync; update.status as it was
#   after:  lab 9001 (PR head) -> lab 9002 by zsync; a fresh window from one ui.show updates at start and after restart
# Run from $UPD_ROOT (default ~/work/put.gd-u348acc) holding tool/ (upd293.py, afa.py, rfb.py,
# appimageupdatetool-x86_64.AppImage) and images/.
cd "${UPD_ROOT:-$HOME/work/put.gd-u348acc}" || exit 1
U="python3 -I tool/upd293.py"
for spec in before:9000 after:9001; do
  IFS=: read label number <<< "$spec"; n=$1-$label
  $U setup $n --appimage images/putgd-0.1.1-nightly.$number-x86_64.AppImage --scenario a --tool tool/appimageupdatetool-x86_64.AppImage --vnc-port $2 > /dev/null || { echo "setup $n failed"; exit 1; }
  $U start $n --label first-show > runs/$n/start.json && $U check $n > runs/$n/check.json && $U install $n > runs/$n/install.json
  [ $label = after ] && $U restart $n > runs/$n/restart.json
  python3 - $n <<PY
import json,sys
n=sys.argv[1]; i=json.load(open(f"runs/{n}/install.json"))["install"]; res=i["install"].get("result") or {}; z=res.get("zsync") or {}
print(n, "| install:", i["install"]["state"], res.get("method"), "| zsync used", z.get("used"), "| sec", i["seconds"], "| image", i["imageSha256"][:12])
print("   zsync.output:", json.dumps(z.get("output")))
try:
    r=json.load(open(f"runs/{n}/restart.json"))["restart"]; print("   restart", r["version"], "| updateAvailable", r["afterRestart"]["updateAvailable"])
except FileNotFoundError: pass
PY
  # The resident's own log line for the install (#349: the same output, cleaned).
  mkdir -p results/$n && grep -rh "Update installed" runs/$n/state/putgd/ > results/$n/install-log.jsonl 2>/dev/null
  cp runs/$n/*.json results/$n/ 2>/dev/null
  $U cleanup $n > /dev/null
done
ls -d ~/.local/share/putgd 2>/dev/null
