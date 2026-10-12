#!/usr/bin/env python3
"""Issue7 candidate2 gnome-wayland preparation/runtime driver. No import-time guest access."""
import argparse, datetime, hashlib, importlib.util, json, os, re, shlex, stat, subprocess, sys, tarfile, tempfile, time, uuid
from pathlib import Path
os.umask(0o077)
ROOT=Path(__file__).resolve().parents[1]
EXPECTED=None
WAYLAND_SOCKET=None
SOURCE=Path('/home/patrick/.codex/worktrees/live-s3-memory/put.gd')
RUN=Path('/home/patrick/work/put.gd/work/lab-runs/p2g-a1')
CASES=[(s,'qt') for s in ('region','window','desktop','clipboard-text','clipboard-image','clipboard-files','region-cancel-escape','region-cancel-right-click','region-cancel-zero-area')]+[(s,'gtk') for s in ('clipboard-text','clipboard-image','clipboard-files')]
TERMINAL={'completed','cancelled','failed','partial','interrupted'}
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
now=lambda:datetime.datetime.now(datetime.timezone.utc).isoformat()
def read(p):return json.loads(Path(p).read_text())
def save(p,v):
 with Path(p).open('x') as f:json.dump(v,f,indent=2);f.write('\n')
def check(v,msg):
 if not v:raise RuntimeError(msg)
def command(argv):return subprocess.check_output(argv,text=True).strip()
def expected_identity(args):
 global EXPECTED
 EXPECTED={'commit':args.commit,'tree':args.tree,'count':args.count,'version':args.version}
 check(args.count==114,'parent_tls_fix_checkpoint_required')
 identity={'branch':args.branch,'gitCommit':args.commit,'gitTree':args.tree,'commitCount':args.count,'version':args.version}
 check(bool(args.branch) and re.fullmatch('[0-9a-f]{40}',args.commit) and re.fullmatch('[0-9a-f]{40}',args.tree),'explicit_full_candidate_identity_required')
 check(args.count>0 and args.version==f'0.1.1-dev.{args.count}+g{args.commit[:12]}','candidate_version_identity_mismatch')
 check(identity['gitCommit']==EXPECTED['commit'] and identity['gitTree']==EXPECTED['tree'] and identity['commitCount']==EXPECTED['count'] and identity['version']==EXPECTED['version'],'explicit_current_diagnostics_identity_required')
 return identity
def source_snapshot(identity):
 check(command(['git','-C',str(SOURCE),'branch','--show-current'])==identity['branch'],'wrong_source_branch')
 check(command(['git','-C',str(SOURCE),'rev-parse','HEAD'])==identity['gitCommit'],'wrong_candidate2_commit')
 check(command(['git','-C',str(SOURCE),'rev-parse','HEAD^{tree}'])==identity['gitTree'],'wrong_candidate2_tree')
 check(int(command(['git','-C',str(SOURCE),'rev-list','--count','HEAD']))==identity['commitCount'],'wrong_candidate2_count')
 check(not command(['git','-C',str(SOURCE),'status','--porcelain','--untracked-files=all']),'diagnostics_source_not_clean')
 untracked=command(['git','-C',str(SOURCE),'ls-files','--others','--exclude-standard','--','src','tests','tools/lab','tools/diagnostics','cmake']).splitlines()
 check(not untracked,'untracked_candidate_source_requires_parent_freeze')
 names=command(['git','-C',str(SOURCE),'ls-files']).splitlines()
 selected=[p for p in names if p.startswith(('src/','tests/','tools/lab/','tools/diagnostics/')) or p in ('CMakeLists.txt','CMakePresets.json') or p.startswith('cmake/')]
 check(all(Path(p).name!='credentials.txt' for p in selected),'secret_path_rejected')
 return {'sourceCheckout':str(SOURCE),'branch':identity['branch'],'gitCommit':command(['git','-C',str(SOURCE),'rev-parse','HEAD']),'gitTree':command(['git','-C',str(SOURCE),'rev-parse','HEAD^{tree}']),'sourceFiles':{p:sha(SOURCE/p) for p in selected},'trackedDiffSha256':hashlib.sha256(subprocess.check_output(['git','-C',str(SOURCE),'diff','HEAD','--','src','tests','tools/lab','tools/diagnostics','cmake','CMakeLists.txt','CMakePresets.json'])).hexdigest()}
