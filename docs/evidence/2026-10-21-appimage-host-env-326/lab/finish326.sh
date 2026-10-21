#!/bin/sh
# Undo one run of run326.sh: the default handler, the resident, afa's run state, the input agent.
set -u
name=$1
root=/home/patrick/work/put.gd-326/afa
cd "$root" || exit 2
export AFA_ROOT=$root
python3 tool/i326.py handler-off "$name" > "runs/$name/handler-off.json" 2>&1; grep -E '"same"|"filesMatch"' "runs/$name/handler-off.json"
python3 tool/afa.py quit-tray "$name" > "runs/$name/quit.json" 2>&1
python3 tool/afa.py cleanup "$name" > "runs/$name/cleanup.json" 2>&1
python3 -c "import json;d=json.load(open('runs/$name/cleanup.json'));print('after', d['after'])"
if [ -f "input-$name.pid" ]; then
    pid=$(cat "input-$name.pid")
    # sudo relays the signal to its exact agent child.
    sudo -n kill "$pid"; sleep 1; [ -d "/proc/$pid" ] && echo "agent $pid still running"
    rm -f "input-$name.pid"
fi
pgrep -u "$(id -u)" -af 'putgd|host_input|videotestsrc|cover_player|strace' | grep -v -E 'pgrep|finish326' || echo 'no leftovers'
