#!/bin/bash
# Stub tests for f-run-v3.sh, campaign_o5.sh and module_smoke_v2.sh, meant to run from a clean `git archive`
# export (scripts keep mode 100644 and are invoked through bash). Launches no product, display or network
# beyond a stub loopback server.  usage: bash test_wrappers.sh LABDIR
set -uo pipefail
lab=$(realpath -e -- "$1"); T=$(mktemp -d "/tmp/i560 wrapper test.XXXX"); fail=0
check(){ if [[ $2 == "$3" ]]; then echo "PASS $1 ($3)"; else echo "FAIL $1: got $2 want $3"; fail=1; fi; }
# --- f-run-v3 ---
root="$T/work root"; mkdir -p "$root"
printf 'set -e\nmkdir "$I560_WORK_ROOT/$1"\necho "stub ran $1" >> "$I560_WORK_ROOT/$1/stub.log"\n' > "$T/stub-f-run.sh"
I560_WORK_ROOT="$root" I560_F_RUNNER="$T/stub-f-run.sh" bash "$lab/f-run-v3.sh" zq >/dev/null 2>&1; check "f-run-v3 no scope" "$?/$(ls "$root" | wc -l)" "3/0"
I560_CLIPBOARD_SCOPE=shared I560_WORK_ROOT="$root" I560_F_RUNNER="$T/stub-f-run.sh" bash "$lab/f-run-v3.sh" zq >/dev/null 2>&1; check "f-run-v3 invalid scope" "$?/$(ls "$root" | wc -l)" "3/0"
I560_CLIPBOARD_SCOPE=dedicated:test-approval I560_WORK_ROOT="$root" I560_F_RUNNER="$T/stub-f-run.sh" bash "$lab/f-run-v3.sh" zq >/dev/null 2>&1; rc=$?
check "f-run-v3 permitted dispatch" "$rc/$(cat "$root/zq/stub.log" 2>/dev/null)/$(cat "$root/zq.clipboard-scope.txt" 2>/dev/null)" "0/stub ran zq/dedicated:test-approval"
I560_CLIPBOARD_SCOPE=dedicated:test-approval I560_WORK_ROOT="$root" I560_F_RUNNER="$T/stub-f-run.sh" bash "$lab/f-run-v3.sh" zq >/dev/null 2>&1; check "f-run-v3 existing output" "$?/$(grep -c . "$root/zq/stub.log")" "2/1"
bash "$lab/f-run-v3.sh" >/dev/null 2>&1; check "f-run-v3 no label" "$?" "2"
# --- campaign_o5 (runner stub keeps mode 644, like the export) ---
for n in ua ub; do [[ ! -e /tmp/p560$n && ! -e /tmp/p560${n}b ]] || { echo "FAIL /tmp/p560$n exists"; exit 1; }; done
mkdir -p "$T/h" "$T/ev" "$T/bins/bin-base" "$T/bins/bin-cand"
printf 'mkdir -p "${3}b"; printf "{\\"runExit\\": %%s}" "${STUB_STATUS:-0}" > "${3}b/wrapper-cleanup.json"; exit "${STUB_STATUS:-0}"\n' > "$T/h/run_o4.sh"; chmod 644 "$T/h/run_o4.sh"
STUB_STATUS=0 bash "$lab/campaign_o5.sh" "$T/src" "$T/h" "$T/bins" "$T/ev" >/dev/null 2>&1; check "campaign_o5 stub pass" "$?/$(grep -c 'exit=0 ' "$T/ev/campaign5.log")" "0/2"
rm -rf /tmp/p560ua /tmp/p560uab /tmp/p560ub /tmp/p560ubb "$T/ev/campaign5.log"
STUB_STATUS=37 bash "$lab/campaign_o5.sh" "$T/src" "$T/h" "$T/bins" "$T/ev" >/dev/null 2>&1; check "campaign_o5 stub exit 37" "$?/$(grep -c 'exit=37 ' "$T/ev/campaign5.log")" "1/2"
rm -rf /tmp/p560ua /tmp/p560uab /tmp/p560ub /tmp/p560ubb
bash "$lab/campaign_o5.sh" a b c >/dev/null 2>&1; check "campaign_o5 missing args" "$?" "2"
# --- module_smoke_v2 with relative roots, stub fixture, stub probe/worker and a real tiny shared object ---
cd "$T"; mkdir -p "src dir/tests/fixtures" "out dir/rev-aaaaaaaa/bin" "out dir/rev-aaaaaaaa/lib/x86_64-linux-gnu/putgd"
cat > "src dir/tests/fixtures/s3_server.py" <<'PY'
import http.server,json
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self,*a):pass
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length',0)));b=b'{}';self.send_response(200);self.send_header('Content-Length',str(len(b)));self.end_headers();self.wfile.write(b)
s=http.server.HTTPServer(('127.0.0.1',0),H);print(s.server_address[1],flush=True);s.serve_forever()
PY
echo 'int putgd_stub_module;' > m.c && cc -shared -fPIC -o "out dir/rev-aaaaaaaa/lib/x86_64-linux-gnu/putgd/libputgd-s3.so" m.c && rm m.c
# Stub worker: like the real one, resolves ../lib/<multiarch>/putgd/libputgd-s3.so from its own executable and
# loads it (LD_PRELOAD cannot express a path with spaces), then holds briefly so the smoke can read its maps.
cat > w.c <<'C'
#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
int main(void){char exe[PATH_MAX];ssize_t n=readlink("/proc/self/exe",exe,sizeof exe-1);if(n<0)return 1;exe[n]=0;
char path[PATH_MAX];snprintf(path,sizeof path,"%s/../lib/x86_64-linux-gnu/putgd/libputgd-s3.so",dirname(exe));
if(!dlopen(path,RTLD_NOW))return 2;sleep(2);return 0;}
C
cc -o "out dir/rev-aaaaaaaa/bin/putgd-s3-worker" w.c -ldl && rm w.c
cat > "out dir/rev-aaaaaaaa/bin/operations_upload_probe" <<'SH'
#!/bin/bash
d=$(dirname "$(realpath "$0")")
"$d/putgd-s3-worker" & wait $! || exit 1
echo '{"operations":[{"files":[{"status":"uploaded"}]}]}'
SH
chmod +x "out dir/rev-aaaaaaaa/bin/operations_upload_probe"
out=$(bash "$lab/module_smoke_v2.sh" "src dir" "out dir" aaaaaaaa 2>&1); rc=$?
check "module_smoke_v2 relative roots with spaces" "$rc/$(echo "$out" | python3 -c 'import json,sys;print(json.loads(sys.stdin.read().strip().splitlines()[-1])["pass"])' 2>/dev/null)" "0/True"
bash "$lab/module_smoke_v2.sh" "src dir" "out dir" >/dev/null 2>&1; check "module_smoke_v2 missing TAG" "$?" "2"
bash "$lab/module_smoke_v2.sh" "src dir" "out dir" bbbbbbbb >/dev/null 2>&1; check "module_smoke_v2 missing bundle" "$?" "2"
bash "$lab/module_smoke_v2.sh" "no src" "out dir" aaaaaaaa >/dev/null 2>&1; check "module_smoke_v2 missing SRC" "$?" "2"
cd /; rm -rf "$T"
exit $fail