def expansion_gate(path):
 release=read(path)
 check(release.get('gnomeFullRegressionAuthorized') is True and release.get('kdeTargetedPassed') is True,'kde_targeted_release_required')
 check(release['identity']=={'commit':EXPECTED['commit'],'tree':EXPECTED['tree'],'count':EXPECTED['count'],'version':EXPECTED['version']},'release_source_identity_mismatch')
 for label in ('kdeTargetedUploads','kdeTargetedCleanup','campaign'):
  record=release[label];q=Path(record['path']);s=q.lstat()
  check(stat.S_ISREG(s.st_mode) and s.st_uid==os.getuid() and not s.st_mode&0o077,'release_receipt_not_owned_private')
  check(sha(q)==record['sha256'],'release_receipt_changed');value=read(q)
  if label=='campaign':
   check(value['aggregateCap']=={'objects':128,'bytes':33554432},'aggregate_campaign_cap_changed')
   check(value['partitions']['gnomeFullRegression']=={'objects':32,'bytes':10485760},'gnome_partition_cap_changed')
  else:
   check(value['passed'] is True and value['sourceCommit']==EXPECTED['commit'],'current_kde_targeted_pass_required')
   check(value['uploadsPassed']==12 if label=='kdeTargetedUploads' else value['removed']==12,'kde_targeted_count_mismatch')
 return {'path':str(Path(path).resolve()),'sha256':sha(path)}
def verify_wayland(m,path):
 code="import json,os,stat,subprocess\nfrom pathlib import Path\nenv=dict(line.split('=',1) for line in subprocess.check_output(['systemctl','--user','show-environment'],text=True).splitlines() if '=' in line);display=env.get('WAYLAND_DISPLAY');assert display and '/' not in display;assert not os.environ.get('WAYLAND_DISPLAY') or os.environ['WAYLAND_DISPLAY']==display;p=Path("+repr(path)+");s=p.lstat();assert stat.S_ISSOCK(s.st_mode) and s.st_uid==os.getuid();assert str(p)==str(Path(os.environ['XDG_RUNTIME_DIR'])/display);print(json.dumps({'path':str(p),'uid':s.st_uid,'waylandDisplay':display,'environmentSource':'native user manager; compare Shell when present','session':os.environ.get('XDG_SESSION_TYPE')}))"
 value=json.loads(m.guest_session(RUN,"python3 - <<'PY'\n"+code+"\nPY\n"));check(value['session']=='wayland','native_wayland_session_required');return value

def socket_paths(proxy=None):
 return [str((RUN/'qmp.sock').resolve()),str((proxy/'qmp.sock').resolve()) if proxy else str((ROOT/'r'/'future-001'/'qmp.sock').resolve()),'/run/user/1000/putgd/session.sock',WAYLAND_SOCKET,'/run/user/1000/bus','/tmp/.X11-unix/X0','/run/dbus/system_bus_socket']
def preflight(proxy=None):
 check(isinstance(WAYLAND_SOCKET,str) and Path(WAYLAND_SOCKET).is_absolute() and '\x00' not in WAYLAND_SOCKET,'actual_wayland_socket_path_required')
 argv=['python3',str(SOURCE/'tools/diagnostics/check_socket_paths.py'),'--socket',*socket_paths(proxy)]
 value=json.loads(command(argv));check(value['passed'],'socket_path_overlong');return {'argv':argv,'result':value}
def package_data(path):
 with tempfile.TemporaryDirectory(prefix='c2-deb-') as tmp:
  subprocess.run(['dpkg-deb','--extract',str(path),tmp],check=True,stdout=subprocess.DEVNULL)
  base=Path(tmp);build=read(base/'usr/share/putgd/build-info.json')
  binaries={str(p.relative_to(base)):sha(p) for p in (base/'usr/bin').glob('putgd*') if p.is_file() and os.access(p,os.X_OK)}
 return build,{'/'+p:h for p,h in binaries.items()}
