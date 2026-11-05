#!/bin/bash
# Live gate check through the production CLI on an owned Cinnamon guest (synthetic; no credentials).
set -u
cd ~/work/put.gd/src
D=$(git rev-parse HEAD); H=$(python3 -c "import json;print(json.load(open(\"$HOME/sd726/catalog/manifest.json\"))[\"build\"][\"gitCommit\"])"); B=${H:0:12}; CAT=$HOME/sd726/catalog; RUN=work/lab-runs/${1:-c800}; G=$HOME/sd726/gate
rm -rf $G; mkdir -p $G/stub
common="--desktop mint-cinnamon-x11 --host boat --placement guest --guest cinnamon-x11 --run $RUN --package deb --recipes core_demo"
echo "driver $D  app+catalog $H  build-arg $B"; tesseract --version 2>&1 | head -1
echo "== pin: catalog secret-control states vs committed pin"
python3 -m tools.lab.surface_driver.gate check-catalog $CAT; echo "exit=$?"
show() { echo "exit=$1 out_exists=$(test -e $2 && echo yes || echo no)"; }
echo "== A. correct configuration: guest placement, exact build, pinned catalog, real OCR, destination s3"
python3 tools/lab/surface_driver/run.py $common --destination s3 --catalog $CAT --build $B --out $G/out-admitted --coverage $G/admitted-coverage.json > $G/admitted.out 2> $G/admitted.err; rc=$?
grep -h "blocked by secret-safe evidence" $G/admitted.err || echo "gate: admitted (no secret-safe evidence blocker)"; show $rc $G/out-admitted
python3 -c "
import json;r=json.load(open('$G/out-admitted/receipt.json'));e=r['environment']
print('resident build', e.get('observed_build',{}).get('gitCommit'), 'blocked:', e.get('blocked'))
print('rows', sorted({(i['functional'], (i.get('note') or '')[:70]) for i in r['interactions']})[:4]); print('secret_scan', r.get('secret_scan',{}).get('passed'))"
echo "== B1. build identity"
for b in 2 ${H:0:8} 26.1030.2490-dev; do echo "-- --build $b"; python3 tools/lab/surface_driver/run.py $common --destination s3 --catalog $CAT --build $b --out $G/out-b$b 2>&1 | tail -1; show ${PIPESTATUS[0]} $G/out-b$b; done
echo "-- --build ffffffffffff (12 hex, another commit)"; python3 tools/lab/surface_driver/run.py $common --destination s3 --catalog $CAT --build ffffffffffff --out $G/out-bother 2>&1 | tail -1; show ${PIPESTATUS[0]} $G/out-bother
echo "== B2. catalog with all destination/credential secret states but one removed (manifest edited to match)"
cp -r $CAT $G/reduced; python3 - $G/reduced <<'PY'
import json, sys
from pathlib import Path
d = Path(sys.argv[1]); m = json.loads((d/'manifest.json').read_text())
sys.path.insert(0, '.'); from tools.lab.surface_driver.gate import catalog_states
states = catalog_states(d)[1]
drop = [k for k in states if any(w in k for w in ('destination', 'credential', 'proton', 'settings'))][1:]
m['captures'] = [c for c in m['captures'] if (c.get('controls') or '')[:-len('.controls.json')] not in drop]
(d/'manifest.json').write_text(json.dumps(m))
for k in drop: (d/(k+'.controls.json')).unlink()
print('removed', len(drop), 'of', len(drop)+1, 'destination/credential secret states')
PY
python3 tools/lab/surface_driver/run.py $common --destination s3 --catalog $G/reduced --build $B --out $G/out-reduced 2>&1 | tail -1; show ${PIPESTATUS[0]} $G/out-reduced
echo "== B3. OCR that replays a fixed probe token"
printf '#!/bin/sh\ncat >/dev/null\necho "Link copied: https://put.gd/Kq7Zp3Wx9Ty.png"\n' > $G/stub/tesseract; chmod 755 $G/stub/tesseract
PATH=$G/stub:$PATH python3 tools/lab/surface_driver/run.py $common --destination s3 --catalog $CAT --build $B --out $G/out-ocr 2>&1 | tail -1; show ${PIPESTATUS[0]} $G/out-ocr
echo "== B4. bypass run.main: DriverContext / run_recipes directly (the old evidence-driver path)"
python3 - "$RUN" "$G" "$CAT" "$B" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, '.')
from tools.lab.surface_driver.api import Recipe
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.gate import LiveEvidenceRefused
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.run import run_recipes
from tools.lab.surface_driver.target import Target
run, G, cat, build = sys.argv[1], Path(sys.argv[2]), sys.argv[3], sys.argv[4]
target = Target(run=Path(run)); profile = load_profile('mint-cinnamon-x11')
env = dict(desktop='mint-cinnamon-x11', host='boat', placement='guest', guest='cinnamon-x11', package='deb', destination='s3', build=build)
for label, change, catalog in (('reduced catalog', {}, G/'reduced'), ('native placement', {'placement': 'native'}, cat),
                               ('version build', {'build': '26.1030.2490-dev'}, cat)):
    out = G/('bypass-'+label.replace(' ', '-'))
    try:
        DriverContext(target, dict(env, **change), profile, catalog, out); print(label, 'ADMITTED (finding)')
    except LiveEvidenceRefused as error:
        print(label, '-> refused:', str(error).split(': ', 1)[1], '| out exists:', out.exists())
local = DriverContext(target, dict(env, destination='local'), profile, cat, G/'bypass-local')
try:
    run_recipes(local, [Recipe(('editor.undo',), lambda c: None, destinations=('s3',))], env, G/'bypass-run', 'gate', G/'bypass-run'/'coverage.json')
    print('run_recipes with an unadmitted context ADMITTED (finding)')
except LiveEvidenceRefused as error:
    print('run_recipes with an unadmitted context -> refused:', error, '| receipt exists:', (G/'bypass-run'/'receipt.json').exists())
PY
echo "== C. core_demo, local destination"
python3 tools/lab/surface_driver/run.py $common --destination local --catalog $CAT --build $B --out $G/out-local --coverage $G/local-coverage.json > $G/local.out 2> $G/local.err; rc=$?
cat $G/local.out | tail -5; echo "exit=$rc"
python3 -c "
import json;r=json.load(open('$G/out-local/receipt.json'))
print([(i['interaction'], i['functional'], i.get('visual')) for i in r['interactions']]); print('secret_scan', r['secret_scan']['passed'])"
