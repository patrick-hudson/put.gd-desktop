#!/bin/bash
# Stub tests v4 = v3 plus an explicit process check (#579 Cubic 4372459172): pgrep's status and output are
# classified together, and a failed or inconsistent inspection can never count as stopped fixtures.
# v3 header follows. Stub tests v3 (#579 Cubic 4372411646/4372411656/4372411661/4372411665), for a clean `git archive` export with
# scripts invoked through bash. Setup is guarded step by step: if the lab path, the work directory T, the separate
# stand-in directory S or the stand-in sentinel cannot be created, the test exits 2 before writing anything else
# (expected failures inside the checks are not under errexit). Test output stays inside T. S holds a TEST-OWNED
# stand-in for someone else's data; it is not another user's file. Fixed campaign paths such as /tmp/p560ua are only
# observed, never created or deleted. Launches no product, display or network beyond stub loopback servers in T.
# usage: bash test_wrappers_v4.sh LABDIR
set -uo pipefail
[[ $# -eq 1 ]] || { echo "usage: bash test_wrappers_v4.sh LABDIR" >&2; exit 2; }
lab=$(realpath -e -- "$1") || { echo "bad LABDIR" >&2; exit 2; }
T=$(mktemp -d "/tmp/i560 wrapper test v4.XXXX") || { echo "cannot create work directory" >&2; exit 2; }
[[ -d $T && ! -L $T ]] || { echo "work directory missing" >&2; exit 2; }
S=$(mktemp -d "/tmp/i560 stand-in owner.XXXX") || { echo "cannot create stand-in directory" >&2; rm -rf -- "$T"; exit 2; }
sentinel="$S/stand-in for foreign data"
printf 'test-owned stand-in\n' > "$sentinel" || { echo "cannot create stand-in sentinel" >&2; rm -rf -- "$T" "$S"; exit 2; }
fail=0; n=0
# process_state PATTERN: "none" only when pgrep exits 1 with no output; "matches:<pids>" when it exits 0 with output;
# anything else (exit >= 2, a missing tool, or status and output disagreeing) is "error:..." or "inconsistent:...".
# Read-only: it never signals a process. PGREP can name a stub for the controlled cases below.
process_state(){ local out rc; out=$("${PGREP:-pgrep}" -f -- "$1" 2>&1); rc=$?
 case $rc in
  0) if [[ -n $out ]]; then echo "matches:$(tr '\n' ' ' <<< "$out")"; else echo "inconsistent:0-without-output"; fi;;
  1) if [[ -z $out ]]; then echo none; else echo "inconsistent:1-with-output:$(tr '\n' ' ' <<< "$out")"; fi;;
  *) echo "error:$rc:$(tr '\n' ' ' <<< "$out")";;
 esac; }
