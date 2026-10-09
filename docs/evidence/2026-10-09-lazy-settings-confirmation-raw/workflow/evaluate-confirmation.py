#!/usr/bin/env python3
"""Persist paired metrics and call the installed decision tool; never integrate."""
import argparse
import datetime
import json
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path('/home/patrick/work/put.gd')
WORK = ROOT/'work/view-startup-latency'
SCOPE = ROOT/'.context/compound-engineering/ce-optimize/view-startup-latency'
SKILL = Path('/home/patrick/.codex/plugins/cache/compound-engineering-plugin/compound-engineering/3.30.4/skills/ce-optimize')
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runs', required=True)
args = parser.parse_args()
out = Path(args.runs).resolve(strict=True)
assert out.parent == WORK
execution = json.loads((out/'execution.json').read_text())
assert execution['status']=='complete'
spec = json.loads((SCOPE/'spec.yaml').read_text())
ledger = json.loads((SCOPE/'experiment-log.yaml').read_text())

def save(path,value):
    path.write_text(json.dumps(value,indent=2)+'\n')
    assert json.loads(path.read_text())==value

metrics={}
for kind in ('reference','candidate'):
    command = shlex.join([sys.executable,str(WORK/'evaluate.py'),'--result',str(out/(kind+'-1/results.json')),'--result',str(out/(kind+'-2/results.json'))])
    argv=['bash',str(SKILL/'scripts/measure.sh'),command,'30',str(ROOT)]
    result=subprocess.run(argv,capture_output=True,text=True)
    (out/(kind+'-aggregation.stdout')).write_text(result.stdout)
    (out/(kind+'-aggregation.stderr')).write_text(result.stderr)
    save(out/(kind+'-aggregation-command.json'),{'argv':argv,'exitCode':result.returncode})
    result.check_returncode()
    metrics[kind]=json.loads(result.stdout)
    save(out/(kind+'-metrics.json'),metrics[kind])
    # Raw aggregation is durable before decision dispatch.
    marker=WORK/'exp001/work/lazy-confirmation/result.yaml'
    value=json.loads(marker.read_text());value['aggregates']=metrics.copy();save(marker,value)

entry=next(x for x in ledger['experiments'] if x['iteration']==1)
entry['metrics']=metrics['candidate']
entry['matchedReferenceMetrics']=metrics['reference']
entry['outcome']='measured'
save(SCOPE/'experiment-log.yaml',ledger)
node = next((name for name in ('node','nodejs') if subprocess.run(['bash','-c','command -v "$1" >/dev/null 2>&1', '_', name]).returncode==0),None)
assert node,'Host workflow decision runtime unavailable; guest/product tests do not use Node'
decisions={}
for pairing,baseline in [('original-source41',ledger['baseline']),('matched-source45',{'metrics':metrics['reference'],'gates':metrics['reference']})]:
    payload={'spec':spec,'baseline':baseline,'candidate':{'metrics':metrics['candidate'],'gates':metrics['candidate'],'smoke_passed':True}}
    path=out/(pairing+'-decision-input.json');save(path,payload)
    argv=[node,str(SKILL/'scripts/decide.mjs'),str(path)]
    result=subprocess.run(argv,capture_output=True,text=True)
    (out/(pairing+'-decision.stdout')).write_text(result.stdout)
    (out/(pairing+'-decision.stderr')).write_text(result.stderr)
    save(out/(pairing+'-decision-command.json'),{'argv':argv,'exitCode':result.returncode,'runtimeScope':'Installed host-only Compound Engineering workflow; no product/guest dependency'})
    result.check_returncode();decisions[pairing]=json.loads(result.stdout)
    save(out/(pairing+'-decision.json'),decisions[pairing])
    ledger=json.loads((SCOPE/'experiment-log.yaml').read_text())
    entry=next(x for x in ledger['experiments'] if x['iteration']==1)
    if 'comparisons' not in entry:entry['comparisons']=[]
    assert not any(x['pairing']==pairing for x in entry['comparisons'])
    entry['comparisons'].append({'pairing':pairing,'baseline':baseline,'candidate':metrics['candidate'],'decision':decisions[pairing],'source46Candidate':'6f6372e79d8bd81346924aacec7eac842db98725','classification':'Original contract versus current candidate' if pairing.startswith('original') else 'Matched incremental lazy-page contribution; independent UI/tray changes excluded'})
    save(SCOPE/'experiment-log.yaml',ledger)
save(out/'decisions.json',decisions)
print(json.dumps({'metrics':metrics,'decisions':decisions}))
