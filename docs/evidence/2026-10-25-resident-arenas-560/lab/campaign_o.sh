#!/bin/bash
# Sequential private headless campaign; NOT physical acceptance.
set -u
H=/home/patrick/work/i560o-h
log=$H/evidence/campaign.log
run(){ name=$1 bin=$2; shift 2
 echo "$(date -u +%FT%TZ) start $name $bin $*" >> $log
 env "$@" I560_THP_OFF=1 $H/harness/run_o1.sh /tmp/p560$name $H/$bin/putgd > $H/evidence/$name.out 2>&1
 echo "$(date -u +%FT%TZ) end $name exit=$? $(cat /tmp/p560${name}b/wrapper-cleanup.json 2>/dev/null | tr -d '\n ')" >> $log; }
run o0 bin-base I560_SETTLE=300
run o1 bin-cand I560_SETTLE=300
run o2 bin-base I560_SETTLE=300 MALLOC_ARENA_MAX=1
run oa bin-base I560_MODE=cancel
run ob bin-cand I560_MODE=cancel
echo "$(date -u +%FT%TZ) campaign-done" >> $log
