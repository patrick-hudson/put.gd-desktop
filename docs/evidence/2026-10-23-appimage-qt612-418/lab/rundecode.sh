#!/bin/bash
# one process per file and Qt version; 60 s limit; real exit status or signal; stderr kept per run
cd "$(dirname "$0")"; mkdir -p "${1:-decode}"; out=${1:-decode}
for f in samples/*; do for v in 6.8.3 6.12.0; do
  run=$(mktemp -d /tmp/qt418-dec-XXXX)
  env -i HOME=$HOME PATH=/usr/bin:/bin QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR=$run timeout 60 ./decode-$v "$f" > "$out/$(basename $f).$v.out" 2> "$out/$(basename $f).$v.err"; rc=$?
  rm -rf "$run"
  if [ $rc = 124 ]; then st=TIMEOUT; elif [ $rc -ge 128 ]; then st="SIGNAL-$((rc-128))"; else st="exit-$rc"; fi
  printf "%-42s %-7s %-10s %s | %s\n" "$(basename $f)" "$v" "$st" "$(tail -1 "$out/$(basename $f).$v.out")" "$(grep -v "^$" "$out/$(basename $f).$v.err" | tail -1 | cut -c1-110)"
done; done
