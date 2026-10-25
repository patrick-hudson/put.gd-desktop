#!/bin/bash
# Phase 2 (private headless diagnostics, NOT acceptance): default host THP pair, then
# multi-item throughput/main-loop control with the private probe patch at both commits.
set -u
H=/home/patrick/work/i560o-h; S=/home/patrick/work/i560o; log=$H/evidence/phase2.log
until grep -q campaign-done $H/evidence/campaign.log; do sleep 10; done
run(){ name=$1 bin=$2; shift 2
 echo "$(date -u +%FT%TZ) start $name $bin $* (default host THP)" >> $log
 env "$@" $H/harness/run_o1.sh /tmp/p560$name $H/$bin/putgd > $H/evidence/$name.out 2>&1
 echo "$(date -u +%FT%TZ) end $name exit=$? $(tr -d '\n ' < /tmp/p560${name}b/wrapper-cleanup.json 2>/dev/null)" >> $log; }
run o3 bin-base I560_SETTLE=300
run o4 bin-cand I560_SETTLE=300
# Probe builds: identical private patch on each commit, own build tree copies.
cd $S
for rev in 80ffa2e6c0d34223d2cc0f31254f2e60ac2bd247 98878cfe31c2506d25ea6cbec97af002210319d2; do
 tag=${rev:0:8}
 git checkout -q $rev && git apply $H/harness/probe-control.patch && nice -n 10 cmake --build --preset dev -j3 --target operations_upload_probe putgd-s3-worker >> $log 2>&1
 mkdir -p $H/probe-$tag && cp build/dev/operations_upload_probe build/dev/putgd-s3-worker $H/probe-$tag/ && cp build/dev/libputgd-s3.so $H/probe-$tag/ 2>/dev/null
 sha256sum $H/probe-$tag/* > $H/probe-$tag/SHA256SUMS
 git checkout -q -- tests/performance/operations_upload_probe.cpp
done
git checkout -q fix-560-opus-2; echo "tree $(git rev-parse HEAD) clean=$(git status --porcelain | wc -l)" >> $log
media=/tmp/p560om; mkdir -m 700 -p $media
python3 - $media <<'PY'
import os,sys
root=sys.argv[1]
for name,size,count in (('m4',4,6),('m96',96,2),('m1',1,8)):
 for i in range(count):
  p=f'{root}/{name}-{i}.bin'
  if not os.path.exists(p):open(p,'wb').write(os.urandom(size*1024*1024))
PY
g1=$(ls $media/m4-*.bin | paste -sd,); g2=$(ls $media/m96-*.bin | paste -sd,); g3=$(ls $media/m1-*.bin | paste -sd,)
for round in 1 2 3; do for tag in 80ffa2e6 98878cfe; do
 d=/tmp/p560op-$tag-$round; mkdir -m 700 $d; cd $d
 python3 $S/tests/fixtures/s3_server.py > port 2> fixture.err & fx=$!
 for _ in $(seq 100); do [ -s port ] && break; sleep .1; done
 echo "$(date -u +%FT%TZ) probe $tag round $round" >> $log
 PATH=$H/probe-$tag:$PATH TMPDIR=$d nice -n 10 $H/probe-$tag/operations_upload_probe http://127.0.0.1:$(head -1 port) fixture 4 "$g1" "$g2" "$g3" > result.json 2> probe.err
 echo "$(date -u +%FT%TZ) probe $tag round $round exit=$? $(head -c 600 result.json)" >> $log
 kill $fx; wait $fx 2>/dev/null
done; done
echo "$(date -u +%FT%TZ) phase2-done" >> $log