check(){ n=$((n+1)); if [[ $2 == "$3" ]]; then echo "PASS $1 ($3)"; else echo "FAIL $1: got $2 want $3"; fail=1; fi; }
state(){ for p in /tmp/p560ua /tmp/p560uab /tmp/p560ub /tmp/p560ubb "$sentinel"; do if [[ -e $p ]]; then stat -c '%n %i %Y %s' -- "$p"; else echo "$p absent"; fi; done; }
before=$(state)
# --- f-run-v3: hostile inherited scope must not leak into the no-scope case ---
export I560_CLIPBOARD_SCOPE=dedicated:hostile-inherited
root="$T/work root"; mkdir -p "$root"
printf 'set -e\nmkdir "$I560_WORK_ROOT/$1"\necho "stub ran $1" >> "$I560_WORK_ROOT/$1/stub.log"\n' > "$T/stub-f-run.sh"
env -u I560_CLIPBOARD_SCOPE I560_WORK_ROOT="$root" I560_F_RUNNER="$T/stub-f-run.sh" bash "$lab/f-run-v3.sh" zq >/dev/null 2>&1; check "f-run-v3 no scope despite hostile inherited env" "$?/$(ls "$root" | wc -l)" "3/0"
I560_CLIPBOARD_SCOPE=shared I560_WORK_ROOT="$root" I560_F_RUNNER="$T/stub-f-run.sh" bash "$lab/f-run-v3.sh" zq >/dev/null 2>&1; check "f-run-v3 invalid scope" "$?/$(ls "$root" | wc -l)" "3/0"
I560_CLIPBOARD_SCOPE=dedicated:test-approval I560_WORK_ROOT="$root" I560_F_RUNNER="$T/stub-f-run.sh" bash "$lab/f-run-v3.sh" zq >/dev/null 2>&1; rc=$?
check "f-run-v3 permitted dispatch" "$rc/$(cat "$root/zq/stub.log" 2>/dev/null)/$(cat "$root/zq.clipboard-scope.txt" 2>/dev/null)" "0/stub ran zq/dedicated:test-approval"
I560_CLIPBOARD_SCOPE=dedicated:test-approval I560_WORK_ROOT="$root" I560_F_RUNNER="$T/stub-f-run.sh" bash "$lab/f-run-v3.sh" zq >/dev/null 2>&1; check "f-run-v3 existing output" "$?/$(grep -c . "$root/zq/stub.log")" "2/1"
env -u I560_CLIPBOARD_SCOPE bash "$lab/f-run-v3.sh" >/dev/null 2>&1; check "f-run-v3 no label" "$?" "2"
unset I560_CLIPBOARD_SCOPE
# --- campaign_o5: the stub runner writes only inside T ---
mkdir -p "$T/h" "$T/ev" "$T/bins/bin-base" "$T/bins/bin-cand"
printf 'echo "$3" >> "%s/runner-calls"; exit "${STUB_STATUS:-0}"\n' "$T" > "$T/h/run_o4.sh"; chmod 644 "$T/h/run_o4.sh"
STUB_STATUS=0 bash "$lab/campaign_o5.sh" "$T/src" "$T/h" "$T/bins" "$T/ev" >/dev/null 2>&1; check "campaign_o5 stub pass" "$?/$(grep -c 'exit=0 ' "$T/ev/campaign5.log")" "0/2"
mv "$T/ev/campaign5.log" "$T/ev/campaign5-pass.log"
STUB_STATUS=37 bash "$lab/campaign_o5.sh" "$T/src" "$T/h" "$T/bins" "$T/ev" >/dev/null 2>&1; check "campaign_o5 stub exit 37" "$?/$(grep -c 'exit=37 ' "$T/ev/campaign5.log")" "1/2"
bash "$lab/campaign_o5.sh" a b c >/dev/null 2>&1; check "campaign_o5 missing args" "$?" "2"
# --- phase2_o6: dirty tree rejected before OUTROOT exists; an existing OUTROOT is kept ---
mkdir -p "$T/p2/bin" "$T/p2/src/tools/diagnostics" "$T/p2/src/build/dev" "$T/p2/h"; touch "$T/p2/src/tools/diagnostics/check_socket_paths.py" "$T/p2/h/probe-control.patch"
printf 'CMAKE_INSTALL_BINDIR:PATH=bin\nCMAKE_INSTALL_LIBDIR:PATH=lib/x86_64-linux-gnu\n' > "$T/p2/src/build/dev/CMakeCache.txt"
printf '#!/bin/bash\ncase "$1" in status) [[ ${FAIL:-} == dirty ]] && echo " M file";; symbolic-ref) echo b;; esac\nexit 0\n' > "$T/p2/bin/git"; chmod +x "$T/p2/bin/git"
R=80ffa2e6c0d34223d2cc0f31254f2e60ac2bd247
(cd "$T/p2" && FAIL=dirty PATH="$T/p2/bin:$PATH" bash "$lab/phase2_o6.sh" src h "out dirty" $R >/dev/null 2>&1); check "phase2_o6 dirty tree before OUTROOT" "$?/$([[ -e "$T/p2/out dirty" ]] && echo created || echo none)" "3/none"
mkdir "$T/p2/out kept"; echo keep > "$T/p2/out kept/existing"
(cd "$T/p2" && PATH="$T/p2/bin:$PATH" bash "$lab/phase2_o6.sh" src h "out kept" $R >/dev/null 2>&1); check "phase2_o6 existing OUTROOT kept" "$?/$(cat "$T/p2/out kept/existing")/$(ls "$T/p2/out kept" | wc -l)" "2/keep/1"
# --- identity_check_v3 ---
printf 'x\ng80ffa2e6c0d3\ny\n' > "$T/good"; printf 'x\ny\n' > "$T/wrong"
bash "$lab/identity_check_v3.sh" "$T/good" $R 3 >/dev/null 2>&1; check "identity_check_v3 correct worker, 3 rounds" "$?" "0"
bash "$lab/identity_check_v3.sh" "$T/good" $R >/dev/null 2>&1; check "identity_check_v3 default rounds" "$?" "0"
bash "$lab/identity_check_v3.sh" "$T/wrong" $R 3 >/dev/null 2>&1; check "identity_check_v3 wrong worker" "$?" "1"
bash "$lab/identity_check_v3.sh" "$T/missing" $R 3 >/dev/null 2>&1; check "identity_check_v3 missing worker" "$?" "2"
bash "$lab/identity_check_v3.sh" "$T/good" 80ffa2e6 3 >/dev/null 2>&1; check "identity_check_v3 short revision" "$?" "2"
for bad in "" 0 -1 abc 1.5 201 007 "2 3"; do bash "$lab/identity_check_v3.sh" "$T/good" $R "$bad" >/dev/null 2>&1; check "identity_check_v3 rounds ${bad@Q} rejected" "$?" "2"; done
# --- module_smoke_v5 with relative roots, stub fixture, stub probe and compiled stub worker ---
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
if(!dlopen(path,RTLD_NOW))return 2;sleep(2);return 0;}
C
echo 'int putgd_stub_module;' > m.c
bundle(){ d="out dir/rev-$1"; mkdir -p "$d/bin" "$d/lib/x86_64-linux-gnu/putgd"; cc -shared -fPIC -o "$d/lib/x86_64-linux-gnu/putgd/libputgd-s3.so" m.c; cc -o "$d/bin/putgd-s3-worker" w.c -ldl
 printf '#!/bin/bash\nd=$(dirname "$(realpath "$0")")\n"$d/putgd-s3-worker" & wait $!\n%s\n' "$2" > "$d/bin/operations_upload_probe"; chmod +x "$d/bin/operations_upload_probe"; }
