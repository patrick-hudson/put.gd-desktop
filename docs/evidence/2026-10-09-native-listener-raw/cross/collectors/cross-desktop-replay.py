#!/usr/bin/env python3
"""Gated GNOME/KDE replay. Preparation/help are local; no phase auto-runs another."""
from pathlib import Path
import argparse, hashlib, json, re, shlex, subprocess, sys, tarfile
ROOT=Path('/home/patrick/work/put.gd')
HERE=ROOT/'work/ui-refinement/native-replay'
sys.path.insert(0,str(HERE))
import replay
CANDIDATE=HERE/'nativecore-exp001-c49'
PREVIOUS=HERE/'frozen-exp001-source46'
COMMIT='676f6e6d6e0b026dcdd1b4ef2ae7dabc8a29ded3'
COUNT=49
DEFAULT_OUTPUT=HERE/'nativecore-exp001-c49-cross'

def source():
 s=replay.read(CANDIDATE/'source.json')
 assert s['commit']==COMMIT and s['commitCount']==COUNT and s['dirty'] is False
 assert replay.sha(s['archive'])==s['archiveSha256']
 return s

def authorize(expected):
 # Called before constructing Replay, which inspects existing run metadata.
 assert re.fullmatch('[0-9a-f]{64}',expected or ''), 'Exact root-approved gate SHA required'
 path=CANDIDATE/'cinnamon/gates.json'
 assert replay.sha(path)==expected, 'Cinnamon gate receipt changed'
 gate=replay.read(path)
 assert gate['schemaVersion']==1 and gate['sourceCommit']==COMMIT
 assert all(gate.get(k) is True for k in ('passed','functionalPassed','memoryPassed'))
 assert isinstance(gate.get('receipts'),list) and gate['receipts'], 'Real gate evidence required'
 for item in gate['receipts']:
  p=Path(item['path']);p=p if p.is_absolute() else path.parent/p
  assert p.name not in ('credentials.txt','password') and '.aws' not in p.parts
  assert replay.sha(p)==item['sha256'], 'A Cinnamon evidence hash changed'
 source()
 return gate

def prepare_local(args):
 # No Replay construction, run_for, SSH, QMP, guest inventory or secret access.
 out=Path(args.output).resolve()
 assert out.parent==HERE and re.fullmatch('[A-Za-z0-9-]+',out.name)
 out.mkdir(mode=0o700,exist_ok=False)
 s=source();(out/'source.json').write_bytes((CANDIDATE/'source.json').read_bytes())
 guests={}
 for name in ('gnome','kde'):
  started=replay.read(PREVIOUS/name/'start.json');installed=replay.read(PREVIOUS/name/'installed.json')
  resident=started['resident'];assert resident['sha256']==installed['binarySha256']['/usr/bin/putgd']
  assert installed['manifest']['commitCount']==46
  guests[name]={'guest':{'processes':[{'comm':'putgd',**resident}],'manifest':installed['manifest']}}
 replay.save(out/'preparation-inventory.json',{'schemaVersion':1,'utc':replay.now(),'guests':guests,'scope':'Saved source46 identities only; no fresh guest interaction; stop revalidates every identity'})
 replay.save(out/'prepared.json',{'utc':replay.now(),'sourceCommit':s['commit'],'CinnamonGate':'pending; mandatory SHA and all three gate flags before every guest phase','previousCohort':str(PREVIOUS),'runnerSha256':replay.sha(__file__),'fallbackAdapters':['gnome-wayland','kde-wayland'],'nativeAcceptance':'pending','memoryAcceptance':'pending'})

