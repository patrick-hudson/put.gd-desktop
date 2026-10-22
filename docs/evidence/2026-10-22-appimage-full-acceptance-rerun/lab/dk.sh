#!/bin/bash
# dk.sh NAME DRAG RECRECT SMALLDRAG : items B (tray, window), D, E, F, G, H on a started run; one JSON per step in runs/NAME/.
#   DRAG      region capture drag x0,y0,x1,y1 (1120x680 on 1920x1080)
#   RECRECT   recording rect x,y,w,h
#   SMALLDRAG 80x60 drag x0,y0,x1,y1
set -u
name=$1 drag=$2 recrect=$3 small=$4
root=$HOME/work/put.gd-afa2; cd "$root" || exit 2
export AFA_ROOT=$root
afa() { python3 -I tool/afa.py "$@"; }
rr() { python3 -I tool/rerun.py "$@"; }
i326() { python3 -I tool/i326.py "$@"; }
d=runs/$name
log() { printf '%s %s\n' "$(date -u +%FT%TZ)" "$*" >> "$d/steps.log"; }
step() { local out=$1; shift; "$@" > "$d/$out.json" 2> "$d/$out.err"; local rc=$?; log "$out rc=$rc"; return $rc; }
# B: tray and window
step find-tray afa find-tray "$name"
read tx ty < <(python3 -c "import json;f=json.load(open('$d/find-tray.json'))['found'];k='top' if 'top' in f else 'bottom';print(*f[k]['centre'])" 2>/dev/null) || true
# TRAY_XY="x y" overrides the finder (it can match a task-manager entry with the same icon)
[ -n "${TRAY_XY:-}" ] && read tx ty <<< "$TRAY_XY"
echo "$tx $ty" > "$d/tray-xy.txt"
[ -n "${tx:-}" ] && step tray-menu-click afa tray-click "$name" --label "$name-tray-menu" "$tx" "$ty"
step tray-menu-dbus afa tray-menu "$name"
step window afa window "$name" --label "$name-settings"
afa click "$name" --key Escape > /dev/null 2>&1
# D: captures under the moving cover
step cover-start afa cover "$name" start --pattern --precheck
step capture-region afa capture "$name" region --drag "$drag"
cx=$(python3 -c "d='$drag'.split(',');print((int(d[0])+int(d[2]))//2)"); cy=$(python3 -c "d='$drag'.split(',');print((int(d[1])+int(d[3]))//2)")
step capture-window afa capture "$name" window --focus "$cx,$cy"
step capture-desktop afa capture "$name" desktop
step region-size-small rr region-size "$name" --drag "$small"
step edit afa edit "$name" --dialog
# E: recording, then the static control (cover without the pattern)
step record afa record "$name" --rect "$recrect"
step cover-stop afa cover "$name" stop
step cover-still afa cover "$name" start
step control afa control "$name" --rect "$recrect"
step cover-stop2 afa cover "$name" stop
# D #333 origin: the coordinate pattern (no cover)
step region-size-coords-large rr region-size "$name" --drag "$drag" --coords
step region-size-coords-small rr region-size "$name" --drag "$small" --coords
# F: the real default handler (portal reads ~/.config/mimeapps.list), the banner click, then restore
step handler-on i326 handler-on "$name"
step notify afa notify "$name" --label "$name-notify"
sleep 2; step recorder-full i326 full "$name" --label "$name-link-opened" --wait 1
step handler-off i326 handler-off "$name"
# G
step history afa history "$name"
step delete afa delete "$name"
# H: theme, update status, Check now; #349; #344 stable and back
step settings afa settings "$name"
step status-clean-settings rr status-clean "$name" --label settings
step channel-stable rr channel "$name" --to stable --label "$name-344"
step channel-nightly rr channel "$name" --to nightly --label "$name-344"
log dk-done
