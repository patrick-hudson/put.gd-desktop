#!/bin/bash
# Stub test for module_smoke_v4.sh from a clean export: the probe's raw stdout survives byte-for-byte when it is
# malformed, the failure is classified separately with probeExit kept, the script returns nonzero, and the stub
# fixture is stopped. Everything lives inside this test's own mktemp directory.  usage: bash test_module_smoke_v4.sh LABDIR
set -uo pipefail
lab=$(realpath -e -- "$1"); T=$(mktemp -d "/tmp/i560 smoke v4 test.XXXX"); fail=0; n=0
check(){ n=$((n+1)); if [[ $2 == "$3" ]]; then echo "PASS $1 ($3)"; else echo "FAIL $1: got $2 want $3"; fail=1; fi; }
cd "$T"; mkdir -p "src dir/tests/fixtures"
cat > "src dir/tests/fixtures/s3_server.py" <<'PY'
import http.server
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self,*a):pass
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length',0)));b=b'{}';self.send_response(200);self.send_header('Content-Length',str(len(b)));self.end_headers();self.wfile.write(b)
s=http.server.HTTPServer(('127.0.0.1',0),H);print(s.server_address[1],flush=True);s.serve_forever()
PY
cat > w.c <<'C'
#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
int main(void){char exe[PATH_MAX];ssize_t n=readlink("/proc/self/exe",exe,sizeof exe-1);if(n<0)return 1;exe[n]=0;
char path[PATH_MAX];snprintf(path,sizeof path,"%s/../lib/x86_64-linux-gnu/putgd/libputgd-s3.so",dirname(exe));
if(!dlopen(path,RTLD_NOW))return 2;sleep(1);return 0;}
C
echo 'int putgd_stub_module;' > m.c
printf 'probe diagnostic: \xff\x00 not json {"operations":\n' > malformed.bin
bundle(){ d="out dir/rev-$1"; mkdir -p "$d/bin" "$d/lib/x86_64-linux-gnu/putgd"; cc -shared -fPIC -o "$d/lib/x86_64-linux-gnu/putgd/libputgd-s3.so" m.c; cc -o "$d/bin/putgd-s3-worker" w.c -ldl
 printf '#!/bin/bash\nd=$(dirname "$(realpath "$0")")\n"$d/putgd-s3-worker" & wait $!\n%s\n' "$2" > "$d/bin/operations_upload_probe"; chmod +x "$d/bin/operations_upload_probe"; }
bundle aaaaaaaa "echo '{\"operations\":[{\"files\":[{\"status\":\"uploaded\"}]}]}'"
bundle dddddddd "cat \"$T/malformed.bin\"; exit 5"
pj(){ python3 -c 'import json,sys;d=json.loads(sys.stdin.read().strip().splitlines()[-1]);print(d["pass"],d["probeExit"],d.get("smokeError"))' 2>/dev/null; }
out=$(bash "$lab/module_smoke_v4.sh" "src dir" "out dir" aaaaaaaa 2>&1); check "module_smoke_v4 pass" "$?/$(echo "$out" | pj)/$([[ -e "out dir/smoke-aaaaaaaa/result.classified.json" ]] && echo classified || echo raw)" "0/True 0 None/raw"
out=$(bash "$lab/module_smoke_v4.sh" "src dir" "out dir" dddddddd 2>&1); rc=$?
check "module_smoke_v4 malformed output: failure, probeExit kept" "$rc/$(echo "$out" | pj)" "1/False 5 missing_or_invalid_result"
check "module_smoke_v4 malformed output: raw bytes preserved" "$(cmp -s malformed.bin "out dir/smoke-dddddddd/result.json" && echo identical || echo changed)" "identical"
check "module_smoke_v4 malformed output: classification separate" "$(python3 -c 'import json,sys;d=json.load(open(sys.argv[1]));print(d["smokeError"],d["rawResult"],d["rawBytes"])' "out dir/smoke-dddddddd/result.classified.json")" "missing_or_invalid_result result.json $(stat -c %s malformed.bin)"
sleep .3; check "module_smoke_v4 stub fixtures stopped" "$(pgrep -f -- "$T/src dir/tests/fixtures/s3_server.py" | wc -l)" "0"
cd /; rm -rf -- "$T"; echo "$n checks, failed=$fail"; exit $fail
