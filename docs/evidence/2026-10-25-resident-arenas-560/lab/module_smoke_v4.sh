#!/bin/bash
# Relocated-bundle smoke v4 (v3 plus byte-preserved raw probe output; classification kept separately) for phase2_o4/o5 output (module_smoke.sh produced the retained real smoke): one small upload per revision through the relocated probe,
# worker and module against a fresh loopback fixture. While the PUT is held, record which libputgd-s3.so the
# probe's own putgd-s3-worker child has mapped.  usage: bash module_smoke_v4.sh SRC OUTROOT TAG...
set -euo pipefail
# v2 (#579 Greptile 4372270482): arguments checked and roots canonical before any mutation or cd.
[[ $# -ge 3 ]] || { echo "usage: bash module_smoke_v4.sh SRC OUTROOT TAG..." >&2; exit 2; }
src=$(realpath -e -- "$1") || { echo "bad SRC $1" >&2; exit 2; }
outroot=$(realpath -e -- "$2") || { echo "bad OUTROOT $2" >&2; exit 2; }
shift 2
[[ -f $src/tests/fixtures/s3_server.py ]] || { echo "SRC lacks tests/fixtures/s3_server.py" >&2; exit 2; }
for tag in "$@"; do
 [[ $tag =~ ^[0-9a-f]{8}$ && -x $outroot/rev-$tag/bin/operations_upload_probe && -e $outroot/rev-$tag/bin/putgd-s3-worker ]] || { echo "bad TAG or missing bundle: $tag" >&2; exit 2; }
done
failed=0
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
   # The path is the sixth field onward and may contain spaces; a poll before the module is mapped finds nothing,
   # which must not end the script under set -e/pipefail (v1 parsed with [^ ]* and aborted on an empty poll).
   mapped=$(awk '{ p=$0; for (i=1;i<=5;i++) sub(/^[^ ]+ +/, "", p); if (p ~ /\/libputgd-s3\.so$/) print p }' "/proc/$w/maps" | sort -u || true)
  done
  [[ -n $mapped ]] && break; sleep .1
 done
 # v3 (#579 Cubic 4372333048): under set -e a nonzero wait used to end the script before any status or failure record.
 if wait $probe; then status=0; else status=$?; fi
 kill $fx 2>/dev/null || true; wait $fx 2>/dev/null || true; trap - EXIT
 # v4 (#579 follow-up): result.json keeps the probe's stdout byte-for-byte. Missing, empty or malformed output is
 # classified in a separate result.classified.json that the checker reads instead; the raw bytes are never replaced.
 if python3 -c 'import json,sys; json.load(open(sys.argv[1]))' result.json 2>/dev/null; then verdict=result.json
 else printf '{"operations":[],"smokeError":"missing_or_invalid_result","rawResult":"result.json","rawBytes":%s}\n' "$(stat -c %s result.json 2>/dev/null || echo 0)" > result.classified.json; verdict=result.classified.json; fi
 if python3 - "$run/$verdict" "$status" "$mapped" "$dest" "$tag" <<'PY'
import json,sys,os
r=json.load(open(sys.argv[1]));files=[f for o in r.get('operations',[]) for f in o.get('files',[])]
mapped=[m for m in sys.argv[3].split('\n') if m];expected=os.path.realpath(os.path.join(sys.argv[4],'lib/x86_64-linux-gnu/putgd/libputgd-s3.so'))
ok=int(sys.argv[2])==0 and len(files)==1 and files[0]['status']=='uploaded' and [os.path.realpath(m) for m in mapped]==[expected]
print(json.dumps({'tag':sys.argv[5],'smokeError':r.get('smokeError'),'probeExit':int(sys.argv[2]),'uploaded':[f['status'] for f in files],'workerMappedModule':mapped,'expectedModule':expected,'pass':ok}))
sys.exit(0 if ok else 1)
PY
 then :; else failed=1; fi
done
exit $failed