class CrossReplay(replay.Replay):
 def __init__(self,args):
  authorize(args.cinnamon_gates_sha256)
  super().__init__(args)
  replay.save(self.attempt/'cinnamon-authorization.json',{'utc':replay.now(),'path':str(CANDIDATE/'cinnamon/gates.json'),'sha256':args.cinnamon_gates_sha256,'sourceCommit':COMMIT,'functionalPassed':True,'memoryPassed':True,'latencyPassed':authorize(args.cinnamon_gates_sha256)['latencyPassed'],'scope':'Regression-test expansion after approved functional/memory prerequisites; no keep or release acceptance','runnerSha256':replay.sha(__file__)})
 def command(self,*args,**kwargs):
  authorize(self.args.cinnamon_gates_sha256)
  return super().command(*args,**kwargs)
 def all_stopped(self):
  authorize(self.args.cinnamon_gates_sha256)
  assert (self.dest/'stopped.json').is_file(), 'Stop this target old resident before its build/install'
 def source(self):
  s=super().source();assert s['commit']==COMMIT and s['commitCount']==COUNT
  return s
 def assert_current(self):
  resident,snapshot=super().assert_current()
  assert snapshot['diagnostics']['capabilities']['source']['adapter']==self.name+'-wayland'
  # These declared fallback routes have no ephemeral Cinnamon capture helper.
  return resident,snapshot
 def guest_build(self):
  self.all_stopped()
  # Existing distro compiler/Qt and pinned official SDK only; never apt-install.
  self.shell('''set -eu
command -v cmake
command -v ninja
command -v c++
command -v python3
command -v dpkg-deb
pkg-config --atleast-version=6.4.2 Qt6Widgets
pkg-config --exists xcb xcb-randr xcb-keysyms xcb-xfixes wayland-client
test -f /home/lab/putgd-memory-build/sdk/lib/cmake/AWSSDK/AWSSDKConfig.cmake
''')
  frozen=self.source()
  with tarfile.open(frozen['archive']) as archive:
   lock=archive.extractfile('cmake/dependency-lock.json').read()
  assert hashlib.sha256(lock).hexdigest()==frozen['files']['cmake/dependency-lock.json']['sha256']
  expected=json.loads(lock)['awsSdk']
  sdkCheck="""from pathlib import Path
import json,subprocess
expected=EXPECTED
sdk='/home/lab/putgd-memory-build/dependencies/aws-sdk-cpp'
def git(*args):return subprocess.check_output(['git','-C',sdk,*args],text=True)
assert git('rev-parse','HEAD').strip()==expected['commit']
actual={}
for line in git('submodule','status','--recursive').splitlines():
 assert line.startswith(' ')
 commit,path,*_=line.split();actual[path]=commit
assert actual=={item['path']:item['commit'] for item in expected['submodules']}
assert not git('status','--porcelain','--untracked-files=no').strip()
print(json.dumps({'officialSdkVerified':True,'commit':expected['commit'],'submoduleCount':len(actual)}))
"""
  verified=self.python(sdkCheck.replace('EXPECTED',repr(expected)))
  replay.save(self.attempt/'sdk-preflight.json',verified)
  super().guest_build()
 def install(self):
  super().install()
  installed=replay.read(self.dest/'installed.json');build=replay.read(self.dest/'build.json')
  import tempfile
  with tempfile.TemporaryDirectory(prefix='putgd-cross-package-') as tmp:
   self.command(['dpkg-deb','--extract',build['package'],tmp])
   expected=replay.sha(Path(tmp)/'usr/bin/putgd-capture')
  actual=self.python("from pathlib import Path\nimport hashlib,json,subprocess\np=Path('/usr/bin/putgd-capture')\nprint(json.dumps({'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'version':subprocess.check_output([str(p),'--version'],text=True).strip()}))\n")
  assert actual['sha256']==expected and actual['version']=='putgd-capture '+installed['manifest']['version']
  replay.save(self.dest/'capture-helper-install.json',{'utc':replay.now(),'actual':actual,'expectedPackageSha256':expected,'scope':'Installed version proof only; fallback capture does not launch this helper'})
 def unlock_wake(self):
  # The inherited unlock tool uses the existing visible console and VNC output.
  self.command([sys.executable,str(HERE/'unlock-visible.py'),self.name,'wake','--output',str(self.out)])
 def unlock_password(self):
  assert self.args.masked_focus_reviewed, 'Inspect visibly focused masked field first'
  self.command([sys.executable,str(HERE/'unlock-visible.py'),self.name,'type-password','--output',str(self.out)])
 def memory(self):
  resident,snapshot=self.assert_current()
  assert not snapshot['diagnostics']['ui'].get('pid') and self.cli('diagnostics')['activeOperations']==0
  build=replay.read(self.dest/'build.json');modulePath=build['guestDirectory']+'/source/tests/performance/bench_ui_process.py'
  digest=self.source()['files']['tests/performance/bench_ui_process.py']['sha256']
  parameters={'scope':'guest post-native-use quiescent whole-family idle; not physical Cinnamon or cold memory certification','guest':self.name,'durationSeconds':self.args.seconds,'intervalSeconds':5,'sourceCommit':COMMIT,'resident':resident,'moduleSha256':digest,'vncPort':replay.GUESTS[self.name][2],'fortyMiBTargetClaimed':False}
  replay.save(self.attempt/'memory-parameters.json',parameters)
  code=MEMORY.replace('MODULE_PATH',repr(modulePath)).replace('MODULE_SHA',repr(digest)).replace('RESIDENT',repr(resident)).replace('SECONDS',str(self.args.seconds))
  result=self.python(code,timeout=self.args.seconds+60)
  after,_=self.assert_current();assert after==resident and self.cli('diagnostics')['activeOperations']==0
  replay.save(self.attempt/'memory.json',{'utc':replay.now(),'parameters':parameters,'result':result})

