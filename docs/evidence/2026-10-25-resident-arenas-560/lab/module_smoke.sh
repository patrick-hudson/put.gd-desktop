#!/bin/bash
# Relocated-bundle smoke for phase2_o4 output: one small upload per revision through the relocated probe,
# worker and module against a fresh loopback fixture. While the PUT is held, record which libputgd-s3.so the
# probe's own putgd-s3-worker child has mapped.  usage: module_smoke.sh SRC OUTROOT TAG...
set -euo pipefail
src=$1 outroot=$2; shift 2
for tag in "$@"; do
 dest=$outroot/rev-$tag; run=$outroot/smoke-$tag; [[ ! -e $run ]] || { echo "smoke dir exists" >&2; exit 2; }
 mkdir -m 700 "$run"; cd "$run"
 head -c 65536 /dev/urandom > payload.bin
 python3 "$src/tests/fixtures/s3_server.py" > port 2> fixture.err & fx=$!
 trap 'kill $fx 2>/dev/null || true' EXIT
 for _ in $(seq 100); do [[ -s port ]] && break; sleep .1; done
 endpoint=http://127.0.0.1:$(head -1 port)
 python3 - "$endpoint" <<'PY'
import json,sys,urllib.request
req=urllib.request.Request(sys.argv[1]+'/__rules',data=json.dumps({'rules':[{'method':'PUT','delay':4,'remaining':5}]}).encode(),headers={'Content-Type':'application/json'})
urllib.request.urlopen(req,timeout=10).read()
PY
 TMPDIR=$run "$dest/bin/operations_upload_probe" "$endpoint" fixture 1 "$run/payload.bin" > result.json 2> probe.err & probe=$!
 mapped=""
 for _ in $(seq 80); do
  for w in $(pgrep -P $probe -x putgd-s3-worker || true); do
   [[ $(readlink /proc/$w/exe) == "$dest/bin/putgd-s3-worker" ]] || continue
   mapped=$(grep -o '/[^ ]*libputgd-s3\.so' /proc/$w/maps | sort -u | tr '\n' ' ')
  done
  [[ -n $mapped ]] && break; sleep .1
 done
 wait $probe; status=$?
 kill $fx; wait $fx 2>/dev/null || true; trap - EXIT
 python3 - "$run/result.json" "$status" "$mapped" "$dest" "$tag" <<'PY'
import json,sys,os
r=json.load(open(sys.argv[1]));files=[f for o in r['operations'] for f in o['files']]
mapped=sys.argv[3].split();expected=os.path.realpath(os.path.join(sys.argv[4],'lib/x86_64-linux-gnu/putgd/libputgd-s3.so'))
ok=int(sys.argv[2])==0 and len(files)==1 and files[0]['status']=='uploaded' and [os.path.realpath(m) for m in mapped]==[expected]
print(json.dumps({'tag':sys.argv[5],'probeExit':int(sys.argv[2]),'uploaded':[f['status'] for f in files],'workerMappedModule':mapped,'expectedModule':expected,'pass':ok}))
sys.exit(0 if ok else 1)
PY
done
