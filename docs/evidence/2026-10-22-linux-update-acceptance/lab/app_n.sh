#!/bin/bash
# app_n.sh HOST VNC INPUT [SSL_CERT_FILE]: the AppImage routes on one desktop, N -> N+1 on the live feed and mirror.
#   sched  N with a fresh private profile and notifications on, no Check now: the resident's own scheduled check
#          (5-15 min after start) runs while the notification watch is on (U14 nightly never); stopped by `sched-end`.
#   zs     U7 zsync: the real appimageupdatetool on PATH; install, restart, quit.
#   cr     U7 forced fallback: a crashing appimageupdatetool; install (verified full download), then U12 "restart pending"
#          (a second install before the restart), restart, quit.
#   aut    U8: appimageupdatetool itself on N (SSL_CERT_FILE on Fedora), the new file checked against SHA256SUMS.
# Each prints one line per step; run.json and frames stay under ~/work/put.gd-ua/app.
set -u
host=$1 vnc=$2 input=$3 ssl=${4:-}
N=v26.1022.1017-nightly N1=v26.1022.1019-nightly
A=/home/patrick/work/put.gd-ua/assets
IMG=$A/$N/putgd-${N#v}-x86_64.AppImage TOOL=$A/appimageupdatetool-x86_64.AppImage
cd /home/patrick/work/put.gd-ua/tool
U="python3 -I ua_app.py"
st() { python3 -c "import json,sys; d=json.load(sys.stdin); v=list(d.values())[0] if len(d)==1 else d; print($1)"; }
allow=""; [ -e ~/.local/share/putgd ] && allow=--allow-data-home
case "${5:-all}" in
all|sched)
  $U setup $host-sched --appimage $IMG --live --notifications --input $input --vnc-port $vnc $allow | st "'sched setup', v['status']['build']['version'], v['status']['channel']"
  $U watch $host-sched start --label u14 | st "'sched watch', v['pid'], v['started']" ;;&
all|zs)
  $U setup $host-zs --appimage $IMG --live --tool $TOOL --input $input --vnc-port $vnc --allow-data-home > /dev/null && echo "zs setup"
  $U start $host-zs --label u7-zs-1-start | st "'zs start', v['status']['build'], v['status']['latest']"
  $U check $host-zs --label u7-zs-2-available | st "'zs check', v['status']['latest'], v['status']['updateAvailable'], v['status']['install'].get('available')"
  $U install $host-zs --label u7-zs-3 | st "'zs install', v['install']['state'], (v['install'].get('result') or {}).get('method'), ((v['install'].get('result') or {}).get('zsync') or {}).get('used'), v['seconds'], v['imageSha256'][:12], v['folder']"
  $U restart $host-zs --label u7-zs-4-restarted | st "'zs restart', v['version'], v['newAppimage'], v['afterRestart'], v['folder'], ((v.get('install') or {}).get('installed') or {}).get('backupRemoved')"
  $U quit $host-zs > /dev/null; $U cleanup $host-zs | st "'zs cleanup', v['residentGone']" ;;&
all|cr)
  $U setup $host-cr --appimage $IMG --live --crash --input $input --vnc-port $vnc --allow-data-home > /dev/null && echo "cr setup"
  $U start $host-cr --label u7-cr-1-start > /dev/null; $U check $host-cr --label u7-cr-2-available > /dev/null
  $U install $host-cr --label u7-cr-3 | st "'cr install', v['install']['state'], (v['install'].get('result') or {}).get('method'), ((v['install'].get('result') or {}).get('zsync') or {}).get('fallbackReason'), v['seconds'], v['imageSha256'][:12]"
  $U refuse $host-cr restart | st "'cr refuse restart', v['reply'], v['install'].get('restartRequired'), v['unchanged']"
  $U restart $host-cr --label u7-cr-4-restarted | st "'cr restart', v['version'], v['afterRestart'], v['folder']"
  $U quit $host-cr > /dev/null; $U cleanup $host-cr | st "'cr cleanup', v['residentGone']" ;;&
all|aut)
  $U setup $host-aut --appimage $IMG --live --input $input --vnc-port $vnc --allow-data-home > /dev/null && echo "aut setup"
  $U quit $host-aut > /dev/null
  extra=""; [ -n "$ssl" ] && extra="--ssl-cert-file $ssl"
  $U aut $host-aut --tool $TOOL --tag $N1 $extra | st "'aut', v['check']['exit'], v['update']['exit'], v['update']['seconds'], v['new'], v['matchesSums'], v['oldUnchanged'], v['check']['output'][-160:].replace(chr(10),' ')"
  $U cleanup $host-aut | st "'aut cleanup', v['residentGone']" ;;&
sched-end)
  $U watch $host-sched stop --label u14 | st "'sched watch', v['from'], v['to'], 'putgdNotify', v['putgdNotifyCalls'], [ (j.get('message'), (j.get('fields') or {}).get('latest'), (j.get('fields') or {}).get('scheduled')) for j in v['journal']]"
  $U frame $host-sched --label u14-sched-settings > /dev/null; $U quit $host-sched > /dev/null; $U cleanup $host-sched | st "'sched cleanup', v['residentGone']" ;;
esac