MEMORY='''from pathlib import Path
import hashlib,importlib.util,json,os,time
module=Path(MODULE_PATH);assert hashlib.sha256(module.read_bytes()).hexdigest()==MODULE_SHA
spec=importlib.util.spec_from_file_location('putgd_family',module);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
resident=RESIDENT
# Reject untracked pre-existing GUI/capture processes rather than omit their RSS.
for p in Path('/proc').iterdir():
 try:
  if p.name.isdecimal() and p.stat().st_uid==os.getuid():
   assert (p/'comm').read_text().strip() not in ('putgd-ui','putgd-capture'),'Untracked helper exists before idle measurement'
 except FileNotFoundError:pass
family=m.ProcessFamily(resident['pid'],'/usr/bin/putgd','/usr/bin/putgd-ui',resident['startTicks'])
assert family.root['startTicks']==resident['startTicks'] and m.sha256(Path('/proc')/str(resident['pid'])/'exe')==resident['sha256']
start=time.monotonic();samples=[]
for index in range(SECONDS//5+1):
 delay=start+index*5-time.monotonic()
 if delay>0:time.sleep(delay)
 sample=family.sample();assert sample['processCount']==1 and sample['uiProcessCount']==0 and sample['escapedProcessCount']==0,'Idle family changed'
 samples.append({'elapsedSeconds':time.monotonic()-start,**sample})
wall=samples[-1]['elapsedSeconds']-samples[0]['elapsedSeconds'];ticks=sum(p['cpuTicks'] for p in samples[-1]['processes'])-sum(p['cpuTicks'] for p in samples[0]['processes'])
print(json.dumps({'samples':samples,'observedExits':family.exited,'peakRssBytes':max(x['totals']['rssBytes'] for x in samples),'finalRssBytes':samples[-1]['totals']['rssBytes'],'finalPssBytes':samples[-1]['totals']['pssBytes'],'finalPrivateBytes':samples[-1]['totals']['privateBytes'],'cpuPercentOneCore':100*ticks/os.sysconf('SC_CLK_TCK')/wall,'scope':'VM quiescent post-native-use totals, no GUI interaction or acquisition during samples'}))
'''

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--output',default=str(DEFAULT_OUTPUT));p.add_argument('--guest',choices=['gnome','kde'])
 p.add_argument('--cinnamon-gates-sha256');p.add_argument('--seconds',type=int,default=30)
 p.add_argument('--masked-focus-reviewed',action='store_true');p.add_argument('--view',choices=['settings','history','activity','about'],default='settings')
 p.add_argument('--label',default='view');p.add_argument('--kind',default='empty');p.add_argument('--values',nargs='*',default=[])
 p.add_argument('phase',choices=['prepare-local','stop','guest-build','install','start','configure','matrix','show','close','step','memory','unlock-wake','unlock-password'])
 args=p.parse_args()
 if args.phase=='prepare-local':prepare_local(args);return
 assert args.guest and args.seconds in (30,60,300)
 r=CrossReplay(args)
 try:
  getattr(r,args.phase.replace('-','_'))()
  replay.save(r.attempt/'complete.json',{'utc':replay.now(),'phase':args.phase})
 except Exception as error:
  replay.save(r.attempt/'failed.json',{'utc':replay.now(),'phase':args.phase,'errorType':type(error).__name__,'message':'Preserve attempt; diagnose explicitly before retry'})
  raise
if __name__=='__main__':main()
