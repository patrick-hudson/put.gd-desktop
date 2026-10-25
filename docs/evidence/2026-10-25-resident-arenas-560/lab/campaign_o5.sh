#!/bin/bash
# Revision of campaign_o4.sh (#579 Greptile 4372270477): the runner is invoked through bash, so a checkout or
# git-archive export with mode 100644 still runs it. Otherwise as campaign_o4.sh: status captured before logging.
# usage: bash campaign_o5.sh SRC HARNESS BINROOT EVIDENCE
set -uo pipefail
[[ $# -eq 4 ]] || { echo "usage: bash campaign_o5.sh SRC HARNESS BINROOT EVIDENCE" >&2; exit 2; }
src=$1 harness=$2 bins=$3 evidence=$4; log=$evidence/campaign5.log
run(){ name=$1 bin=$2; shift 2
 echo "$(date -u +%FT%TZ) start $name $bin $*" >> "$log"
 env "$@" I560_THP_OFF=1 bash "$harness/run_o4.sh" "$src" "$harness" "/tmp/p560$name" "$bins/$bin/putgd" > "$evidence/$name.out" 2>&1
 local status=$?
 echo "$(date -u +%FT%TZ) end $name exit=$status $(tr -d '\n ' < "/tmp/p560${name}b/wrapper-cleanup.json" 2>/dev/null)" >> "$log"
 return $status; }
failed=0
run ua bin-base I560_MODE=cancel3 || failed=1
run ub bin-cand I560_MODE=cancel3 || failed=1
echo "$(date -u +%FT%TZ) campaign5-done failed=$failed" >> "$log"
exit $failed
