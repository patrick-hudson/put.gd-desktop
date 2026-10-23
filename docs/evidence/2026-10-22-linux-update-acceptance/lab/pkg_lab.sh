#!/bin/bash
# pkg_lab.sh HOST LABEL VNC INPUT a|b|c: U6's populated-stable leg on the lab channels (pkg.put.gd/lab), one desktop.
# The commands are the ones Settings -> Other channel shows (update.status action.otherChannel.commands), run as given;
# only `sudo` gets -n and apt/dnf get -y (apt-get for apt), for a non-interactive shell.
#   a  (L1 + S1 published): the live put.gd is removed; lab nightly L1 from its file; Settings; the first commands only
#      (the wait path: stable S1 is older); then the last one (downgrade to S1); Settings; notification watch on.
#   b  (S2 published): Check now shows S2 from the stable repository; the system's own upgrade takes S1 -> S2
#      (apt-get install --only-upgrade putgd / dnf upgrade putgd); Settings after a restart.
#   c  (L2 + S3 published): Check now shows S3, watch off (a repository install never notifies); the "to nightly"
#      commands move to L2; Settings; then put.gd is removed with its repository file and the run cleaned up.
set -u
host=$1 label=$2 vnc=$3 input=$4 step=$5
O=/home/patrick/work/put.gd-ua/lab/out
cd /home/patrick/work/put.gd-ua/tool
U="python3 -I ua_pkg.py"
R=/home/patrick/work/put.gd-ua/results/$host-u6lab-$step.log
ext=deb; [ "$label" = fedora44 ] && ext=rpm
status() { $U check $host-lab --label "$1" | python3 -c "import json,sys; v=list(json.load(sys.stdin).values())[0]; s=v['status']; print('$1', s['build'], s['channel'], s['channelSource'], 'latest', s['latest'], 'available', s['updateAvailable'], 'older', s['action'].get('older'), 'repo', v['repo'].get('sameAs'), v['repo'].get('installed'))"; }
commands() {  # the Other channel commands from update.status, adapted for a non-interactive shell (one per line)
  $U check $host-lab --label cmds > /tmp/ua-cmds.$$ 2>/dev/null
  python3 -c "
import json, re
v = list(json.load(open('/tmp/ua-cmds.$$')).values())[0]
for c in v['status']['action']['otherChannel']['commands']:
    c = c.replace('sudo ', 'sudo -n ').replace('apt install', 'apt-get install -y').replace('apt update', 'apt-get update').replace('dnf distro-sync', 'dnf -y distro-sync')
    print(c)"
  rm -f /tmp/ua-cmds.$$
}
{
case $step in
a)
  echo "### remove the live put.gd"
  if [ $ext = deb ]; then sudo -n apt-get purge -y putgd 2>&1 | grep -E '^(Removing|Purging)'; else sudo -n dnf remove -y putgd 2>&1 | grep -E 'Removing|Complete'; fi
  echo "### install lab nightly L1 from its file"
  f=$(ls $O/v26.1019.1019-nightly/putgd-26.1019.1019-nightly-$label-x86_64.$ext)
  if [ $ext = deb ]; then sudo -n apt-get install -y "$f" 2>&1 | tail -1; else sudo -n dnf install -y "$f" 2>&1 | tail -1; fi
  $U start $host-lab --vnc-port $vnc --label u6lab-1-L1 --input $input --profile private --feed lab --notifications > /dev/null
  status u6lab-1-L1
  mapfile -t cmds < <(commands)
  printf 'copied: %s\n' "${cmds[@]}"
  n=${#cmds[@]}
  for ((i = 0; i < n - 1; i++)); do echo "### ${cmds[$i]}"; bash -c "${cmds[$i]}" 2>&1 | tail -2; done
  status u6lab-2-stable-older-wait
  echo "### ${cmds[$((n - 1))]}"; bash -c "${cmds[$((n - 1))]}" 2>&1 | tail -3
  $U restart $host-lab --label u6lab-3-S1 > /dev/null
  status u6lab-3-S1
  $U watch $host-lab start --label u14repo > /dev/null && echo "watch on" ;;
b)
  status u6lab-4-S2-available
  echo "### the system's upgrade"
  if [ $ext = deb ]; then sudo -n apt-get update > /dev/null 2>&1; sudo -n apt-get install -y --only-upgrade putgd 2>&1 | grep -E 'putgd|upgraded|onffile'; else sudo -n dnf upgrade -y --refresh putgd 2>&1 | grep -E 'putgd|Complete'; fi
  $U restart $host-lab --label u6lab-5-S2 > /dev/null
  status u6lab-5-S2 ;;
c)
  status u6lab-6-S3-available
  $U watch $host-lab stop --label u14repo | python3 -c "import json,sys; v=json.load(sys.stdin)['watch']; print('u14 repo stable', v['from'], v['to'], 'putgd notifications', v['putgdNotifyCalls'])"
  mapfile -t cmds < <(commands)
  printf 'copied: %s\n' "${cmds[@]}"
  for c in "${cmds[@]}"; do echo "### $c"; bash -c "$c" 2>&1 | tail -2; done
  $U restart $host-lab --label u6lab-7-back-on-nightly > /dev/null
  status u6lab-7-back-on-nightly
  $U cleanup $host-lab > /dev/null && echo "run cleaned"
  echo "### remove put.gd and its repository file"
  if [ $ext = deb ]; then sudo -n apt-get purge -y putgd 2>&1 | grep -E '^(Removing|Purging)'; sudo -n apt-get update > /dev/null 2>&1; else sudo -n dnf remove -y putgd 2>&1 | grep -E 'Complete'; fi
  ls /etc/apt/sources.list.d/putgd.sources /etc/yum.repos.d/putgd.repo 2>&1 | grep -v 'No such' ;;
esac
} 2>&1 | tee $R