bundle aaaaaaaa "echo '{\"operations\":[{\"files\":[{\"status\":\"uploaded\"}]}]}'"
bundle bbbbbbbb "echo '{\"operations\":[{\"files\":[{\"status\":\"uploaded\"}]}]}'; exit 7"
pj(){ python3 -c 'import json,sys;d=json.loads(sys.stdin.read().strip().splitlines()[-1]);print(d["pass"],d["probeExit"],d.get("smokeError"))' 2>/dev/null; }
out=$(bash "$lab/module_smoke_v5.sh" "src dir" "out dir" aaaaaaaa 2>&1); check "module_smoke_v5 valid success" "$?/$(echo "$out" | pj)" "0/True 0 None"
out=$(bash "$lab/module_smoke_v5.sh" "src dir" "out dir" bbbbbbbb 2>&1); check "module_smoke_v5 valid shape, probe exit 7" "$?/$(echo "$out" | pj)" "1/False 7 None"
i=0
while IFS='|' read -r body expect; do
 i=$((i+1)); tag=$(printf 'e%07d' "$i"); printf '%s\n' "$body" > "$T/raw-$tag"
 bundle "$tag" "cat \"$T/raw-$tag\"; exit 3"
 out=$(bash "$lab/module_smoke_v5.sh" "src dir" "out dir" "$tag" 2>&1); rc=$?
 check "module_smoke_v5 ${body:0:40} -> $expect" "$rc/$(echo "$out" | pj)/$(cmp -s "$T/raw-$tag" "out dir/smoke-$tag/result.json" && echo raw-kept || echo raw-changed)" "1/False 3 $expect/raw-kept"
done <<'CASES'
null|invalid_schema:root
[]|invalid_schema:root
{}|invalid_schema:operations
{"operations":{}}|invalid_schema:operations
{"operations":[1]}|invalid_schema:operations[0]
{"operations":[{}]}|invalid_schema:operations[0].files
{"operations":[{"files":"x"}]}|invalid_schema:operations[0].files
{"operations":[{"files":[{}]}]}|invalid_schema:operations[0].files[0].status
{"operations":[{"files":[{"status":1}]}]}|invalid_schema:operations[0].files[0].status
not json at all|missing_or_invalid_result
CASES
sleep .3; state=$(process_state "$T/src dir/tests/fixtures/s3_server.py"); check "module_smoke_v5 stub fixtures stopped (inspection: $state)" "$state" "none"
bash "$lab/module_smoke_v5.sh" "src dir" "out dir" >/dev/null 2>&1; check "module_smoke_v5 missing TAG" "$?" "2"
bash "$lab/module_smoke_v5.sh" "no src" "out dir" aaaaaaaa >/dev/null 2>&1; check "module_smoke_v5 missing SRC" "$?" "2"
# --- process_state with controlled pgrep stubs: only a clean "no match" may pass ---
mkdir "$T/pg"
stub(){ printf '#!/bin/bash\n%s\n' "$2" > "$T/pg/$1"; chmod +x "$T/pg/$1"; }
stub none 'exit 1'; stub match 'echo 4242; exit 0'; stub err2 'echo "pgrep: bad pattern" >&2; exit 2'; stub err3 'exit 3'
stub odd0 'exit 0'; stub odd1 'echo 99; exit 1'
for c in "none|none" "match|matches:4242 " "err2|error:2:pgrep: bad pattern " "err3|error:3:" "odd0|inconsistent:0-without-output" "odd1|inconsistent:1-with-output:99 " "missing|error:127:"; do
 name=${c%%|*}; want=${c#*|}; got=$(PGREP="$T/pg/$name" process_state "anything")
 # A missing tool reports its own message after "error:127:"; compare that case by prefix, the stubs exactly.
 if [[ $name == missing ]]; then shown=${got:0:${#want}}; else shown=$got; fi
 check "process_state stub $name" "$shown" "$want"
 check "process_state stub $name cannot pass as stopped" "$([[ $got == none ]] && echo counts-as-stopped || echo not-stopped)" "$([[ $name == none ]] && echo counts-as-stopped || echo not-stopped)"
done
# --- ownership: fixed paths and the foreign sentinel are exactly as before ---
cd /; after=$(state); check "fixed campaign paths and test-owned stand-in unchanged" "$([[ $before == "$after" ]] && echo same || echo changed)" "same"
rm -rf -- "$T" "$S"
echo "$n checks, failed=$fail"; exit $fail