def validate_pin():
 global EXPECTED
 p=read(ROOT/'candidate.json');check(p['candidateId']=='issue7-http-diagnostics','wrong_candidate_id');identity=p['identity'];EXPECTED={'commit':identity['gitCommit'],'tree':identity['gitTree'],'count':identity['commitCount'],'version':identity['version']};check(EXPECTED['count']==114,'tls_fix_checkpoint_required')
 check(sha(p['package'])==p['packageSha256'],'package_changed')
 check(command(['dpkg-deb','-f',p['package'],'Version'])==p['build']['packageVersion'],'wrong_package_version')
 check(sha(p['sourceFreeze'])==p['sourceFreezeSha256'],'source_freeze_changed')
 check(source_snapshot(p['identity'])==p['sourceSnapshot'],'candidate_source_changed')
 check(sha(__file__)==p['driverSha256'],'driver_changed_since_pin');return p

def bind(args):
 identity=expected_identity(args)
 release=expansion_gate(args.expansion_release)
 freeze=Path(args.source_freeze).resolve();check(sha(freeze)==args.source_freeze_sha256,'freeze_hash_mismatch');f=read(freeze)
 check(f['candidateId']=='issue7-http-diagnostics','freeze_candidate_mismatch')
 snap=source_snapshot(identity);check(f['sourceSnapshot']==snap,'freeze_source_mismatch')
 pkg=Path(args.package).resolve();check(sha(pkg)==args.package_sha256,'package_hash_mismatch')
 build,binaries=package_data(pkg);check(build==f['build'],'package_build_freeze_mismatch')
 check(build['version']==identity['version'] and build['gitCommit']==identity['gitCommit'] and build['commitCount']==identity['commitCount'] and build['dirty'] is False,'package_version_or_clean_candidate2_mismatch')
 check(build['gitCommit']==snap['gitCommit'] and build['gitTree']==snap['gitTree'],'build_source_mismatch')
 check(f['packageSha256']==args.package_sha256,'freeze_package_mismatch')
 check(f['buildAuditPassed'] is True and sha(f['buildAuditReceipt'])==f['buildAuditReceiptSha256'],'build_audit_required')
 check(command(['dpkg-deb','-f',str(pkg),'Version'])==build['packageVersion'],'deb_build_version_mismatch')
 save(ROOT/'candidate.json',{'schemaVersion':1,'candidateId':'issue7-http-diagnostics','identity':identity,'preparedUtc':now(),'sourceFreeze':str(freeze),'sourceFreezeSha256':args.source_freeze_sha256,'sourceSnapshot':snap,'package':str(pkg),'packageSha256':args.package_sha256,'build':build,'installedBinarySha256':binaries,'runState':read(RUN/'state.json'),'runStateSha256':sha(RUN/'state.json'),'driverSha256':sha(__file__),'socketPreflight':preflight(),'executionAuthorized':False,'waylandSocket':WAYLAND_SOCKET,'expansionRelease':release})
 (ROOT/'candidate.json').chmod(0o400)
 print(json.dumps({'candidatePin':str(ROOT/'candidate.json'),'sha256':sha(ROOT/'candidate.json'),'graphicalExecuted':False}))

