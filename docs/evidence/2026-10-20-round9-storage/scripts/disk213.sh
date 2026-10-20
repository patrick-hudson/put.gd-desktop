#!/bin/bash
# #213 before/after on a private Xvfb display with a 32 GiB loop filesystem as
# put.gd's data folder, filled to FREE bytes free (the lab guest had 2360406016).
#   disk213.sh up VARIANT FREE      start Xvfb, the resident and its UI
#   disk213.sh cli VARIANT METHOD [JSON]
#   disk213.sh shot VARIANT FILE    capture the whole private display
#   disk213.sh down VARIANT         stop everything this script started, unmount
set -u
cmd=$1; v=$2
case $v in before) tree=/home/patrick/work/put.gd-r9storage-base; disp=:93;; after) tree=/home/patrick/work/put.gd-r9storage; disp=:94;; esac
bin=$tree/build/dev
R=/tmp/r9s-$v
img=/home/patrick/work/put.gd-r9storage/build/disk-$v.img
envs() {
  export DISPLAY=$disp XDG_SESSION_TYPE=x11 QT_QPA_PLATFORM=xcb XDG_CURRENT_DESKTOP=X-Generic
  unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS SESSION_MANAGER
  export XDG_RUNTIME_DIR=$R/run XDG_STATE_HOME=$R/state XDG_CONFIG_HOME=$R/config XDG_CACHE_HOME=$R/cache XDG_DATA_HOME=$R/data
}
case $cmd in
up)
  free=$3
  mkdir -p $R/run $R/state $R/config $R/cache $R/data && chmod 700 $R $R/run $R/state $R/config $R/cache
  truncate -s 32G $img && mkfs.ext4 -q -F -m 0 $img
  sudo -n mount -o loop $img $R/data && sudo -n chown "$(id -u):$(id -g)" $R/data && chmod 700 $R/data
  avail=$(df -B1 --output=avail $R/data | tail -n1)
  fallocate -l $((avail - free)) $R/data/filler
  df -B1 $R/data | tail -n1 > $R/df.txt
  Xvfb $disp -screen 0 1600x1000x24 -nolisten tcp >/dev/null 2>&1 & echo $! > $R/xvfb.pid
  sleep 1
  envs
  setsid dbus-run-session -- $bin/putgd --background > $R/resident.log 2>&1 < /dev/null & echo $! > $R/resident.pid
  for i in $(seq 1 100); do [ -S $R/run/putgd/session.sock ] && break; sleep 0.2; done
  cat $R/df.txt
  ;;
cli)
  envs; shift 2; $bin/putgd-cli "$@" ;;
shot)
  envs; import -window root "$3" ;;
down)
  envs
  [ -f $R/resident.pid ] && kill -- -$(cat $R/resident.pid) 2>/dev/null; sleep 2
  [ -f $R/xvfb.pid ] && kill $(cat $R/xvfb.pid) 2>/dev/null; sleep 1
  sudo -n umount $R/data; rm -f $img; rm -rf $R
  ;;
esac
# resident-only restart helpers for the #202 log run
case $cmd in
stopres) [ -f $R/resident.pid ] && kill -- -$(cat $R/resident.pid) 2>/dev/null; sleep 2 ;;
startres)
  envs
  setsid dbus-run-session -- $bin/putgd --background >> $R/resident.log 2>&1 < /dev/null & echo $! > $R/resident.pid
  for i in $(seq 1 100); do [ -S $R/run/putgd/session.sock ] && break; sleep 0.2; done ;;
esac
