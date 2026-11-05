#!/bin/bash
# Revision identity_check_v3.sh = v2 plus ROUNDS validation (#579 Cubic 4372411646): ROUNDS must be a decimal
# integer from 1 to 200, so an empty, zero, negative or non-numeric value can no longer skip the loop and exit 0.
# identity_check_v2.sh was the revision of identity_check.sh (#579 Cubic 4372333045). v1 derived the identity from the worker itself, so a
# worker without the expected revision gave an empty id, and grep -qF '' matched any file. v2 requires a full
# expected revision, a present worker and a non-empty expected id before counting, and fails if the id is absent.
# Pipe form vs file form are still compared, under pipefail.
# usage: bash identity_check_v3.sh WORKER REV40 [ROUNDS 1-200]
set -uo pipefail
[[ $# -ge 2 && $# -le 3 && $2 =~ ^[0-9a-f]{40}$ ]] || { echo "usage: bash identity_check_v3.sh WORKER REV40 [ROUNDS 1-200]" >&2; exit 2; }
worker=$1 rev=$2 rounds=${3-20}; id=g${rev:0:12}
[[ $rounds =~ ^[1-9][0-9]{0,2}$ ]] && (( rounds <= 200 )) || { echo "ROUNDS must be an integer from 1 to 200: ${rounds@Q}" >&2; exit 2; }
[[ -f $worker ]] || { echo "worker missing: $worker" >&2; exit 2; }
[[ ${#id} -eq 13 ]] || { echo "empty identity" >&2; exit 2; }
tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
strings "$worker" > "$tmp" || { echo "strings failed" >&2; exit 2; }
grep -qF -- "$id" "$tmp" || { echo "identity $id absent from $worker"; exit 1; }
pipe=""; file=0
for _ in $(seq "$rounds"); do strings "$worker" | grep -qF -- "$id"; pipe+="$? "; grep -qF -- "$id" "$tmp" && file=$((file+1)); done
echo "identity $id present at line $(grep -nF -m1 -- "$id" "$tmp" | cut -d: -f1) of $(wc -l < "$tmp"); pipe statuses: $(tr ' ' '\n' <<< "$pipe" | grep . | sort | uniq -c | tr -s ' ' | tr '\n' ';'); file form $file/$rounds"
