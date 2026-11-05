#!/bin/sh
# kwin_close.sh: close only put.gd's own UI windows (resourceClass putgd-ui) via a temporary KWin script.
set -e
PID=$(pgrep -u "$(id -u)" -x plasmashell | head -1)
export $(tr '\0' '\n' < /proc/$PID/environ | grep -E '^(DBUS_SESSION_BUS_ADDRESS)=' | xargs)
F=$(mktemp /tmp/pt374-kwin.XXXXXX.js)
cat > "$F" <<'JS'
var n = 0;
for (var w of workspace.windowList()) if (w.resourceClass == "putgd-ui") { print("pt374 close " + w.caption); w.closeWindow(); n++; }
print("pt374 closed " + n);
JS
ID=$(gdbus call --session --dest org.kde.KWin --object-path /Scripting --method org.kde.kwin.Scripting.loadScript "$F" pt374close | sed 's/[^0-9]//g')
gdbus call --session --dest org.kde.KWin --object-path /Scripting/Script$ID --method org.kde.kwin.Script.run >/dev/null
sleep 1
gdbus call --session --dest org.kde.KWin --object-path /Scripting --method org.kde.kwin.Scripting.unloadScript pt374close >/dev/null
rm -f "$F"
journalctl --user --since "-10s" -o cat | grep "pt374" || true
