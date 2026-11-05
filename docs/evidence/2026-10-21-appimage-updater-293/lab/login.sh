#!/bin/bash
# login.sh HOSTTAG: launch at login on lab N, install N+1 without restarting, quit, real logout and
# autologin, then: which image and version did login start. m74: Cinnamon + LightDM; d69: Plasma + SDDM
# (a temporary Relogin=true drop-in for one SDDM restart, removed right after).
set -u
cd /home/patrick/work/put.gd-i293acc
U="python3 -I tool/upd293.py"; n=$1-l; T=runs/$n/timeline.txt
$U setup $n --appimage images/putgd-0.1.1-nightly.886-x86_64.AppImage --scenario a --tool tool/aiu/appimageupdatetool-x86_64.AppImage --vnc-port 5900 > /dev/null || exit 1
$U login $n enable > runs/$n/login-enable.json && $U check $n > /dev/null && $U install $n > runs/$n/install.json && $U quit $n > /dev/null
python3 -c "import json; i=json.load(open('runs/$n/install.json'))['install']; print('$n install:', i['install']['state'], (i['install'].get('result') or {}).get('method'), i['imageSha256'][:12], i['folder'])"
env_of() { python3 -I -c "import sys; sys.path.insert(0,'tool'); import afa; e,_,_=afa.session_environment(); print(' '.join(f'{k}={v}' for k,v in e.items() if k in ('DISPLAY','XAUTHORITY','DBUS_SESSION_BUS_ADDRESS','WAYLAND_DISPLAY')))"; }
case $1 in
  m74) shell=cinnamon; env $(env_of) cinnamon-session-quit --logout --no-prompt ;;
  d69) shell=plasmashell; env $(env_of) dbus-send --session --type=method_call --dest=org.kde.Shutdown /Shutdown org.kde.Shutdown.logout ;;
esac
echo "logout $(date -u +%T)" >> $T
for i in $(seq 1 60); do pgrep -u patrick -x $shell >/dev/null || break; sleep 1; done; echo "$shell gone $(date -u +%T)" >> $T
case $1 in
  m74) sudo -n systemctl restart lightdm; echo "lightdm restart $(date -u +%T)" >> $T ;;
  d69) printf '[Autologin]\nRelogin=true\n' | sudo -n tee /etc/sddm.conf.d/zz-i293-relogin.conf > /dev/null; sudo -n systemctl restart sddm; echo "sddm restart with zz-i293-relogin.conf $(date -u +%T)" >> $T ;;
esac
for i in $(seq 1 90); do pgrep -u patrick -x $shell >/dev/null && break; sleep 1; done; echo "$shell up $(date -u +%T)" >> $T
[ "$1" = d69 ] && { sudo -n rm -f /etc/sddm.conf.d/zz-i293-relogin.conf; echo "drop-in removed $(date -u +%T)" >> $T; }
for i in $(seq 1 90); do pgrep -u patrick -x putgd >/dev/null && break; sleep 1; done; echo "putgd up $(date -u +%T)" >> $T
sleep 15
cat $T
$U login $n check > runs/$n/login-check.json
python3 -c "import json; c=json.load(open('runs/$n/login-check.json'))['loginCheck']; print('login started:', c['version'], c['imageSha256'][:12], [(r['pid'], r['parent'], r['appimage']) for r in c['residents']])"
