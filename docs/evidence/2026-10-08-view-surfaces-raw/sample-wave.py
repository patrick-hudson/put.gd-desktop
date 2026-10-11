"""Record one common, quiet native-VM observation using the frozen sampler."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import datetime, hashlib, json, subprocess, sys
ROOT=Path('/home/patrick/work/put.gd'); OUT=ROOT/'work/memory-surfaces'
sys.path.insert(0,str(ROOT/'tools/lab'));import lab
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
sampler=ROOT/'work/cross-desktop-current/guest-sample-resident.py'
assert sha(sampler)=='725dd5d4a34ffbf74db161e3d08286d7990f0d5569fa50aab293c354b5f42f73'
assert sha(ROOT/'tools/lab/lab.py')=='09ed6fd84f09a90a617aab38ca646089e9819def06668fe7d29da5173f9efb89'
assert not (OUT/'memory-wave.json').exists()
wave={'schemaVersion':1,'sourceCommit':'dac46af06feb0e8326d472c77bdf96cff117be51',
      'candidateSourceTree':'d7f9c996d6dd43fc864bcb42239431ff7b2e47d1',
      'sourceArchiveSha256':sha(OUT/'candidate-001b.tar'),'scope':'Candidate post-functional native VM observation; separate from cold physical target',
      'startedUtc':now(),'parameters':{'durationSeconds':120,'intervalSeconds':5,'warmupSeconds':5},
      'guestScriptSha256':sha(sampler),'labRunnerSha256':sha(ROOT/'tools/lab/lab.py'),'runs':{}}
for name in ('cinnamon','gnome','kde'):
 run=ROOT/f'work/lab-runs/current-{name}-a3d70dc';state=lab.state(run);identity=lab.verify_running_app(run)
 cases=[]
 for p in (run/'tests').glob('*/results.json'):
  result=json.loads(p.read_text())
  if result.get('runningApp')==identity:cases.append(result)
 assert len(cases)==12 and all(x['ok'] for x in cases),(name,'case count or outcome')
 assert len({(x['scenario'],x['clipboardClient']) for x in cases})==12
 profile=lab.load_profile(state['profile'])
 operation_count=len(lab.cli(run,'operation.recent')['operations'])
 assert operation_count==(13 if name=='gnome' else 12)
 wave['runs'][name]={'run':str(run),'profile':state['profile'],'vncPort':5900+state['vnc_display'],
  'hostQemuPid':state['pid'],'runningApp':identity,'package':lab.read_json(run/'package.json'),
  'functionalTestIds':[x['testId'] for x in cases], 'operationCount':operation_count,
  'vcpus':profile['cpus'],'vmMemoryMiB':profile['memory_mib'],'status':'ready'}
lab.write_json(OUT/'memory-wave-started.json',wave)
def observe(item):
 name,row=item;identity=row['runningApp'];command=['python3','-','--pid',str(identity['pid']),
  '--start-ticks',str(identity['startTicks']),'--sha256',identity['sha256'],'--seconds','120','--interval','5']
 started=now();p=subprocess.run(lab.ssh_args(row['run'])+command,input=sampler.read_text(),text=True,capture_output=True,timeout=150)
 (OUT/name/'memory-sample.stderr').write_text(p.stderr)
 assert p.returncode==0,(name,p.returncode)
 path=OUT/name/'memory-sample.json';lab.write_json(path,json.loads(p.stdout))
 after=lab.verify_running_app(row['run']);assert after==identity
 return name,{'exitCode':p.returncode,'startedUtc':started,'finishedUtc':now(),'command':command,
  'resultSha256':sha(path),'identityUnchanged':after==identity}
with ThreadPoolExecutor(max_workers=3) as pool:
 for name,receipt in pool.map(observe,wave['runs'].items()):wave['runs'][name]['measurement']=receipt
wave['finishedUtc']=now();lab.write_json(OUT/'memory-wave.json',wave)
print(json.dumps({'completed':list(wave['runs']),'wave':str(OUT/'memory-wave.json')}))
