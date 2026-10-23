#!/bin/bash
# nk.sh NAME DRAG RECRECT : items B (tray, window), D, E, G and H on a started private run (after `nfa.py start` and
# `memory`); one JSON per step in runs/NAME/. From the AppImage pass's dk.sh.
#   DRAG     region capture drag x0,y0,x1,y1, inside the cover's magenta (not over the moving pattern)
#   RECRECT  recording rect x,y,w,h over the pattern
# F (live S3, real browser) and K (quit from the tray) are separate steps; see README.md.
set -u
name=$1 drag=$2 recrect=$3
root=${NFA_ROOT:-$HOME/work/put.gd-nfa}; cd "$root" || exit 2
export AFA_ROOT=$root
N() { python3 -I tool/nfa.py "$@"; }
d=runs/$name
log() { printf '%s %s\n' "$(date -u +%FT%TZ)" "$*" >> "$d/steps.log"; }
step() { local out=$1; shift; "$@" > "$d/$out.json" 2> "$d/$out.err"; local rc=$?; log "$out rc=$rc"; return $rc; }
# B: tray and window
step find-tray N find-tray "$name"
read tx ty < <(python3 -c "import json;f=json.load(open('$d/find-tray.json'))['found'];k='top' if 'top' in f else 'bottom';print(*f[k]['centre'])" 2>/dev/null) || true
[ -n "${TRAY_XY:-}" ] && read tx ty <<< "$TRAY_XY"
echo "${tx:-} ${ty:-}" > "$d/tray-xy.txt"
[ -n "${tx:-}" ] && step tray-menu-click N tray-click "$name" --label "$name-tray-menu" "$tx" "$ty"
step tray-menu-dbus N tray-menu "$name"
step window N window "$name" --label "$name-settings"
N key "$name" Escape > /dev/null 2>&1
# D: captures under the moving cover
step cover-start N cover "$name" start --pattern --precheck
step capture-region N capture "$name" region --drag "$drag"
cx=$(python3 -c "d='$drag'.split(',');print((int(d[0])+int(d[2]))//2)"); cy=$(python3 -c "d='$drag'.split(',');print((int(d[1])+int(d[3]))//2)")
step capture-window N capture "$name" window --focus "$cx,$cy"
step capture-desktop N capture "$name" desktop
step edit N edit "$name" --dialog
# E: recording, then the static control (cover without the pattern)
step record N record "$name" --rect "$recrect"
step cover-stop N cover "$name" stop
step cover-still N cover "$name" start
step control N control "$name" --rect "$recrect"
step cover-stop2 N cover "$name" stop
# G
step history N history "$name"
step delete N delete "$name"
# H: theme switch, then Settings -> Updates on the repository install and Open software updates
step settings N settings "$name"
step updates N updates "$name" --label "$name-updates" --open
log nk-done
