#!/bin/bash
# login_n.sh HOST VNC INPUT pre|post: U11, start at login after an in-app update without a restart.
#   pre:  N with Launch at login on (the session's real autostart entry), Check now, Download and install (full
#         download), no restart, quit. Then relogin.sh (#397's) does the real logout and login.
#   post: which put.gd the login started (image path, version, parent), a frame, then cleanup (the login-started
#         resident by exact PID, the autostart entry, the folders the run created).
set -u
host=$1 vnc=$2 input=$3
A=/home/patrick/work/put.gd-ua/assets
cd /home/patrick/work/put.gd-ua/tool
U="python3 -I ua_app.py"
st() { python3 -c "import json,sys; d=json.load(sys.stdin); v=list(d.values())[0]; print($1)"; }
case $4 in
pre)
  $U setup $host-login --appimage $A/v26.1022.1017-nightly/putgd-26.1022.1017-nightly-x86_64.AppImage --live --input $input --vnc-port $vnc > /dev/null && echo setup
  $U start $host-login --label u11-1-start > /dev/null
  $U login $host-login enable | st "'login enable', v['entry'].count('X-putgd-Managed'), (v['script'] or '')[:0]"
  $U check $host-login --label u11-2-available | st "'check', v['status']['latest'], v['status']['updateAvailable']"
  $U install $host-login --label u11-3 | st "'install', v['install']['state'], (v['install'].get('result') or {}).get('method'), v['imageSha256'][:12], v['folder']"
  grep -E '^(configured|update_information)=' ~/.local/share/putgd/login/appimage-start.sh
  $U quit $host-login | python3 -c "import json,sys; print('quit', json.load(sys.stdin))" ;;
post)
  $U login $host-login check | st "'login check', v['version'], v['residents'], v['imageSha256'][:12]"
  $U frame $host-login --label u11-4-after-login > /dev/null
  $U cleanup $host-login | python3 -c "import json,sys; print('cleanup', json.load(sys.stdin)['residentGone'])"
  ls -d ~/.config/autostart/putgd.desktop ~/.local/share/putgd ~/.config/putgd 2>&1 | grep -v 'No such'; pgrep -u $(id -u) -x putgd || echo "no putgd process" ;;
esac
