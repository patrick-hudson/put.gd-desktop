#!/bin/bash
# labupd.sh HOST VNCPORT : #418 in-app update from the current nightly (26.1023.1058, Qt 6.8.3) to the PR head's lab nightly
# (v26.1023.1068-nightly, Qt 6.12.0) through the lab feed (upd293.py without --live: PUTGD_UPDATE_FEED_URL and the mirror
# under https://pkg.put.gd/lab), scenario a (the real appimageupdatetool, zsync).
set -u
h=$1 vnc=$2; n=$h-lab1058
root=$HOME/work/put.gd-qt418/afa; cd "$root" || exit 2
export UPD_ROOT=$root/upd AFA_ROOT=$root
U="python3 -I tool/upd293.py"; t() { date -u +%FT%TZ; }
mkdir -p upd
echo "$(t) J: in-app update 1058 -> the lab feed's nightly, v26.1023.1069-nightly (run $n) start" >> timeline-$h.txt
$U setup $n --appimage images/putgd-26.1023.1058-nightly-x86_64.AppImage --scenario a --tool images/appimageupdatetool-x86_64.AppImage --vnc-port $vnc > upd/$n-setup.json || { echo setup-failed; tail -3 upd/$n-setup.json; exit 3; }
d=upd/runs/$n
$U check $n > $d/check.json && $U install $n > $d/install.json && $U restart $n > $d/restart.json
python3 - $d <<'PY'
import json, sys, re
d = sys.argv[1]; c = json.load(open(f'{d}/check.json'))['check']; i = json.load(open(f'{d}/install.json'))['install']; r = json.load(open(f'{d}/restart.json'))['restart']
res = i['install'].get('result') or {}; z = res.get('zsync') or {}
ctl = re.compile(r'[\x00-\x08\x0b-\x1f\x7f]')
print(json.dumps({'checkLatest': (c['status'].get('result') or {}).get('latest'), 'state': i['install']['state'], 'method': res.get('method'), 'zsyncUsed': z.get('used'),
  'zsyncOutput': z.get('output'), 'zsyncOutputHasControl': bool(ctl.search(z.get('output') or '')), 'seconds': i['seconds'], 'image': i['imageSha256'][:12],
  'backup': (i['backupSha256'] or '')[:12], 'restartVersion': r['version'], 'afterRestart': r['afterRestart'], 'folder': r['folder'],
  'backupRemoved': ((r.get('install') or {}).get('installed') or {}).get('backupRemoved')}, default=str))
PY
$U quit $n > $d/quit.json; $U cleanup $n > upd/$n-cleanup.json
echo "$(t) J: in-app update done; run quit and cleaned" >> timeline-$h.txt
grep -c mount_putgd /proc/mounts
