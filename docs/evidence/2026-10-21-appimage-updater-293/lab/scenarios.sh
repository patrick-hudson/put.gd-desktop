#!/bin/bash
# scenarios.sh HOSTTAG VNCPORT: scenarios a, b, c (N -> N+1, restart, cleanup) with a one-line summary each.
cd /home/patrick/work/put.gd-i293acc
U="python3 -I tool/upd293.py"
for s in a b c; do
  n=$1-$s
  $U setup $n --appimage images/putgd-0.1.1-nightly.886-x86_64.AppImage --scenario $s --tool tool/aiu/appimageupdatetool-x86_64.AppImage --vnc-port $2 > /dev/null || { echo "setup $n failed"; exit 1; }
  $U check $n > runs/$n/check.json && $U install $n > runs/$n/install.json && $U restart $n > runs/$n/restart.json
  python3 - $n <<PY
import json,sys
n=sys.argv[1]; i=json.load(open(f"runs/{n}/install.json"))["install"]; r=json.load(open(f"runs/{n}/restart.json"))["restart"]
res=i["install"].get("result") or {}; z=res.get("zsync") or {}; e=i["install"].get("error") or {}
print(n, "| install:", i["install"]["state"], res.get("method"), e.get("code") or "", "| zsync used", z.get("used"), z.get("fallbackReason"), "|", (z.get("output") or "")[:80].replace(chr(27),""), "| sec", i["seconds"])
print("   image", i["imageSha256"][:12], "backup", (i["backupSha256"] or "")[:12], "| restart", r["version"], "| after restart: updateAvailable", r["afterRestart"]["updateAvailable"], "| backupRemoved", (r["install"] or {}).get("installed",{}).get("backupRemoved"), "| folder", r["folder"])
PY
  $U cleanup $n > /dev/null
done
ls -d ~/.local/share/putgd 2>/dev/null
