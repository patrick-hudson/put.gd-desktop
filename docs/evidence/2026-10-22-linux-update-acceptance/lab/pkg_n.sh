#!/bin/bash
# pkg_n.sh HOST LABEL VNC INPUT: the start of U2-U5 and U14 on one desktop, with N installed from its release file.
# Installs N (only putgd), starts its resident in put.gd's default profile (notifications on, live feed), points it at
# the live S3 test prefix acceptance/ua/HOST/ (key set 2 from ~/Documents/creds.txt, on stdin only), changes the theme,
# makes two uploads, records a snapshot and starts the notification watch. Prints one line per step.
set -u
host=$1 label=$2 vnc=$3 input=$4
N=v26.1022.1017-nightly
cd /home/patrick/work/put.gd-ua/tool
R=/home/patrick/work/put.gd-ua/results; mkdir -p $R
U="python3 -I ua_pkg.py"
v=${N#v}; ext=deb; [ "$label" = fedora44 ] && ext=rpm
f=/home/patrick/work/put.gd-ua/assets/$N/putgd-$v-$label-x86_64.$ext
for d in ~/.config/putgd; do [ -e "$d" ] && { echo "refusing: $d exists"; exit 3; }; done
if [ $ext = deb ]; then sudo -n apt-get install -y "$f" > $R/$host-u2-install-$v.log 2>&1; else sudo -n dnf install -y "$f" > $R/$host-u2-install-$v.log 2>&1; fi
echo "install $? $( (dpkg-query -W -f='${Version}' putgd || rpm -q putgd) 2>/dev/null)"
$U start $host-pkg --vnc-port $vnc --label u4-0-n-start --input $input --profile default --feed live --notifications \
  | python3 -c "import json,sys;d=list(json.load(sys.stdin).values())[0];s=d['status'];print('start', s['build'], s['channelSource'], s['latest'], d['repo'].get('sameAs'))"
$U watch $host-pkg start --label u14 > /dev/null && echo "watch on"
python3 -I s3creds.py 2 | $U live-s3 $host-pkg --prefix acceptance/ua/$host/ | python3 -c "import json,sys;d=json.load(sys.stdin)['liveS3'];print('live-s3', d['stored'], d.get('credentials'), d['profile']['objectPrefix'])"
$U patch $host-pkg '{"preferences":{"appearance":{"theme":"dark"}}}' | python3 -c "import json,sys;print('theme dark', json.load(sys.stdin)['patch']['ok'])"
$U upload $host-pkg --label u5-upload-1 | python3 -c "import json,sys;d=json.load(sys.stdin)['upload'];print('upload1', d['operation'], d['link'], d['served'])"
$U upload $host-pkg --label u5-upload-2 | python3 -c "import json,sys;d=json.load(sys.stdin)['upload'];print('upload2', d['operation'], d['link'], d['served'])"
$U snapshot $host-pkg --label u5-1-before | python3 -c "import json,sys;d=list(json.load(sys.stdin).values())[0];print('snapshot', d['build']['version'], d['config']['sha256'][:12], d['history']['total'], [r['uploadStatus'] for r in d['history']['records']])"
