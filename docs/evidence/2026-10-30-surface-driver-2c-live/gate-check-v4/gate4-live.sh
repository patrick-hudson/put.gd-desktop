#!/bin/bash
# Fourth-validation live gate check (synthetic; no credentials): production CLI + direct contexts.
set -u
cd ~/work/put.gd/src
D=$(git rev-parse HEAD); H=$(python3 -c "import json;print(json.load(open('$HOME/sd726/catalog/manifest.json'))['build']['gitCommit'])"); B=${H:0:12}
CAT=$HOME/sd726/catalog; RUN=work/lab-runs/${1:-c900}; G=$HOME/sd726/gate4; PIN=tools/lab/surface_driver/secret-states.json
rm -rf $G; mkdir -p $G
common="--desktop mint-cinnamon-x11 --host boat --placement guest --guest cinnamon-x11 --run $RUN --package deb --recipes core_demo"
echo "driver $D  app+catalog $H  build-arg $B"; git status --short | head -3
show() { echo "exit=$1 out_exists=$(test -e $2 && echo yes || echo no)"; }
echo "== A. correct configuration, destination s3"
python3 tools/lab/surface_driver/run.py $common --destination s3 --catalog $CAT --build $B --out $G/out-admitted --coverage $G/admitted-coverage.json > $G/admitted.out 2> $G/admitted.err; rc=$?
grep -h "blocked by secret-safe evidence" $G/admitted.err || echo "gate: admitted"; show $rc $G/out-admitted
python3 -c "
import json;r=json.load(open('$G/out-admitted/receipt.json'));e=r['environment']
print('resident build', e.get('observed_build',{}).get('gitCommit'), 'dirty', e.get('observed_build',{}).get('dirty'), 'blocked:', e.get('blocked'), 'secret_scan', r.get('secret_scan',{}).get('passed'))"
echo "== B1. edited local pin: write-pin on a catalog without the credential states"
cp -r $CAT $G/reduced; python3 - $G/reduced <<'PY'
import json, sys
from pathlib import Path
d = Path(sys.argv[1]); m = json.loads((d/'manifest.json').read_text())
drop = [c['controls'] for c in m['captures'] if 'credential' in (c.get('controls') or '')]
m['captures'] = [c for c in m['captures'] if c.get('controls') not in drop]
(d/'manifest.json').write_text(json.dumps(m)); [(d/x).unlink() for x in drop]; print('removed', len(drop), 'credential states')
PY
python3 -m tools.lab.surface_driver.gate write-pin $G/reduced; git status --short $PIN
python3 tools/lab/surface_driver/run.py $common --destination s3 --catalog $G/reduced --build $B --out $G/out-editedpin 2>&1 | tail -1; show ${PIPESTATUS[0]} $G/out-editedpin
git checkout -q -- $PIN; git status --short $PIN; echo "pin restored"
echo "== B2. one control removed from one state, in both the local pin and the catalog"
cp -r $CAT $G/onecontrol; python3 - $G/onecontrol $PIN <<'PY'
import json, sys
from pathlib import Path
d, pin = Path(sys.argv[1]), Path(sys.argv[2])
inv = d/'04-destinations-s3.controls.json'; v = json.loads(inv.read_text())
v['controls'] = [c for c in v['controls'] if c.get('name') != 'credentialAccessKey']; inv.write_text(json.dumps(v))
p = json.loads(pin.read_text()); p['states'].pop('04-destinations-s3'); pin.write_text(json.dumps(p, indent=2, sort_keys=True)+'\n')
print('removed credentialAccessKey from 04-destinations-s3 (its only secret control) in catalog and local pin')
PY
python3 -m tools.lab.surface_driver.gate check-catalog $G/onecontrol; echo "local check-catalog exit=$? (the checkout's own pin agrees)"
python3 tools/lab/surface_driver/run.py $common --destination s3 --catalog $G/onecontrol --build $B --out $G/out-onecontrol 2>&1 | tail -1; show ${PIPESTATUS[0]} $G/out-onecontrol
git checkout -q -- $PIN; git status --short $PIN; echo "pin restored"
echo "== B3. a build whose own committed pin differs: 3d9cb9db's catalog with --build 3d9cb9db (driver pin is newer)"
python3 tools/lab/surface_driver/run.py $common --destination s3 --catalog $HOME/sd726/catalog-3d9cb9db --build 3d9cb9db97cb --out $G/out-stalepin 2>&1 | tail -1; show ${PIPESTATUS[0]} $G/out-stalepin
echo "== C. placement label: direct DriverContext construction"
python3 - "$RUN" "$G" "$CAT" "$B" <<'PY'
import shutil, sys
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0, '.')
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.gate import LiveEvidenceRefused
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.target import Target
run, G, cat, build = sys.argv[1], Path(sys.argv[2]), sys.argv[3], sys.argv[4]
profile = load_profile('mint-cinnamon-x11')
env = dict(desktop='mint-cinnamon-x11', host='boat', guest='cinnamon-x11', package='deb', destination='s3', build=build)
guest = Target(run=Path(run))
physical = Target(run=Path(run)); physical.physical = SimpleNamespace(config={'host': '192.168.68.74', 'user': 'patrick', 'ssh_port': 22})
fake = SimpleNamespace(run=G/'not-a-guest', physical=None, root='/tmp/sd-x', socket='/tmp/sd-x/s')
(G/'not-a-guest').mkdir(); shutil.copy(Path(run)/'state.json', G/'not-a-guest'/'state.json')
cases = (('placement boat on a physical (builder-like) target', physical, 'boat', {'boat_sandbox_id': 'bx_v5jp6qqv'}),
         ('placement guest on a physical target', physical, 'guest', {}),
         ('placement boat on a copied run directory with no owned QEMU', fake, 'boat', {}),
         ('placement guest on the real owned QEMU guest', guest, 'guest', {}),
         ('placement boat on the real owned QEMU guest (nested guest on boat)', guest, 'boat', {}))
for label, target, placement, details in cases:
    out = G/('ctx-'+str(abs(hash(label))))
    try:
        ctx = DriverContext(target, dict(env, placement=placement, placement_details=details), profile, cat, out)
        print(label, '-> admitted, live_admitted =', ctx._live_admitted, '| out exists:', out.exists())
    except LiveEvidenceRefused as error:
        print(label, '-> refused:', str(error).split(': ', 1)[1], '| out exists:', out.exists())
PY
git status --short | head -3; echo DONE