def load_runtime(args):
 global WAYLAND_SOCKET
 p=validate_pin();g=read(args.go);WAYLAND_SOCKET=p['waylandSocket']
 check(expansion_gate(p['expansionRelease']['path'])==p['expansionRelease'],'expansion_release_changed')
 check(g['expansionRelease']==p['expansionRelease'] and g['waylandSocket']==WAYLAND_SOCKET,'handoff_expansion_or_wayland_mismatch')
 check(g['candidatePinSha256']==sha(ROOT/'candidate.json'),'handoff_candidate_mismatch')
 for key in ('graphicalAuthorized','vmOwnershipTransferred','guestLiveS3Drained','guestProfileIsolated','hostQuiet'):
  check(g.get(key) is True,'handoff_missing_'+key)
 check(g['runStateSha256']==p['runStateSha256']==sha(RUN/'state.json'),'vm_run_changed')
 state=read(RUN/'state.json');check(state['profile']=='gnome-wayland' and state['ssh_port']==22236 and state['vnc_display']==36,'wrong_vm')
 check(Path(state['overlay']).resolve()==RUN/'disk.qcow2','wrong_overlay')
 check((RUN/'qmp.sock').is_socket(),'existing_vm_socket_missing')
 pre=preflight()
 sp=importlib.util.spec_from_file_location('c2_lab',SOURCE/'tools/lab/lab.py');m=importlib.util.module_from_spec(sp);sp.loader.exec_module(m)
 verify_wayland(m,WAYLAND_SOCKET)
 package=read(RUN/'package.json');check(package['sha256']==p['packageSha256'],'wrong_installed_package')
 check(package['installedBinarySha256']==p['installedBinarySha256'],'installed_executable_set_mismatch')
 resident=m.verify_running_app(RUN);check(resident==g['resident'],'handoff_resident_changed')
 d=m.cli(RUN,'diagnostics');check(d['build']==p['build'] and d['journalReady'],'runtime_build_mismatch')
 # The parent must first preserve its live-S3 profile and hand over fresh synthetic-only state.
 s=m.cli(RUN,'settings.get');check(s['document']['profiles']==[],'live_or_s3_profile_rejected')
 return p,m,resident,pre,s

def resources(m,resident):
 # Values, identities, counts and FD classes only; no payload or application log content.
 code=r'''import json,os,hashlib,collections,sqlite3
from pathlib import Path
pid=PID;root=Path('/proc')/str(pid);before=int((root/'stat').read_text().rsplit(')',1)[1].split()[19]);classes=collections.Counter()
for fd in (root/'fd').iterdir():
 try:
  v=os.readlink(fd);kind='socket' if v.startswith('socket:') else 'pipe' if v.startswith('pipe:') else 'anon_inode' if v.startswith('anon_inode:') else 'file';classes[kind]+=1
 except FileNotFoundError:pass
family=[]
for q in Path('/proc').iterdir():
 if not q.name.isdecimal():continue
 try:
  if q.stat().st_uid!=os.getuid():continue
  comm=(q/'comm').read_text().strip();argv=(q/'cmdline').read_bytes().split(bytes([0]));script=Path(argv[1].decode()).name if comm.startswith('python') and len(argv)>1 and argv[1] else ''
  if comm.startswith('putgd') or script in ('gtk_clipboard_fixture.py','observe_issue7.py','sc-notification-observer.py'):
   family.append({'pid':int(q.name),'comm':comm,'startTicks':int((q/'stat').read_text().rsplit(')',1)[1].split()[19]),'script':script or None,'executable':Path(os.readlink(q/'exe')).name,'executableSha256':hashlib.sha256((q/'exe').read_bytes()).hexdigest()})
 except (FileNotFoundError,ProcessLookupError):pass
journal=Path.home()/'putgd-lab/data/putgd/putgd/history.sqlite';rows=[];deletions=[]
assert journal.is_file() and journal.stat().st_uid==os.getuid()
if journal.exists():
 with sqlite3.connect(journal.as_uri()+'?mode=ro',uri=True) as db:
  rows=[dict(id=x[0],state=x[1]) for x in db.execute("SELECT json_extract(data,'$.id'),json_extract(data,'$.state') FROM operations")]
  deletions=[dict(id=x[0],state=x[1]) for x in db.execute("SELECT json_extract(data,'$.id'),json_extract(data,'$.state') FROM deletions")]
after=int((root/'stat').read_text().rsplit(')',1)[1].split()[19]);assert before==after
print(json.dumps({'pid':pid,'startTicks':before,'threads':len(list((root/'task').iterdir())),'fdClasses':dict(classes),'fdCount':sum(classes.values()),'family':family,'operations':rows,'deletions':deletions}))
'''.replace('PID',str(resident['pid']))
 return json.loads(m.ssh(RUN,"python3 - <<'PY'\n"+code+"\nPY\n"))

