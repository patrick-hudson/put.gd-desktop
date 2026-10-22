#!/bin/bash
# ac.sh NAME INPUT VNCPORT : items A, B (start), C, then the first-open checks (#348, #349, #344 dropdown state).
# Runs on the host from SSH. Every step's JSON goes to runs/NAME/<step>.json; one line per step to runs/NAME/steps.log.
set -u
name=$1 input=$2 vnc=$3
root=$HOME/work/put.gd-afa2; cd "$root" || exit 2
export AFA_ROOT=$root
img=$root/images/putgd-0.1.1-nightly.942-x86_64.AppImage
afa() { python3 -I tool/afa.py "$@"; }
rr() { python3 -I tool/rerun.py "$@"; }
mkdir -p "$root/pre-$name"
afa identity "$img" images/SHA256SUMS.942 v0.1.1-nightly.942 --save "$root/pre-$name/identity.json" > /dev/null || echo identity-failed
afa versions --save "$root/pre-$name/versions.json" > /dev/null || echo versions-failed
if [ "$input" = uinput ] && [ ! -S /run/user/$(id -u)/putgd-lab-input.sock ]; then
    sock=/run/user/$(id -u)/putgd-lab-input.sock
    sudo -n python3 -I tool/host_input.py serve --socket "$sock" --user patrick > "$root/input-$name.log" 2>&1 &
    echo $! > "$root/input-$name.pid"
    for _ in $(seq 20); do [ -S "$sock" ] && break; sleep .5; done
fi
afa setup "$name" --appimage "$img" --vnc-port "$vnc" --input "$input" > "$root/pre-$name/setup.json" || { echo setup-failed; exit 3; }
d=runs/$name; mv "$root/pre-$name"/* "$d/"; rmdir "$root/pre-$name"
log() { printf '%s %s\n' "$(date -u +%FT%TZ)" "$*" >> "$d/steps.log"; }
log setup; afa allow-full "$name" on > /dev/null
afa start "$name" > "$d/start.json" || { log start-failed; exit 4; }; log started
afa memory "$name" --seconds 180 > "$d/memory.json"; log memory
rr updates-first-open "$name" --label "$name-348-first-open" > "$d/updates-first-open.json"; log updates-first-open
rr status-clean "$name" --label start > "$d/status-clean-start.json"; log status-clean
rr channel-options "$name" --label "$name-channel" > "$d/channel-options.json"; log channel-options
python3 - "$d" <<'PY'
import json, sys
d = sys.argv[1]; j = lambda n: json.load(open(f'{d}/{n}.json'))
i = j('identity'); m = j('memory'); s = j('start'); c = j('channel-options')
print(json.dumps({'sha': i['sha256'][:12], 'hash': i['hashMatches'], 'tag': i['tagMatches'], 'updinfo': i['updateInformationMatches'], 'root': i['rootOk'],
  'start': s['startSeconds'], 'creds': s['credentials'], 'sock': s['sessionSock'], 'procs': [(p['comm'], p.get('platform')) for p in s['processes']],
  'rss': m['finalRssMiB'], 'peak': m['peakRssMiB'], 'pss': m['pssMiB'], 'flagged': m['flaggedLibraries'], 'load': m['load'], 'memPass': m['pass'],
  'channel': [c['storedChannel'], c['status']]}, default=str))
PY
