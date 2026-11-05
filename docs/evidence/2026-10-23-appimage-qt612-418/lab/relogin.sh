#!/bin/bash
# relogin.sh HOST : real logout of the seat0 session, display-manager restart for autologin; prints UTC times.
h=$1; export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus
t() { date -u +%FT%TZ; }
shell=$(case $h in f44) echo gnome-shell;; m74) echo cinnamon;; *) echo plasmashell;; esac)
old=$(pgrep -u patrick -x $shell | head -1); echo "$(t) shell $shell pid $old"
case $h in
  f44) gnome-session-quit --logout --no-prompt; echo "$(t) logout requested (gnome-session-quit --logout)";;
  m74) export DISPLAY=:0; cinnamon-session-quit --logout --no-prompt; echo "$(t) logout requested (cinnamon-session-quit --logout)";;
  *) (qdbus6 org.kde.Shutdown /Shutdown org.kde.Shutdown.logout 2>/dev/null || qdbus org.kde.Shutdown /Shutdown org.kde.Shutdown.logout || busctl --user call org.kde.Shutdown /Shutdown org.kde.Shutdown logout); echo "$(t) logout requested (org.kde.Shutdown.logout)";;
esac
for i in $(seq 60); do kill -0 $old 2>/dev/null || break; sleep 1; done
kill -0 $old 2>/dev/null && echo "$(t) old shell still running after 60 s" || echo "$(t) old shell gone"
if [ $h = d69 ]; then printf '[Autologin]\nRelogin=true\n' | sudo -n tee /etc/sddm.conf.d/zz-qt418-relogin.conf >/dev/null; echo "$(t) temp drop-in /etc/sddm.conf.d/zz-qt418-relogin.conf (Relogin=true) added"; fi
dm=$(case $h in f44) echo gdm;; m74) echo lightdm;; *) echo sddm;; esac)
sudo -n systemctl restart $dm; echo "$(t) systemctl restart $dm"
for i in $(seq 90); do n=$(pgrep -u patrick -x $shell | head -1); [ -n "$n" ] && [ "$n" != "$old" ] && break; sleep 1; done
echo "$(t) new shell pid ${n:-none}"
if [ $h = d69 ]; then sleep 3; sudo -n rm -f /etc/sddm.conf.d/zz-qt418-relogin.conf; echo "$(t) temp drop-in removed"; fi
loginctl list-sessions --no-legend | grep seat0
