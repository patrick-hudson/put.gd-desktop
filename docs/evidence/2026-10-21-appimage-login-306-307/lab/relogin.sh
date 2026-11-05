#!/bin/bash
# relogin.sh LABEL TAG: quit lab-started residents, real logout, LightDM login over VNC,
# then the on-screen summary terminal and a frame (frames/TAG.png) plus results/mint/TAG.txt.
set -u
LABEL=$1 TAG=$2
S=/tmp/claude-1000/-home-patrick-work-put-gd--claude-worktrees-recursing-feynman-beef82/ecc37013-3db7-4fae-b8f1-4d73bfbe88a0/scratchpad
E=/home/patrick/work/put.gd/.claude/worktrees/recursing-feynman-beef82/docs/evidence/2026-10-21-appimage-login-306-307
$S/gssh 'cd ~/r12 && python3 r12.py quit; pgrep -u lab -x putgd && echo "STILL RUNNING" ; . ~/r12/genv.sh; setsid cinnamon-session-quit --logout --no-prompt >/dev/null 2>&1 < /dev/null & sleep 1'
sleep 9
python3 $S/vnc.py shot:greeter-$TAG >/dev/null
python3 $S/guest_login.py >/dev/null
sleep 30
$S/gssh "cd ~/r12 && python3 r12.py summary '$LABEL' | tee ~/r12/$TAG.txt; python3 r12.py state '$LABEL' > ~/r12/$TAG.json; cat /proc/\$(pgrep -u lab -x putgd | head -1)/cgroup 2>/dev/null; ps -o pid,args -p \$(ps -o ppid= -p \$(pgrep -u lab -x putgd | head -1)) 2>/dev/null | tail -1" | tee $E/results/mint/$TAG.txt
$S/gssh "cat ~/r12/$TAG.json" > $E/results/mint/$TAG.json
$S/gssh ". ~/r12/genv.sh; setsid gnome-terminal --geometry=118x16+170+120 --title='r12 $TAG' -- bash -c \"python3 ~/r12/r12.py summary '$LABEL'; exec bash\" >/dev/null 2>&1 < /dev/null & sleep 3"
python3 $S/vnc.py shot:$TAG >/dev/null; cp $S/$TAG.png $S/frames/$TAG.png; echo "frame frames/$TAG.png"