def runtime(args):
 p,m,resident,pre,s=load_runtime(args)
 out=ROOT/'evidence'/('a-'+uuid.uuid4().hex[:12]);out.mkdir(mode=0o700,parents=True)
 r={'schemaVersion':1,'phase':args.phase,'argv':sys.argv,'utc':now(),'candidatePinSha256':sha(ROOT/'candidate.json'),'candidateBuild':p['build'],'resident':resident,'socketPreflight':pre,'passed':False,'visualReview':'pending'}
 def persist(): (out/'receipt.json').write_text(json.dumps(r,indent=2)+'\n')
 def shot(label):
  check(re.fullmatch('[a-z0-9-]+',label),'invalid_witness_label');q=out/(label+'.ppm')
  with m.QMP(RUN/'qmp.sock') as z:z.execute('screendump',{'filename':str(q),'device':'video0','head':0})
  from PIL import Image
  png=q.with_suffix('.png');Image.open(q).save(png)
  value={'path':str(png),'sha256':sha(png),'ppmSha256':sha(q),'vncTcp':5936,'reviewRequired':True};r.setdefault('witnesses',[]).append(value);return value
 def observed():
  st=m.cli(RUN,'settings.get');check(st['document']['profiles']==[],'profile_changed')
  return {'acceptedRevision':st['metadata']['acceptedRevision'],'observedRevision':st['metadata']['observedRevision'],'documentSha256':hashlib.sha256(json.dumps(st['document'],sort_keys=True,separators=(',',':')).encode()).hexdigest(),'profiles':0,'actionOutputs':{a:v['destination'] for a,v in st['document']['actions'].items()},'ui':m.cli(RUN,'ui.snapshot')['diagnostics']['ui'],'diagnostics':m.cli(RUN,'diagnostics')}
 def visual_input():
  v=read(args.visual_input);check(v['candidatePinSha256']==sha(ROOT/'candidate.json') and v['resident']==resident,'visual_input_wrong_candidate')
  check(v['visuallyInspected'] is True and sha(v['witness']['path'])==v['witness']['sha256'],'actual_image_review_required')
  check(v['uiPid']==m.cli(RUN,'ui.snapshot')['diagnostics']['ui']['pid'],'visual_input_helper_changed');return v
 try:
  r['before']=observed();r['resourcesBefore']=resources(m,resident)
  for member in r['resourcesBefore']['family']:
   if member['comm'].startswith('putgd'):
    check(p['installedBinarySha256'].get('/usr/bin/'+member['executable'])==member['executableSha256'],'live_family_executable_mismatch')
  r['beforeWitness']=shot('before');from PIL import Image
  check(Image.open(r['beforeWitness']['path']).size==(1280,800),'unsupported_viewport_preserve_failure_do_not_replay_coordinates');persist()
  if args.phase in ('inspect','shot'):pass
  elif args.phase=='configure-local':
   check(r['before']['ui']['pid']==0,'close_helper_first');patch={'actions':{a:{'destination':'local','profileId':None,'shortcut':''} for a in ('region','window','desktop','clipboard')},'archive':{'root':'/home/lab/putgd-lab/archive','keepUploads':True},'preferences':{'notifications':False,'launchAtLogin':False}}
   r['reply']=m.cli(RUN,'settings.patch',{'expectedRevision':s['metadata']['observedRevision'],'patch':patch});r['localOnlyPatch']=patch
  elif args.phase=='settings-proof':
   baseline=read(args.baseline);check(baseline['passed'] and baseline['candidatePinSha256']==sha(ROOT/'candidate.json'),'settings_baseline_wrong_candidate')
   current=observed();expected=baseline['after'];check(current['documentSha256']==expected['documentSha256'] and current['acceptedRevision']==expected['acceptedRevision'],'settings_cancel_changed_accepted_document');r['settingsDiscardProof']={'baselineReceipt':str(Path(args.baseline).resolve()),'baselineSha256':sha(args.baseline),'acceptedRevision':current['acceptedRevision'],'documentSha256':current['documentSha256']}
  elif args.phase=='show':
   check(r['before']['ui']['pid']==0,'helper_already_open');r['view']=args.view;r['reply']=m.cli(RUN,'ui.show',{'view':args.view});time.sleep(1)
   members=resources(m,resident)['family'];helpers=[x for x in members if x['executable']=='putgd-ui'];check(len(helpers)==1 and helpers[0]['executableSha256']==p['installedBinarySha256']['/usr/bin/putgd-ui'],'live_ui_binary_mismatch');r['helperIdentity']=helpers[0]
  elif args.phase=='input':
   v=visual_input();r['visualInput']=v;r['gesture']=v['gesture'];kind=v['gesture']['kind'];check(kind in ('key','click','right-click','hover'),'unsupported_gesture')
   if kind=='key':m.key(RUN,v['gesture']['keys'])
   else:
    x,y=v['gesture']['point'];check(0<=x<1280 and 0<=y<800,'input_outside_desktop')
    if kind=='right-click':
     m.pointer(RUN,x,y,1280,800);m.input_events(RUN,[{'type':'btn','data':{'button':'right','down':True}}]);time.sleep(.1);m.input_events(RUN,[{'type':'btn','data':{'button':'right','down':False}}])
    else:
     m.pointer(RUN,x,y,1280,800,kind=='click');time.sleep(.1)
     if kind=='click':m.pointer(RUN,x,y,1280,800,False)
   time.sleep(.3)
  elif args.phase=='close':
   v=visual_input();check(v['foregroundOwnedView'] is True,'inspect_foreground_owned_view_first');check(r['before']['ui']['pid']>0,'no_owned_helper');m.key(RUN,['alt','f4']);deadline=time.monotonic()+10
   while time.monotonic()<deadline:
    ui=m.cli(RUN,'ui.snapshot')['diagnostics']['ui']
    if ui['pid']==0:break
    time.sleep(.1)
   check(ui['pid']==0 and ui['lastExit']['status']=='normal' and ui['lastExit']['code']==0,'normal_helper_exit_required');r['normalExit']=ui
  elif args.phase=='matrix':
   check(r['before']['ui']['pid']==0,'close_helper_first');check(all(v['destination']=='local' for v in s['document']['actions'].values()),'local_outputs_required')
   check(len(r['resourcesBefore']['family'])==1,'drain_fixture_observers_before_matrix');r['cases']=[]
   for index,(scenario,client) in enumerate(CASES,1):
    proxy=ROOT/'r'/('n'+uuid.uuid4().hex[:8]);pre=preflight(proxy);proxy.mkdir(mode=0o700,parents=True)
    save(proxy/'state.json',p['runState']);(proxy/'identity').symlink_to(RUN/'identity');(proxy/'qmp.sock').symlink_to(RUN/'qmp.sock');(proxy/'package.json').write_bytes((RUN/'package.json').read_bytes());pre=preflight(proxy)
    check(m.verify_running_app(proxy)==resident,'resident_changed_before_case');check(source_snapshot(p['identity'])==p['sourceSnapshot'],'frozen_harness_changed')
    argv=['python3',str(SOURCE/'tools/lab/lab.py'),'run','--run',str(proxy),'--scenario',scenario,'--clipboard-client',client,'--timeout','90','--keep']
    started=now();result=subprocess.run(argv,text=True,capture_output=True,timeout=180)
    (out/f'{index:02d}.stdout').write_text(result.stdout);(out/f'{index:02d}.stderr').write_text(result.stderr)
    try:value=json.loads(result.stdout)
    except ValueError:value={'ok':False,'unparsedOutput':True}
    case={'index':index,'scenario':scenario,'client':client,'argv':argv,'startedUtc':started,'finishedUtc':now(),'exitCode':result.returncode,'socketPreflight':pre,'result':value,'proxy':str(proxy)};r['cases'].append(case);persist()
    check(result.returncode==0 and value.get('ok') is True,'native_case_failed_stop_no_retry')
    check(value['runningApp']==resident and m.verify_running_app(proxy)==resident,'resident_changed_during_case')
   r['nativeMatrixPassed']=len(r['cases'])==12
  elif args.phase=='fixture-close':
   v=visual_input();check(v['foregroundOwnedFixture'] is True,'inspect_owned_fixture_first');old=resources(m,resident)['family'];fixtures=[x for x in old if x['comm'].startswith('putgd-desktop-f') or x['script']=='gtk_clipboard_fixture.py'];check(len(fixtures)==1,'one_owned_fixture_required')
   m.key(RUN,['alt','f4']);time.sleep(.5);check(fixtures[0] not in resources(m,resident)['family'],'fixture_normal_close_failed');r['fixtureBefore']=fixtures[0];r['normalFixtureClose']='Visible focused owned fixture Alt+F4'
  elif args.phase=='quiet':
   r['resourcesAfter']=resources(m,resident);d=m.cli(RUN,'diagnostics');ui=m.cli(RUN,'ui.snapshot')['diagnostics']['ui'];check(d['activeOperations']==0 and ui['pid']==0,'helpers_or_operations_active');check(all(x['state'] in TERMINAL for x in r['resourcesAfter']['operations']+r['resourcesAfter']['deletions']),'journal_not_drained');check(r['resourcesAfter']['family']==[{'pid':resident['pid'],'comm':'putgd','startTicks':resident['startTicks'],'script':None,'executable':'putgd','executableSha256':resident['sha256']}],'resource_family_not_drained');r['noFurtherInputDuringParentMemory']=True
  r['after']=observed();r['afterWitness']=shot('after');check(m.verify_running_app(RUN)==resident,'resident_changed');r['passed']=True
 except Exception as e:
  r['error']=str(e)
  try:r['failureWitness']=shot('failure')
  except Exception:r['failureWitnessUnavailable']=True
  raise
 finally:
  r['finishedUtc']=now();persist();print(json.dumps({'receipt':str(out/'receipt.json'),'phase':args.phase,'proceduralPassed':r['passed'],'visualReview':'pending','error':r.get('error')}))

