#!/bin/sh
# #326 acceptance on one physical desktop: run NAME INPUT VNCPORT APPIMAGE
# Steps print one JSON line each into runs/NAME/steps.log; cleanup is separate (finish326.sh).
set -u
name=$1 input=$2 vnc=$3 image=$4
root=/home/patrick/work/put.gd-326/afa
cd "$root" || exit 2
export AFA_ROOT=$root
afa() { python3 tool/afa.py "$@"; }
i326() { python3 tool/i326.py "$@"; }
log() { printf '%s %s\n' "$(date -u +%FT%TZ)" "$*" >> "runs/$name/steps.log"; }
if [ "$input" = uinput ]; then
    sock=/run/user/$(id -u)/putgd-lab-input.sock
    sudo -n python3 tool/host_input.py serve --socket "$sock" --user patrick > "$root/input-$name.log" 2>&1 &
    echo $! > "$root/input-$name.pid"
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -S "$sock" ] && break; sleep .5; done
fi
afa setup "$name" --appimage "$image" --vnc-port "$vnc" --input "$input" > "$root/setup-$name.json" || exit 3
log setup
i326 wrap "$name" > /dev/null && afa allow-full "$name" on > /dev/null
afa start "$name" > "runs/$name/start.json" || { log start-failed; exit 4; }
log started
i326 handler-on "$name" > "runs/$name/handler-on.json" && i326 synth "$name" > /dev/null
i326 trace "$name" on > "runs/$name/trace-on.json"
dbus-monitor "interface=org.freedesktop.Notifications" "interface=org.freedesktop.portal.OpenURI" "interface=org.freedesktop.FileManager1" > "runs/$name/logs/dbus.txt" 2>&1 &
echo $! > "runs/$name/dbusmon.pid"
sleep 1
afa notify "$name" --label "$name-click" > "runs/$name/notify.json"; log notify
i326 full "$name" --label "$name-link-opened" --wait 1 > /dev/null
i326 open-link "$name" --label "$name" > "runs/$name/open-link.json"; log open-link
i326 full "$name" --label "$name-history-openlink" --wait 1 > /dev/null
i326 reveal "$name" --label "$name" > "runs/$name/reveal.json"; log reveal
kill "$(cat "runs/$name/dbusmon.pid")"
i326 trace "$name" off > /dev/null
i326 env-check "$name" --label "$name" > "runs/$name/env-check.json"
i326 host-programs "$name" --label "$name" > "runs/$name/host-programs.json"
python3 - "$name" <<'EOF'
import json, sys
n = sys.argv[1]; d = f'runs/{n}'
note = json.load(open(f'{d}/notify.json'))['notification']
ol = json.load(open(f'{d}/open-link.json')); rv = json.load(open(f'{d}/reveal.json'))
ec = json.load(open(f'{d}/env-check.json')); hp = json.load(open(f'{d}/host-programs.json'))
print(json.dumps({'click': {'actionInvoked': note.get('actionInvoked'), 'openedUrls': note.get('openedUrls')},
                  'openLink': {'pass': ol['pass'], 'result': ol['reply'].get('result'), 'recorded': ol['recorded']},
                  'reveal': {'pass': rv['pass'], 'dbus': rv['dbusMembers'], 'logged': [l['fields'] for l in rv['logged']], 'closed': rv['closedWith'], 'started': rv['fileManagersStarted']},
                  'execve': {'pass': ec['pass'], 'hostPrograms': ec['hostPrograms'], 'leaks': ec['leaks']},
                  'wrappers': {'pass': hp['pass'], 'calls': [(c['program'], c['argv'][:50], c['LD_LIBRARY_PATH'], c['exit']) for c in hp['calls']]}}, indent=1))
EOF
