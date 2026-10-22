#!/bin/bash
# live.sh NAME VNCPORT FROM : live in-app update FROM -> newest nightly on the real feed (upd293.py --live, scenario a, real appimageupdatetool)
set -u
n=$1 vnc=$2 from=$3
root=$HOME/work/put.gd-afa2; cd "$root" || exit 2
export UPD_ROOT=$root/upd
U="python3 -I tool/upd293.py"
mkdir -p upd
allow=""; [ -e ~/.local/share/putgd ] && allow=--allow-data-home
$U setup $n --live --appimage images/putgd-0.1.1-nightly.$from-x86_64.AppImage --scenario a --tool images/appimageupdatetool-x86_64.AppImage --vnc-port $vnc $allow > upd/$n-setup.json || { echo setup-failed; tail -3 upd/$n-setup.json; exit 3; }
d=upd/runs/$n
$U check $n > $d/check.json && $U install $n > $d/install.json && $U restart $n > $d/restart.json
python3 -I tool/rerun.py 2>/dev/null
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
ps -u "$USER" -o pid=,args= | grep -E 'mount_putgd' | grep -v grep | cut -c1-120
