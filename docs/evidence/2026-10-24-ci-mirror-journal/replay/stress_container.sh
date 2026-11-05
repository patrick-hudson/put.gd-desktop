#!/usr/bin/env bash
set -euo pipefail
profile=$1
cd "/lab/$profile"
private="/tmp/pg393$profile"
mkdir -m 700 "$private"
for path in config data cache state runtime; do mkdir -m 700 "$private/$path"; done
export XDG_CONFIG_HOME="$private/config" XDG_DATA_HOME="$private/data" XDG_CACHE_HOME="$private/cache"
export XDG_STATE_HOME="$private/state" XDG_RUNTIME_DIR="$private/runtime" QT_QPA_PLATFORM=offscreen
unset DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
python3 tools/diagnostics/check_socket_paths.py --socket "$private/runtime/putgd/session.sock" \
  /tmp/pgst.XXXXXXXX/test_operations-XXXXXX/runtime/putgd/session.sock >"/lab/evidence/stress-$profile-preflight.json"
python3 - "$profile" <<'PY'
import json,os,pathlib,platform,subprocess,sys
def run(*args):return subprocess.check_output(args,text=True).strip()
data={'host':'d69 physical / '+platform.node(),'osRelease':pathlib.Path('/etc/os-release').read_text(),
      'qt':run('pkg-config','--modversion','Qt6Core'),'compiler':run('c++','--version'),
      'source':run('git','rev-parse','HEAD'),'tree':run('git','rev-parse','HEAD^{tree}'),
      'status':run('git','status','--porcelain'),'binaryHash':run('sha256sum','build/dev/test_operations'),
      'buildInfo':json.loads(pathlib.Path('build/dev/build-identity/build-info.json').read_text()),
      'ldd':run('ldd','build/dev/test_operations'),'environment':{k:os.environ.get(k) for k in
        ('DISPLAY','WAYLAND_DISPLAY','DBUS_SESSION_BUS_ADDRESS','QT_QPA_PLATFORM','XDG_RUNTIME_DIR')},
      'scope':'Headless full operations_contract; one busy loop and CTest on CPU0, nice10, no desktop/memory acceptance'}
pathlib.Path('/lab/evidence/context-'+sys.argv[1]+'.json').write_text(json.dumps(data,indent=2)+'\n')
PY
exec nice -n 10 python3 tools/ci/stress_suite.py --source "$PWD" --out "/lab/evidence/stress-$profile" \
  --iterations 10 --hogs 1 --hog-cpus 0 --test-cpus 0 --time-scale 3 --iteration-timeout 360 \
  --label "$profile-head5683c71f" -- --preset dev -R '^operations_contract$' --parallel 1
