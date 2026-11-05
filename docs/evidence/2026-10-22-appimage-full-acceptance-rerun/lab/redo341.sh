#!/bin/bash
# redo341.sh NAME : restart the private resident, then B's tray picture and #341 with put.gd's own tray icon (runs/NAME/systray-xy.txt).
set -u
n=$1; root=$HOME/work/put.gd-afa2; cd "$root" || exit 2; export AFA_ROOT=$root
d=runs/$n; mkdir -p $d/superseded-taskmanager-menu
for f in $d/341-*.json $d/full/$n-341-* $d/full/$n-tray-menu* $d/crops/$n-tray-menu* $d/tray-menu-click.json; do [ -e "$f" ] && mv "$f" $d/superseded-taskmanager-menu/; done
read tx ty < $d/systray-xy.txt
python3 -I tool/afa.py start $n > $d/start2.json || exit 4
sleep 3
python3 -I tool/afa.py tray-click $n --label $n-tray-menu $tx $ty > $d/tray-menu-click.json
R="python3 -I tool/rerun.py menu-shortcut $n"
$R --action desktop --open click:$tx,$ty --label $n-341-desktop-traymenu > $d/341-desktop-tray.json; sleep 2
if [ $n = m74 ]; then
  $R --action region --open click:$tx,$ty --close-after 1 --drag 1000,500,1080,560 --label $n-341-region-traymenu > $d/341-region-tray.json; sleep 2
  $R --action region --open click:$tx,$ty --notice-wait 4 --close-after 0 --drag 1000,500,1080,560 --timeout 8 --label $n-341-region-notice > $d/341-region-notice.json; sleep 2
  # an external window in focus, the tray menu open, then the window shortcut
  (zenity --info --title "put.gd 341 external window" --text "External window for the #341 window-capture check" --width 500 --height 200 >/dev/null 2>&1 & echo $! > $d/zenity.pid); sleep 2
  wid=$(xdotool search --name "put.gd 341 external window" | head -1); xdotool windowactivate --sync $wid; sleep 1
  $R --action window --open click:$tx,$ty --label $n-341-window-traymenu > $d/341-window-tray.json; kill $(cat $d/zenity.pid); rm -f $d/zenity.pid
else
  $R --action region --open click:$tx,$ty --drag 1000,500,1080,560 --label $n-341-region-traymenu > $d/341-region-tray.json
fi
for f in $d/341-*.json; do python3 -c "
import json,sys; r=json.load(open(sys.argv[1])); print(sys.argv[1].split('/')[-1], json.dumps({k:r.get(k) for k in ('shortcut','seconds','newHistory','png','menuInCapture','expectedSize','menuBox','noticeLog')})[:500])" $f; done
python3 -c "import json;d=json.load(open('$d/tray-menu-click.json'));print('tray picture', d.get('kept'), d.get('inner'), d.get('full'))"
