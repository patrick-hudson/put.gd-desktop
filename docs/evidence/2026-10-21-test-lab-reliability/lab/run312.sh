#!/bin/bash
# #312 notification-action check on m74 (run under the m74 desktop lock).
# Loopback feed offering 0.1.2, private profile r312, the resident's first scheduled check,
# then a click on the Cinnamon banner's body. Records PIDs it starts in $R/pids.
set -u
R=/home/patrick/work/put.gd-r12tl
cd "$R"
echo "start $(date -u +%FT%TZ) load $(cut -d' ' -f1-3 /proc/loadavg)"
python3 -m http.server 18312 --bind 127.0.0.1 --directory "$R/feed" > "$R/feed-server.log" 2>&1 &
echo "feed $!" >> "$R/pids"
sleep 1
curl -s -o /dev/null -w "feed stable.json HTTP %{http_code}\n" http://127.0.0.1:18312/stable.json
echo "== GetServerInformation (shared bus)"
DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications -m org.freedesktop.Notifications.GetServerInformation
python3 accept312.py setup r312 --desktop cinnamon --bin "$R/bin" --feed http://127.0.0.1:18312/ | python3 -c 'import json,sys; d=json.load(sys.stdin); print("setup", d["setup"], d["run"]["runtime"])'
python3 accept312.py start r312
python3 accept312.py notify r312 --label m74-312 --max-wait 1000
echo "== status"
python3 accept312.py status r312
echo "end $(date -u +%FT%TZ) load $(cut -d' ' -f1-3 /proc/loadavg)"
