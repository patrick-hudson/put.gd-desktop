#!/bin/bash
# run.sh LABEL [bench_ocr_method args...]  -> ~/work/r1805/runs/$HOST/LABEL
# LABEL "helper" runs the real confined putgd-ocr through test_ocr_run instead.
cd ~/work/r1805
export OMP_THREAD_LIMIT=${OMP_THREAD_LIMIT:-1}  # as the helper sets it
H=$(cat ~/work/r1805/host)
[ -z "${PIN:-}" ] && [ -f ~/work/r1805/pin.env ] && . ~/work/r1805/pin.env
L=$1; shift
M=corpus/records/2026-11-13-issue-1428-corpus-index/manifest.json
O=runs/$H/$L
REF=""; [ "$L" != baseline ] && [ -f runs/$H/baseline/rows.json ] && REF="--reference runs/$H/baseline"
if [ "$L" = helper ]; then
  CMD="src/build/dev/test_ocr_run --helper $PWD/src/build/dev/putgd-ocr --text {image}"
else
  CMD="src/build/dev/bench_ocr_method --budget 16000 --memory 128 $* {image}"
fi
${PIN:+taskset -c $PIN} python3 -I tools/bench.py $M --label "$L" --out $O $REF ${BENCH_EXTRA:-} -- $CMD > $O.log 2>&1
echo RUN_DONE >> $O.log
