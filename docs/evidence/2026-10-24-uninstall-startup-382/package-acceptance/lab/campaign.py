"""Targeted #382 active/inactive-account uninstall acceptance, exact CI artifacts."""
import sys,subprocess,json,time,shlex,hashlib,concurrent.futures,re
from pathlib import Path
import control,guest_control
from full_rfb import RFB
E=Path('work/i382n/evidence');E.mkdir(exist_ok=True)
PORTS={'f44':5970,'d69':5979,'k72':5972,'m74':5974,'u382a':5992,'a382a':5993}
SESSION='codex-i382n-acceptance'
def unique(p):
 if not p.exists():return p
 for n in range(2,100):
  q=p.with_name(p.stem+'.attempt'+str(n)+p.suffix)
  if not q.exists():return q
 raise RuntimeError('too many repeated attempts')
class Row:
 def __init__(self,target,mode,artifact):
  self.target,self.mode,self.artifact=target,mode,artifact;self.out=E/target/mode;self.out.mkdir(parents=True,exist_ok=True);self.physical=target in control.HOSTS;self.remote_home='/home/patrick' if self.physical else '/home/lab';self.base=self.remote_home+'/work/p382n';self.started=time.monotonic();self.heartbeat=time.monotonic();self.memory_assessment={}
  (self.out/'driver-identity.json').write_text(json.dumps({'argv':sys.argv,'campaignSha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'remoteDriverSha256':hashlib.sha256((Path(__file__).parent/'i382.py').read_bytes()).hexdigest()},indent=2)+'\n')
 def remote(self,args,timeout=1200):
  return control.run(self.target,args,timeout) if self.physical else guest_control.guest(self.target,args,timeout=timeout)
 def lock(self,verb):
  args=['python3','tools/lab/desktop_lock.py',verb,self.target,'--session',SESSION]
  if verb=='acquire':args+=['--owner','Codex','--purpose','#382 corrected startup-link uninstall/login acceptance; work/i382n','--minutes','45']
  if verb=='release':args+=['--verified-clean']
  r=subprocess.run(args,capture_output=True,text=True);unique(self.out/(verb+'-lock.txt')).write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr
 def call(self,*args):
  if self.physical and time.monotonic()-self.heartbeat>300:self.lock('heartbeat');self.heartbeat=time.monotonic()
  r=self.remote(['python3',self.base+'/i382.py',self.mode,*args]);unique(self.out/(re.sub(r'[^A-Za-z0-9._-]','_', '-'.join(args[:3]))[:160]+'.jsonl')).write_text(json.dumps(r)+'\n');assert not r['exit'],r
  d=json.loads(r['stdout']);print(self.target,self.mode,' '.join(args[:3]),'passed',flush=True);return d
 def frame(self,label):
  before=self.call('status','frame-'+label);assert not before['personalApps'],before['personalApps']
  for i in range(12):
   path=self.out/(label+('.attempt'+str(i+1) if i else '')+'.png')
   try:
    r=RFB(PORTS[self.target]);f=r.screenshot(str(path));r.close()
    if len(set(f[2][::97]))<=20:raise RuntimeError('uninitialized/blank VNC frame; kept as failure context')
    (self.out/(label+'-visual.json')).write_text(json.dumps({'file':path.name,'at':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'sessionId':before['sessionId'],'running':before['residents'],'build':before.get('snapshot',{}).get('build'),'needsHumanVisualReview':True},indent=2)+'\n');return
   except (OSError,RuntimeError) as x:
    unique(self.out/(label+'-frame-error.txt')).write_text(str(x))
    if i==2 and self.target in ('k72','d69'):
     r=self.remote(['sh','-c','systemctl --user restart plasma-xdg-desktop-portal-kde.service; systemctl --user restart xdg-desktop-portal.service; systemctl --user restart w0vncserver.service']);unique(self.out/(label+'-wall-recovery.json')).write_text(json.dumps(r)+'\n')
    time.sleep(2)
  raise RuntimeError(self.target+' wall/frame did not recover')
 def package(self,verb,label):
  pkg=self.artifact if verb=='install' else 'putgd';s=self.call('pkg-sim',verb,pkg,label);text=s['stdout']
  if 'apt-get' in s['command']:
   import re
   changes=re.findall(r'^(?:Inst|Remv|Purg) (\S+)',text,re.M);assert all(p=='putgd' for p in changes),(self.target,changes,text)
  else:
   assert not any(t in text for t in ['Upgrading:','Removing unused dependencies:','Removing dependent packages:']),text
   # The initial RPM/dependencies were already reviewed in the earlier pass;
   # new dependency changes still need an explicit transaction review.
   assert 'Installing dependencies:' not in text,text
  return self.call('pkg-apply',verb,pkg,label)
 def install(self,label):
  if self.mode=='native':self.package('install',label)
  elif label!='initial':self.call('restore-image',label)
 def remove(self,label):
  if self.mode=='native':self.package('remove',label)
  else:self.call('remove-image',label)
 def login(self,label,present,already_out=False):
  if not already_out:self.call('logout',label)
  if self.target=='u382a':
   r=subprocess.run([sys.executable,'work/i382n/guest_login.py',self.mode+'-'+label],capture_output=True,text=True);unique(self.out/(label+'-console-login.txt')).write_text(r.stdout+r.stderr);assert not r.returncode,r.stderr
  else:self.call('relogin',label)
  self.call('check','present' if present else 'absent',label)
 def assess_memory(self,mem):
  assessment={'strictPass':mem['pass'],'budgetPass':mem['peakRssMiB']<40 and mem['pssMiB']<40,'baselineException':None}
  if not mem['pass']:
   assert self.target=='f44' and self.mode=='native',mem
   assert assessment['budgetPass'] and len(mem['forbiddenMaps'])==1 and re.fullmatch(r'/usr/lib64/libcrypto\.so\.[0-9.]+',mem['forbiddenMaps'][0]),mem
   deps=self.out/'fedora-qt-dependencies.txt'
   if not deps.exists():
    proof=self.remote(['sh','-c','readelf -d /usr/lib64/libQt6Core.so.6; rpm -q qt6-qtbase openssl-libs']);deps.write_text(proof['stdout']+proof['stderr'])
   assert deps.exists() and 'Shared library: [libcrypto.so.3]' in deps.read_text()
   assessment['baselineException']={'mapping':mem['forbiddenMaps'][0],'priorSource':'05991d9d (v26.1023.1058-nightly)','priorEvidence':'docs/evidence/2026-10-23-native-full-acceptance/README.md, item C','currentEvidence':deps.name,'reason':'Pre-existing direct dependency of Fedora Qt6Core; not introduced by the uninstall change. Raw strict failure retained.'}
  self.memory_assessment=assessment;(self.out/'memory-assessment.json').write_text(json.dumps(assessment,indent=2)+'\n')
 def run(self,prepared=False,after_memory=False):
  if (prepared or after_memory) and self.physical:
   r=self.remote(['cat',self.remote_home+'/putgd-lab/locks/desktop.lock']);assert not r['exit'] and 'session: '+SESSION in r['stdout'],r
  elif self.physical:self.lock('acquire')
  if not after_memory:
   if not prepared:self.call('prepare',*([self.artifact] if self.mode=='appimage' else []));self.call('identity')
   self.install('initial');self.call('launch','initial');self.call('enable');self.call('show','enabled');self.frame('enabled-settings');self.call('quit','prep-login')
   self.login('enabled',True);self.frame('enabled-login');mem=self.call('memory','180')
  else:mem=json.loads(json.loads((self.out/'memory-180.jsonl').read_text())['stdout'])
  self.assess_memory(mem)
  self.call('show','login-enabled');self.frame('login-settings');self.call('quit','inactive')
  # Stop the transient watch to model a user manager that ended with logout.
  # Remove only after loginctl confirms there is no active graphical account.
  self.call('stop-watch');self.call('retention','before','inactive');self.call('logout','inactive');self.call('assert-inactive');self.remove('inactive');self.login('inactive',False,already_out=True);self.call('retention','after','inactive');self.frame('inactive-cleaned-login')
  self.install('inactive');self.call('launch','active-rearm');self.call('show','rearmed');self.frame('rearmed-settings');self.call('quit','active');self.call('retention','before','active');self.remove('active');self.call('wait-removed');self.call('retention','after','active');self.frame('active-cleaned');self.login('active',False);self.frame('active-cleaned-login')
  self.install('active');self.call('launch','disable-rearm');self.call('disable');self.call('show','disabled');self.frame('disabled-settings');self.call('quit','disabled');self.remove('disabled');self.login('disabled',False);self.frame('disabled-clean-login');self.call('cleanup')
  if self.physical:self.lock('release')
  (self.out/'completion.json').write_text(json.dumps({'target':self.target,'mode':self.mode,'sourceCommit':'d5b561ddb94f8255b8bc754c1382552d983a6fa9','functionalPassed':True,'memoryPassed':self.memory_assessment['strictPass'],'memoryAssessment':self.memory_assessment,'seconds':round(time.monotonic()-self.started,1),'visualReviewPending':True},indent=2)+'\n');print(self.target,self.mode,'CYCLE COMPLETE; visual review pending',flush=True)
if __name__=='__main__':
 target,mode,artifact=sys.argv[1:4];Row(target,mode,artifact).run('--prepared' in sys.argv[4:],'--after-memory' in sys.argv[4:])
