#!/bin/bash
# scenarios.sh (#344 lab acceptance): the channel switch on one desktop, driven
# through putgd-cli (the MCP path) by upd344.py, with full-desktop frames from
# the host's loopback wall VNC. Run from ~/work/put.gd-i344acc on the host.
#   scenarios.sh nostable VNC   nightly 9101 watches Stable while lab stable.json has no release
#   scenarios.sh n2s VNC        ... then the lab stable v0.1.1 exists: switch (zsync), restart
#   scenarios.sh s2n VNC        stable 0.1.1 watches Nightly 9102 (older): refused without allowOlder, then switch (download), restart
#   scenarios.sh tamper VNC     nightly 9101 switching to a broken copy of lab stable v0.1.1 (loopback)
set -u
U="python3 upd344.py"
host=$(hostname -s)
case "$1" in
nostable)
  n=$host-n2s
  $U setup $n --appimage images/putgd-0.1.1-nightly.9101-x86_64.AppImage --tool tool/appimageupdatetool-x86_64.AppImage --vnc-port $2 > /dev/null || exit 1
  $U start $n --label 1-nightly-start
  $U channel $n stable --label 2-no-stable-yet
  ;;
n2s)
  n=$host-n2s
  $U check $n --label 3-stable-offer
  # Confirm alone is refused for a switch: it must name the channel.
  $U install $n --label 4-switch-n2s-unnamed
  $U install $n --channel stable --label 5-switch-n2s
  $U restart $n --label 6-n2s-restarted
  $U quit $n
  ;;
s2n)
  n=$host-s2n
  $U setup $n --appimage images/putgd-0.1.1-x86_64.AppImage --vnc-port $2 > /dev/null || exit 1
  $U start $n --label 1-stable-start
  $U channel $n nightly --label 2-older-offer
  $U install $n --channel nightly --label 3-older-refused
  $U install $n --channel nightly --allow-older --label 4-switch-s2n
  $U restart $n --label 5-s2n-restarted
  $U quit $n
  ;;
tamper)
  n=$host-tamper
  $U setup $n --appimage images/putgd-0.1.1-nightly.9101-x86_64.AppImage --channel stable --vnc-port $2 > /dev/null || exit 1
  $U tamper $n --tag v0.1.1 --signatures signatures
  ;;
*) echo "usage: $0 nostable|n2s|s2n|tamper VNC_PORT" >&2; exit 2 ;;
esac
