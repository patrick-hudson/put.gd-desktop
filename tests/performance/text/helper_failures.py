#!/usr/bin/env python3
"""One manual helper-failure checklist, never a ctest test.

A runner's documented fault interface is supplied as a JSON mapping from case
name to argv. No guessed switches: absent cases remain blocked. The instrumented
runner must report error, helperPid, helperStartTicks, helperExit, threadsGone,
noPayloadOnDisk. A human reviews its scan/exit witnesses alongside this receipt.
"""
import argparse,json,subprocess,hashlib
from pathlib import Path
CASES=('crash','hang-deadline','limit-data','limit-cpu','limit-file-size','limit-descriptors','cancel','wrong-build','wrong-protocol','corrupt-data','missing-data','confinement-refused','parent-killed')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--plan',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    plan=json.loads(a.plan.read_text()) if a.plan else {};receipt=dict(status='blocked',cases=[],assessment='Manual checklist; unsupported fault injection is blocked, never inferred.')
    with a.output.open('x') as out:
        for name in CASES:
            row=dict(case=name,status='blocked');receipt['cases'].append(row)
            if name not in plan:row['reason']='instrumented test_ocr_run fault interface not supplied';continue
            case=plan[name];cmd=case['argv'];binary=Path(cmd[0]).resolve();row['runner_sha256']=hashlib.sha256(binary.read_bytes()).hexdigest()
            try:
                r=subprocess.run(cmd,capture_output=True,text=True,timeout=35);v=json.loads(r.stdout)
                required=('error','helperPid','helperStartTicks','helperExit','threadsGone','noPayloadOnDisk','witnesses')
                if any(k not in v for k in required):raise ValueError('missing lifecycle/scan witness')
                if type(v['helperPid'])!=int or v['helperPid']<=0 or not str(v['helperStartTicks']).isdecimal() or int(v['helperStartTicks'])<=0:raise ValueError('helper identity missing')
                exit=v['helperExit']
                if not isinstance(exit,dict) or exit.get('pid')!=v['helperPid'] or str(exit.get('startTicks'))!=str(v['helperStartTicks']) or not ('exitCode' in exit or 'signal' in exit):raise ValueError('helper exit identity missing')
                for filename,sha in v['witnesses'].items():
                    f=Path(filename)
                    if f.is_symlink() or not f.is_file() or hashlib.sha256(f.read_bytes()).hexdigest()!=sha:raise ValueError('failure witness missing or changed')
                if not v['helperExit'] or not v['threadsGone'] or not v['noPayloadOnDisk'] or not v['witnesses']:raise ValueError('incomplete failure observation')
                if v['error']!=case['expectedError']:raise ValueError('unexpected terminal error')
                proc=Path('/proc')/str(v['helperPid'])
                if proc.exists() and (proc/'stat').read_text().rsplit(') ',1)[1].split()[19]==str(v['helperStartTicks']):raise ValueError('helper remains alive')
                row.update(status='measured',observation={k:v[k] for k in required},runnerExit=r.returncode)
            except (ValueError,KeyError,subprocess.TimeoutExpired) as e:row['reason']=str(e)
        if all(c['status']=='measured' for c in receipt['cases']):receipt['status']='measured'
        json.dump(receipt,out,indent=2)
    return 0 if receipt['status']=='measured' else 2
if __name__=='__main__':raise SystemExit(main())
