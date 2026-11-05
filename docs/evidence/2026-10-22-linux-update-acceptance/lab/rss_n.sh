#!/bin/bash
# rss_n.sh HOST LABEL [VERSION...]: U15 idle RSS on one quiet desktop, N (1017) and N+1 (1019), AppImage and package.
# Each window: the resident started --background in its own private profile, 30 s settle, RSS every second for 180 s
# (A: cold idle; B: idle after Check now), Pss and the mapped TLS/network libraries (ua_app.py `mem`, #343's method).
set -u
host=$1 label=$2; shift 2
versions=${*:-26.1022.1017-nightly 26.1022.1019-nightly}
A=/home/patrick/work/put.gd-ua/assets
cd /home/patrick/work/put.gd-ua/tool
ext=deb; [ "$label" = fedora44 ] && ext=rpm
sum() { python3 -c "import json,sys; v=list(json.load(sys.stdin).values())[0]; a,b=v['A'],v['B']; print('$1', v['version'], 'A median', a['rssMedianMiB'], 'pss', a['pssMiB'], 'B median', b['rssMedianMiB'], 'pss', b['pssMiB'], 'tls', a['tlsNetworkLibraries'] + b['tlsNetworkLibraries'], 'load', a['load'])"; }
echo "start $(date -u +%TZ) load $(cut -d' ' -f1-3 /proc/loadavg)"
for v in $versions; do
  n=$host-mem-app-${v%%-*}
  [ -f $A/v$v/putgd-$v-x86_64.AppImage ] || { echo "missing AppImage $v"; continue; }
  python3 -I ua_app.py setup $n --appimage $A/v$v/putgd-$v-x86_64.AppImage --live --vnc-port 0 --allow-data-home > /dev/null && \
  python3 -I ua_app.py mem $n --action check | sum "appimage"; python3 -I ua_app.py cleanup $n > /dev/null
done
for v in $versions; do
  f=$A/v$v/putgd-$v-$label-x86_64.$ext
  [ -f "$f" ] || { echo "missing $f"; continue; }
  if [ $ext = deb ]; then sudo -n apt-get install -y --allow-downgrades "$f" > /dev/null 2>&1; else sudo -n dnf install -y "$f" > /dev/null 2>&1 || sudo -n dnf upgrade -y "$f" > /dev/null 2>&1; fi
  n=$host-mem-pkg-${v%%-*}
  python3 -I ua_pkg.py start $n --vnc-port 0 --label start --profile private --feed live > /dev/null 2>&1
  python3 -I ua_pkg.py quit $n > /dev/null
  python3 -I ua_pkg.py mem $n | sum "package"; python3 -I ua_pkg.py cleanup $n > /dev/null
done
if [ $ext = deb ]; then sudo -n apt-get purge -y putgd > /dev/null 2>&1; else sudo -n dnf remove -y putgd > /dev/null 2>&1; fi
echo "end $(date -u +%TZ) load $(cut -d' ' -f1-3 /proc/loadavg); putgd $( (dpkg-query -W putgd || rpm -q putgd) 2>&1 | tail -1)"
