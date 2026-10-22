#!/bin/sh
# x11_close.sh: politely close only put.gd's own UI windows (WM_CLASS putgd-ui) on :0.
export DISPLAY=:0 XAUTHORITY=/home/patrick/.Xauthority
for id in $(xdotool search --class '^putgd-ui$' 2>/dev/null); do
  xprop -id "$id" WM_CLASS _NET_WM_NAME 2>/dev/null | tr '\n' ' '; echo
  if xprop -id "$id" WM_CLASS | grep -q '"putgd-ui"' && xprop -id "$id" _NET_WM_NAME | grep -q '= "put.gd"'; then wmctrl -i -c "$id" && echo "closed $id"; fi
done
