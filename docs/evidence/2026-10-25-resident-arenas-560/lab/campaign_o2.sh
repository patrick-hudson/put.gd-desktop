#!/bin/bash
# Revision of campaign_o.sh (#579 review 4372122101): the run status is captured before logging.
# Corrected queued-first cancellation control (o2 harness), matched base/candidate.
# usage: campaign_o2.sh SRC HARNESS BINROOT EVIDENCE
set -uo pipefail
src=$1 harness=$2 bins=$3 evidence=$4; log=$evidence/campaign2.log
run(){ name=$1 bin=$2; shift 2
 echo "$(date -u +%FT%TZ) start $name $bin $*" >> "$log"
 env "$@" I560_THP_OFF=1 "$harness/run_o2.sh" "$src" "$harness" "/tmp/p560$name" "$bins/$bin/putgd" > "$evidence/$name.out" 2>&1
 local status=$?
 echo "$(date -u +%FT%TZ) end $name exit=$status $(tr -d '\n ' < "/tmp/p560${name}b/wrapper-cleanup.json" 2>/dev/null)" >> "$log"
 return $status; }
failed=0
run qa bin-base I560_MODE=cancel2 || failed=1
run qb bin-cand I560_MODE=cancel2 || failed=1
echo "$(date -u +%FT%TZ) campaign2-done failed=$failed" >> "$log"
exit $failed