def main():
 a=argparse.ArgumentParser(description=__doc__);sub=a.add_subparsers(dest='phase',required=True)
 pf=sub.add_parser('preflight');pf.add_argument('--wayland-socket',required=True);s=sub.add_parser('source-snapshot');s.add_argument('--output',required=True)
 b=sub.add_parser('pin');b.add_argument('--package',required=True);b.add_argument('--package-sha256',required=True);b.add_argument('--source-freeze',required=True);b.add_argument('--source-freeze-sha256',required=True)
 for parser in (s,b):
  parser.add_argument('--expansion-release',required=True);parser.add_argument('--wayland-socket',required=True)
  parser.add_argument('--branch',required=True);parser.add_argument('--commit',required=True);parser.add_argument('--tree',required=True);parser.add_argument('--count',type=int,required=True);parser.add_argument('--version',required=True)
 for phase in ('inspect','shot','configure-local','settings-proof','show','input','close','matrix','fixture-close','quiet'):
  v=sub.add_parser(phase);v.add_argument('--go',required=True)
  if phase=='settings-proof':v.add_argument('--baseline',required=True)
  if phase=='show':v.add_argument('--view',choices=['settings','history','activity'],required=True)
  if phase in ('input','close','fixture-close'):v.add_argument('--visual-input',required=True)
 args=a.parse_args()
 global WAYLAND_SOCKET
 if hasattr(args,'wayland_socket'):WAYLAND_SOCKET=args.wayland_socket
 if args.phase=='preflight':print(json.dumps(preflight()))
 elif args.phase=='source-snapshot':identity=expected_identity(args);expansion_gate(args.expansion_release);save(args.output,source_snapshot(identity));print(json.dumps({'output':args.output,'sha256':sha(args.output),'graphicalExecuted':False}))
 elif args.phase=='pin':bind(args)
 else:runtime(args)
if __name__=='__main__':main()
