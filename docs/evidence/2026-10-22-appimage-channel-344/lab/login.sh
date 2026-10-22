#!/bin/bash
# login.sh (#344 lab acceptance): start at login after a channel switch, up to the logout.
# Launch at login on in nightly 9101; a newer-named nightly decoy (a copy of 9102 named
# putgd-0.1.2-nightly.1-x86_64.AppImage) beside it; switch to stable without restarting;
# the login script before and after; quit. The real logout and login follow; then
# `upd344.py login NAME check`.
set -u
U="python3 upd344.py"
n=$(hostname -s)-login
script=$HOME/.local/share/putgd/login/appimage-start.sh
$U setup $n --appimage images/putgd-0.1.1-nightly.9101-x86_64.AppImage --tool tool/appimageupdatetool-x86_64.AppImage --vnc-port $1 > /dev/null || exit 1
$U start $n --label login-1-start > /dev/null
$U login $n enable > /dev/null
cp images/putgd-0.1.1-nightly.9102-x86_64.AppImage runs/$n/apps/putgd-0.1.2-nightly.1-x86_64.AppImage && chmod 755 runs/$n/apps/putgd-0.1.2-nightly.1-x86_64.AppImage
echo "before: $(grep -E '^(configured|update_information)=' "$script" | tr '\n' ' ')"
$U channel $n stable --label login-2-offer > /dev/null
$U install $n --channel stable --label login-3-switch | python3 -c "import json,sys;d=json.load(sys.stdin)['install'];print('install', d['install']['state'], d['install']['result']['method'], d['imageSha256'])"
sleep 2
echo "after: $(grep -E '^(configured|update_information)=' "$script" | tr '\n' ' ')"
ls -la runs/$n/apps/
$U quit $n
