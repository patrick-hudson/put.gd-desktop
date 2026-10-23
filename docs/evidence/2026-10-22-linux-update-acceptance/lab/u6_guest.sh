#!/bin/bash
# u6_guest.sh RUN stable|nightly: U6 live in a VM guest whose passwordless sudo covers only apt-get. The guest's
# documented lab password arrives as the first line of stdin and reaches sudo -S on stdin only (never argv or a log).
# The Other channel commands come from update.status (action.otherChannel.commands) and run as given, with sudo -S
# and apt-get -y in place of the prompts. Then: the installed version, the sources file against both packaged
# channel files, a Check now status and a Settings frame.
set -u
run=$1 to=$2
IFS= read -r pw
cd ~/ua/tool
export UPD_VNC_HOST=10.0.2.2 AFA_VNC_HOST=10.0.2.2
log=~/ua/u6-to-$to.log
status() { python3 -I ua_pkg.py check "$run" --label "$1"; }
{
status u6-to-$to-before > ~/ua/u6-to-$to-before.json
python3 -c "
import json; v=list(json.load(open('$HOME/ua/u6-to-$to-before.json')).values())[0]['status']
print('before', v['build'], v['channel'], v['channelSource'])
for c in v['action']['otherChannel']['commands']: print(c)" > ~/ua/u6-cmds.txt
head -1 ~/ua/u6-cmds.txt
tail -n +2 ~/ua/u6-cmds.txt | while IFS= read -r c; do
  echo "### $c"
  c=${c/sudo apt install/sudo apt-get install -y}; c=${c/sudo apt update/sudo apt-get update}
  c=${c//sudo /sudo -S -p '' }
  printf '%s\n' "$pw" | bash -c "$c" 2>&1 | tail -4; echo "rc=${PIPESTATUS[1]}"
done
echo "installed $(dpkg-query -W -f '${Version}' putgd)"
for f in stable nightly; do cmp -s /etc/apt/sources.list.d/putgd.sources /usr/share/putgd/repo/putgd-$f.sources && echo "putgd.sources == putgd-$f.sources"; done
sha256sum /etc/apt/sources.list.d/putgd.sources | cut -c1-12
python3 -I ua_pkg.py restart "$run" --label u6-after-$to > /dev/null
status u6-after-$to | python3 -c "
import json,sys; v=list(json.load(sys.stdin).values())[0]['status']
print('after', v['build'], v['channel'], v['channelSource'], 'latest', v.get('latest'), 'available', v.get('updateAvailable'))
print('status text:', (v.get('statusText') or v.get('message') or '')[:300])"
python3 -I ua_pkg.py frame "$run" --label u6-after-$to-1017 > /dev/null && echo framed
} 2>&1 | tee "$log"
