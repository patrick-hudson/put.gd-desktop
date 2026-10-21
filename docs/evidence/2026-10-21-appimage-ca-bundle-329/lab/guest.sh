#!/bin/sh
# #329 acceptance in the Aurora guest, driven from f44: guest.sh NAME IMAGE_NAME (image already in the guest dir).
# Results land in /home/patrick/work/put.gd-i329/results/guest-NAME*.json on f44.
set -u
name=$1 image=$2
T=/home/patrick/work/put.gd-i329/tool
R=/home/patrick/work/put.gd-i329/results; mkdir -p $R
G=/var/home/lab/put.gd-i329
B=$(python3 -c "import json;print(json.load(open('$T/state-allPublic.json'))['bucket'])")
sh $T/g.sh "cd $G && I329_ROOT=$G python3 -I tool/i329.py setup $name --appimage $G/$image --acl omit --b2-bucket $B --action-profile aws" > $R/guest-$name-setup.json || exit 3
sh $T/g.sh "cd $G && I329_ROOT=$G python3 -I tool/i329.py bundle $name" > $R/guest-$name-bundle.json
sh $T/g.sh "cd $G && I329_ROOT=$G python3 -I tool/i329.py update-check $name" > $R/guest-$name-update.json
python3 -I $T/s3creds.py 2 | sh $T/g.sh "cd $G && I329_ROOT=$G python3 -I tool/i329.py live $name aws --delete" > $R/guest-$name-aws.json
sh $T/g.sh "cd $G && I329_ROOT=$G python3 -I tool/i329.py stop $name" > /dev/null
sh $T/g.sh "cd $G && I329_ROOT=$G python3 -I tool/i329.py setup ${name}b2 --appimage $G/$image --b2-bucket $B --action-profile b2" > /dev/null || exit 4
cd $T && python3 -I b2lab.py run allPublic -- sh $T/g.sh "cd $G && I329_ROOT=$G python3 -I tool/i329.py live ${name}b2 b2 --test --delete" > $R/guest-${name}b2.json
sh $T/g.sh "cd $G && I329_ROOT=$G python3 -I tool/i329.py stop ${name}b2" > /dev/null
sh $T/g.sh "cd $G && I329_ROOT=$G python3 -I tool/i329.py cleanup $name && I329_ROOT=$G python3 -I tool/i329.py cleanup ${name}b2" > /dev/null
python3 - $R $name <<'PY'
import json, sys
R, n = sys.argv[1], sys.argv[2]
u = json.load(open(f'{R}/guest-{n}-update.json'))['check']
a = json.load(open(f'{R}/guest-{n}-aws.json')); b = json.load(open(f'{R}/guest-{n}b2.json'))
print('bundle', json.load(open(f'{R}/guest-{n}-bundle.json'))['PUTGD_CA_BUNDLE'])
print('update', u.get('ok'), (u.get('error') or {}).get('code'), (u.get('error') or {}).get('details'), ((u.get('result') or {}).get('result') or {}).get('latest'))
print('aws', a['upload']['uploadStatus'], a.get('delete'))
print('b2', b.get('testConnection', {}).get('state'), b['upload']['uploadStatus'], b.get('linkFetch'), b.get('delete'))
PY
