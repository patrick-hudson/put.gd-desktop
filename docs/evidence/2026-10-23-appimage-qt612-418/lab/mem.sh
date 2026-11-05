#!/bin/bash
# mem.sh HOST INPUT VNCPORT : #418 idle-RSS comparison, Qt 6.8.3 (nightly 1058) vs Qt 6.12.0 (PR build), A B A B.
# Each run: afa.py setup (private profile, loopback fixture), start, 180 s idle (memory), ui.quit, cleanup.
# Nothing is clicked or captured. Results: runs/mem-*/memory.json and mem-HOST.txt.
set -u
host=$1 input=$2 vnc=$3
root=$HOME/work/put.gd-qt418/afa; cd "$root" || exit 2
export AFA_ROOT=$root
afa() { python3 -I tool/afa.py "$@"; }
old=$root/images/putgd-26.1023.1058-nightly-x86_64.AppImage new=${NEW:?NEW=the Qt 6.12 AppImage}
summary=$root/mem-$host.txt
for round in 1 2; do for which in old new; do
  img=${!which}; name=mem-$host-$which-$round
  printf '%s %s start %s\n' "$(date -u +%FT%TZ)" "$name" "$(uptime | sed 's/.*load/load/')" >> "$summary"
  allow=""; [ -e ~/.local/share/putgd ] && allow=--allow-data-home
  afa setup "$name" --appimage "$img" --vnc-port "$vnc" --input "$input" $allow > /dev/null || { echo "$name setup-failed" >> "$summary"; continue; }
  d=runs/$name
  afa start "$name" > "$d/start.json" || { echo "$name start-failed" >> "$summary"; afa cleanup "$name" --state > "$d/cleanup.json"; continue; }
  afa memory "$name" --seconds 180 > "$d/memory.json"
  python3 -I -c "import sys; sys.path.insert(0, 'tool'); import afa; r = afa.Run('$name'); print(afa.quit_resident(r))" > "$d/quit.txt" 2>&1
  afa cleanup "$name" > "$d/cleanup.json"
  python3 - "$d" "$name" >> "$summary" <<'PY'
import json, sys
d, name = sys.argv[1:]; m = json.load(open(f'{d}/memory.json')); s = json.load(open(f'{d}/start.json'))
print(name, 'qt', s['diagnostics'].get('qtVersion'), 'build', (s['diagnostics'].get('build') or {}).get('version'),
      'rssFinal', m['finalRssMiB'], 'rssPeak', m['peakRssMiB'], 'pss', m['pssMiB'], 'private', m['privateMiB'],
      'libs', m['libraryCount'], 'flagged', {k: v for k, v in m['flaggedLibraries'].items() if v}, 'load', [round(x[0], 2) for x in m['load']], 'pass', m['pass'])
PY
done; done
echo MEM-DONE >> "$summary"
