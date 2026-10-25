#!/bin/bash
# Real-binary check of the worker identity test: old pipe form vs file form, under pipefail.
set -uo pipefail
for d in "$@"; do
 rev=$(basename "$d" | sed 's/probe-//')
 id=$(strings "$d/putgd-s3-worker" | grep -oE "g${rev}[0-9a-f]{4}" | head -1)
 for i in $(seq 20); do strings "$d/putgd-s3-worker" | grep -q "$id"; echo -n "$? "; done | tr ' ' '\n' | sort | uniq -c | tr '\n' ' '; echo " <- pipe form statuses ($d, $id)"
 tmp=$(mktemp); strings "$d/putgd-s3-worker" > "$tmp"; ok=0; for i in $(seq 20); do grep -qF "$id" "$tmp" && ok=$((ok+1)); done; rm "$tmp"; echo "file form: $ok/20 pass ($d)"
 echo -n "position of identity in strings output: line "; strings "$d/putgd-s3-worker" | grep -n -m1 "$id" | cut -d: -f1; echo -n "total lines: "; strings "$d/putgd-s3-worker" | wc -l
done
