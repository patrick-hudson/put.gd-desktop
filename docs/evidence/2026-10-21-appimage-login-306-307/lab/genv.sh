# source in the guest: the running Cinnamon session's environment
pid=$(pgrep -u "$(id -u)" -x cinnamon | head -1)
eval "$(tr '\0' '\n' < /proc/$pid/environ | grep -E '^(DISPLAY|XAUTHORITY|DBUS_SESSION_BUS_ADDRESS|XDG_RUNTIME_DIR|XDG_CURRENT_DESKTOP|XDG_DATA_DIRS|XDG_SESSION_DESKTOP|DESKTOP_SESSION)=' | sed "s/'/'\\\\''/g; s/=\(.*\)/='\1'/; s/^/export /")"
export XDG_SESSION_TYPE=x11
