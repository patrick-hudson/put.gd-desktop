#!/bin/bash
set -eu
out=/tmp/p560$1; work=/home/patrick/work/i560o/$1
umask 077; mkdir "$work"; exec >"$work/runner.log" 2>&1
cin=$(pgrep -x -u "$(id -u)" cinnamon | head -1)
while IFS= read -r -d '' kv; do
 case "$kv" in HOME=*|USER=*|LOGNAME=*|PATH=*|LANG=*|DISPLAY=*|XAUTHORITY=*|DBUS_SESSION_BUS_ADDRESS=*|XDG_CURRENT_DESKTOP=*|XDG_SESSION_DESKTOP=*|XDG_SESSION_TYPE=*|XDG_SESSION_ID=*|DESKTOP_SESSION=*) export "$kv";; esac
done < /proc/$cin/environ
/usr/bin/putgd --version-json > "$work/installed-version.json"
set +e; python3 /home/patrick/work/i560o/functional_ci.py "$out" > "$work/functional.out" 2> "$work/functional.err"; echo "exit=$?" > "$work/exit.txt"
