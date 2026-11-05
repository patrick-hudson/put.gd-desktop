#!/bin/bash
# rck.sh HOST INPUT VNCPORT IMAGE TAG : #418 merged-head re-check (rule 16) on the CI AppImage of #497's head 786ed9fc.
# A (identity), start, one region capture under the cover (D), idle RSS once (C), the CVE-2025-10729 SVG through
# Upload clipboard (History frame kept), quit and clean. Private profile; nothing under ~/putgd-lab/proton.
set -u
h=$1 input=$2 vnc=$3 img=$4 tag=$5; n=$h-rck${RCK_RUN:-}
# RCK_RUN=2 MEM_FROM=runs/<first>/memory.json: rerun without the 180 s idle RSS, reusing the first run's.
root=$HOME/work/put.gd-qt418r/afa; cd "$root" || exit 2; export AFA_ROOT=$root
A="python3 -I tool/afa.py"; t() { date -u +%FT%TZ; }
d=runs/$n
echo "$(t) merged-head re-check start ($img)" >> timeline-$h.txt
$A identity "$img" images/SHA256SUMS.ci2 "$tag" --save $root/rck-identity.json > /dev/null
if [ "$input" = uinput ]; then
  sock=/run/user/$(id -u)/putgd-lab-input.sock
  if ! ps -eo args | grep -q "[h]ost_input.py serve"; then
    rm -f "$sock"; sudo -n python3 -I tool/host_input.py serve --socket "$sock" --user patrick > input-$n.log 2>&1 &
    echo $! > input-$n.pid; for _ in $(seq 20); do [ -S "$sock" ] && break; sleep .5; done
  fi
fi
$A setup $n --appimage "$img" --vnc-port $vnc --input $input > /dev/null || exit 4
mv $root/rck-identity.json $d/identity.json
$A allow-full $n on > /dev/null; $A start $n > $d/start.json || exit 5
if [ -n "${MEM_FROM:-}" ]; then cp "$MEM_FROM" $d/memory.json; else $A memory $n --seconds 180 > $d/memory.json; fi
$A cover $n start --pattern --precheck > $d/cover-start.json
$A capture $n region --drag 400,200,1520,880 > $d/capture-region.json
$A cover $n stop > $d/cover-stop.json
python3 -I tool/qt418.py clipfile $n samples/cve-2025-10729-pattern-orphan.svg --label $n-cve > $d/clipfile.json 2> $d/clipfile.err
python3 -I -c "import sys; sys.path.insert(0,'tool'); import afa; print(afa.quit_resident(afa.Run('$n')))" > $d/quit.txt 2>&1
$A cleanup $n > $d/cleanup.json
if [ -f input-$n.pid ]; then p=$(cat input-$n.pid); c=$(pgrep -P $p); sudo -n kill $c $p 2>/dev/null; rm -f input-$n.pid; sleep 1; ps -eo args | grep -q "[h]ost_input.py serve" || rm -f /run/user/$(id -u)/putgd-lab-input.sock; fi
echo "$(t) merged-head re-check done" >> timeline-$h.txt
python3 - $d <<'PY'
import json, sys
d = sys.argv[1]; j = lambda n: json.load(open(f'{d}/{n}.json'))
i, s, m, c, cf = j('identity'), j('start'), j('memory'), j('capture-region'), j('clipfile')
e = cf['entries'][0]
print(json.dumps({'sha': i['sha256'][:12], 'tag': i['tagMatches'], 'root': i['rootOk'], 'qt': s['diagnostics'].get('qtVersion'),
  'version': (s['diagnostics'].get('build') or {}).get('version'), 'rss': m['finalRssMiB'], 'pss': m['pssMiB'], 'flagged': {k: v for k, v in m['flaggedLibraries'].items() if v},
  'region': [c['operation']['state'], (c.get('png') or {}).get('size'), (c.get('coverCheck') or {}).get('ok'), bool(c.get('historyEntry'))],
  'cve': [(e.get('history') or {}).get('mime'), (e.get('preview') or {}).get('state'), e.get('frame')], 'survived': cf['survived'],
  'cleanup': json.load(open(f'{d}/cleanup.json'))['after']}, default=str))
PY
