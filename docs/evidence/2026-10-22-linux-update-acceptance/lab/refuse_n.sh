#!/bin/bash
# refuse_n.sh HOST VNC INPUT CASES...: U12 on N against the live feed, one fresh run; then the live-mirror tamper modes when "tamper" is listed.
set -u
host=$1 vnc=$2 input=$3; shift 3
A=/home/patrick/work/put.gd-ua/assets
cd /home/patrick/work/put.gd-ua/tool
U="python3 -I ua_app.py"
st() { python3 -c "import json,sys; d=json.load(sys.stdin); v=list(d.values())[0]; print($1)"; }
$U setup $host-ref --appimage $A/v26.1022.1017-nightly/putgd-26.1022.1017-nightly-x86_64.AppImage --live --input $input --vnc-port $vnc > /dev/null && echo setup
$U start $host-ref --label u12-0-start > /dev/null
for c in "$@"; do
  if [ "$c" = tamper ]; then
    $U tamper $host-ref --tag v26.1022.1019-nightly --source live --modes image,unsigned,sums | st "'tamper', v['unchanged'], {m: (r['state'], (r['error'] or {}).get('code'), r['folder']) for m, r in v['results'].items()}"
    $U start $host-ref --label u12-9-after-tamper > /dev/null
  elif [ "$c" = double ]; then
    $U refuse $host-ref double | st "'double', v['first']['ok'], (v['second']['error'] or {}).get('code'), v['install']"
  else
    $U refuse $host-ref $c | st "'$c', v['reply']['ok'], (v['reply']['error'] or {}).get('code') or (v['install'].get('error') or {}).get('code'), (v['reply']['error'] or {}).get('message') or (v['install'].get('error') or {}).get('message'), v['unchanged'], v['folder'], v.get('upload')"
  fi
done
$U quit $host-ref > /dev/null; $U cleanup $host-ref | python3 -c "import json,sys; print('cleanup', json.load(sys.stdin)['residentGone'])"
