#!/bin/bash
# ak2a.sh NAME TX TY : B's tray picture from put.gd's own tray icon (TX,TY, checked on a frame), then #341 with that menu open.
set -u
n=$1 tx=$2 ty=$3; root=$HOME/work/put.gd-qt418/afa; cd "$root" || exit 2; export AFA_ROOT=$root
d=runs/$n; t() { date -u +%FT%TZ; }
echo "$tx $ty" > $d/systray-xy.txt
mkdir -p $d/superseded-finder; for f in $d/tray-menu-click.json $d/full/$n-tray-menu.png $d/crops/$n-tray-menu.png; do [ -e "$f" ] && mv "$f" $d/superseded-finder/; done
echo "$(t) B tray picture from put.gd's tray icon ($tx,$ty)" >> timeline-$n.txt
python3 -I tool/afa.py tray-click $n --label $n-tray-menu $tx $ty > $d/tray-menu-click.json
python3 -I tool/afa.py click $n --key Escape > /dev/null 2>&1; sleep 1
R="python3 -I tool/rerun.py menu-shortcut $n"
echo "$(t) #341 start" >> timeline-$n.txt
$R --action desktop --open click:$tx,$ty --label $n-341-desktop-traymenu > $d/341-desktop-tray.json; sleep 2
if [ $n = m74 ]; then
  $R --action region --open click:$tx,$ty --close-after 1 --drag 1000,500,1080,560 --label $n-341-region-traymenu > $d/341-region-tray.json; sleep 2
  $R --action region --open click:$tx,$ty --notice-wait 4 --close-after 0 --drag 1000,500,1080,560 --timeout 8 --label $n-341-region-notice > $d/341-region-notice.json; sleep 2
  (zenity --info --title "put.gd 341 external window" --text "External window for the #341 window-capture check" --width 500 --height 200 >/dev/null 2>&1 & echo $! > $d/zenity.pid); sleep 2
  wid=$(xdotool search --name "put.gd 341 external window" | head -1); xdotool windowactivate --sync $wid; sleep 1
  $R --action window --open click:$tx,$ty --label $n-341-window-traymenu > $d/341-window-tray.json; kill $(cat $d/zenity.pid); rm -f $d/zenity.pid
else
  $R --action region --open click:$tx,$ty --drag 1000,500,1080,560 --label $n-341-region-traymenu > $d/341-region-tray.json
fi
echo "$(t) #341 done" >> timeline-$n.txt
for f in $d/341-*.json; do python3 -c "
import json,sys; r=json.load(open(sys.argv[1])); print(sys.argv[1].split('/')[-1], json.dumps({k:r.get(k) for k in ('shortcut','seconds','newHistory','png','menuInCapture','expectedSize','menuBox','noticeLog')})[:400])" $f; done
python3 -c "import json;d=json.load(open('$d/tray-menu-click.json'));print('tray picture', d.get('kept'), d.get('inner'), d.get('full'))"
