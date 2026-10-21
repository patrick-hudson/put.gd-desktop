#!/bin/sh
# #329 headless acceptance on one builder: accept.sh NAME APPIMAGE
# Update check, the CA bundle the resident got, live S3 (key set 2) and live B2 (temporary bucket), each deleted again.
set -u
name=$1 image=$2
cd /home/patrick/work/put.gd-i329 || exit 2
B=$(python3 -c "import json;print(json.load(open('tool/state-allPublic.json'))['bucket'])")
python3 -I tool/i329.py setup "$name" --appimage "$image" --acl omit --b2-bucket "$B" --action-profile aws > /dev/null || exit 3
python3 -I tool/i329.py bundle "$name" > /dev/null
python3 -I tool/i329.py update-check "$name" > "runs/$name/update-check.json"
python3 -I tool/s3creds.py 2 | python3 -I tool/i329.py live "$name" aws --delete > "runs/$name/live-aws.json"
python3 -I tool/i329.py stop "$name" > /dev/null
python3 -I tool/i329.py cleanup "$name" > /dev/null
python3 -I tool/i329.py setup "${name}b2" --appimage "$image" --b2-bucket "$B" --action-profile b2 > /dev/null || exit 4
python3 -I tool/b2lab.py run allPublic -- python3 -I tool/i329.py live "${name}b2" b2 --test --delete > "runs/${name}b2/live-b2.json"
python3 -I tool/i329.py stop "${name}b2" > /dev/null
python3 -I tool/i329.py cleanup "${name}b2" > /dev/null
python3 - "$name" <<'PY'
import json, sys
n = sys.argv[1]; R = 'results'
u = json.load(open(f'{R}/{n}/update-check.json'))['check']; a = json.load(open(f'{R}/{n}/live-aws.json'))
b = json.load(open(f'{R}/{n}b2/live-b2.json')); ca = json.load(open(f'{R}/{n}/bundle.json'))
print('bundle', ca['PUTGD_CA_BUNDLE'])
print('update', u.get('ok'), (u.get('error') or {}).get('code'), (u.get('error') or {}).get('details'))
print('aws', a['upload']['uploadStatus'], a['upload']['link'], a.get('delete'))
print('b2', b.get('testConnection', {}).get('state'), b['upload']['uploadStatus'], b.get('linkFetch'), b.get('delete'))
PY
