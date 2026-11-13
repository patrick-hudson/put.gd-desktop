#!/usr/bin/env python3
"""Manual TextJob failure checklist; never ctest or a timing assertion.
Default writes a not-run plan. --execute runs on a builder using matching
runner/helper/fixture binaries, a dense <=24MP image and its known synthetic
ASCII canary file. Work and reference inputs must be outside /tmp/runtime.
No --text: receipts retain metadata/hashes, never recognized payloads.
Every new/changed regular file in the scan roots requires review, independent
of whether its bytes match the canary or an image header.
"""
import argparse,ctypes,hashlib,json,os,resource,select,signal,stat,subprocess,sys,time,struct
from pathlib import Path

# (name, fixture plan or real-helper action, allowed parent error codes)
FAILED=['text_job_failed','text_job_timeout','text_job_io','input_excessive','engine_unavailable','language_unavailable']
CASES=[('fixture-control',{'mode':'result'},['']),('real-control','none',['']),
 ('wrong-build',{'mode':'wrong-build'},['text_incompatible']),
 ('malformed-result',{'mode':'result','invalid':True},['text_incompatible']),
 ('malformed-frame','bad-frame',['text_incompatible']),
 ('output-cap',{'mode':'oversize'},['text_incompatible']),
 ('hang-deadline',{'mode':'stall'},['text_job_timeout']),
 ('child-error',{'mode':'child-error','code':'engine_unavailable'},['engine_unavailable']),
 ('crash','kill',['text_job_failed']),('cancel-by-signal','term',['text_job_failed']),
 ('limit-data','data',FAILED),('limit-cpu','cpu',['text_job_failed']),('limit-file-size','fsize',FAILED),
 ('limit-descriptors','nofile',FAILED),('corrupt-data','corrupt',['language_unavailable','engine_unavailable']),
 ('missing-data','missing',['language_unavailable']),
 ('confinement-refused','landlock',['helper_sandbox_unavailable']),('parent-killed','parent',[])]
WRAPPER='''#!/usr/bin/python3 -I
import json,os,time
from pathlib import Path
c=json.loads(Path(__file__).with_suffix('.json').read_text())
p=Path('/proc/self');s=(p/'stat').read_text().rsplit(') ',1)[1].split()
i=Path(c['identity']);tmp=i.with_suffix('.new');tmp.write_text(json.dumps(dict(pid=os.getpid(),parentPid=os.getppid(),startTicks=s[19])));os.replace(tmp,i)
while not Path(c['gate']).exists():time.sleep(.005)
if c['bad_frame']:os.write(1,bytes([255])*4)
os.execv(c['target'],[c['target']])
'''


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def identity(pid):
    try:
        fields=(Path('/proc')/str(pid)/'stat').read_text().rsplit(') ',1)[1].split()
        return dict(pid=pid,startTicks=fields[19],state=fields[0],cpuTicks=int(fields[11])+int(fields[12]))
    except FileNotFoundError:return None

def inventory(roots):
    files={};opaque=[]
    for root in roots:
        def error(e):
            try:
                if Path(e.filename).stat().st_uid==os.getuid():opaque.append(e.filename)
            except OSError:opaque.append(e.filename)
        for folder,dirs,names in os.walk(root,onerror=error,followlinks=False):
            dirs[:]=[d for d in dirs if not (Path(folder)/d).is_symlink()]
            for name in names:
                p=Path(folder)/name
                try:
                    s=p.lstat()
                    if stat.S_ISREG(s.st_mode) and (s.st_uid==os.getuid() or os.access(p,os.W_OK)):
                        files[str(p)]=(s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns)
                except FileNotFoundError:pass
    return files,opaque

def residue(roots,before,patterns):
    after,opaque=inventory(roots);changed=[];hits=[]
    for name,metadata in after.items():
        if before.get(name)==metadata:continue
        changed.append(name);p=Path(name)
        if p.name=='credentials.txt':opaque.append(name);continue
        try:
            with p.open('rb') as f:
                first=f.read(16)
                if first.startswith((b'\x89PNG\r\n\x1a\n',b'\xff\xd8\xff',b'P6\n',b'P5\n',b'BM',b'II*\0',b'MM\0*',b'PGTJ')):hits.append(dict(path=name,kind='image-or-input-header'))
                f.seek(0);tail=b'';overlap=max(map(len,patterns))-1
                while True:
                    data=f.read(2*1024*1024)
                    if not data:break
                    block=tail+data
                    if any(pattern in block for pattern in patterns):hits.append(dict(path=name,kind='canary-or-pixel-pattern'));break
                    tail=block[-overlap:]
        except FileNotFoundError:pass
        except OSError:opaque.append(name)
    return dict(changed=changed,matches=hits,unreadable=opaque)

def wait_until(check,seconds):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        value=check()
        if value:return value
        time.sleep(.01)
    raise RuntimeError('required process observation was not obtained')


def exercise(a,name,mode,expected,patterns,canary):
    d=a.work/name;d.mkdir(mode=0o700);runtime=d/'r';runtime.mkdir(mode=0o700);temp=d/'t';temp.mkdir(mode=0o700)
    sockets=[runtime/'putgd/session.sock',runtime/'bus'];lengths={str(p):len(str(p).encode()) for p in sockets}
    if max(lengths.values())>107:raise RuntimeError('resolved Unix socket path exceeds107 UTF-8 bytes')
    row=dict(case=name,status='not_covered',allowedParentErrors=expected,socketBytes=lengths)
    fixture=isinstance(mode,dict) or mode=='bad-frame';target=a.fixture if fixture else a.helper
    data=d/'data' if fixture or mode in ('missing','corrupt') else a.data
    if data!=a.data:data.mkdir(mode=0o700)
    if fixture:
        plan=dict(mode) if isinstance(mode,dict) else dict(mode='describe')
        if plan['mode']=='result':
            plan['result']=dict(text=canary,lineTops=[0])
            if plan.pop('invalid',False):plan['result']['codes']=[dict(format='INVALID',kind='text',text='x',length=1,quad=[[0,0],[1,0],[1,1],[0,1]])]
        (data/'fixture.json').write_text(json.dumps(plan))
        row['fixturePlan']=str(data/'fixture.json')
    if mode=='corrupt':(data/'eng.traineddata').write_bytes(b'not a Tesseract language file')
    wrapper=d/'child.py';wrapper.write_text(WRAPPER);wrapper.chmod(0o700)
    (d/'child.json').write_text(json.dumps(dict(identity=str(d/'child-id.json'),gate=str(d/'go'),target=str(target),bad_frame=mode=='bad-frame')))
    cmd=[str(a.runner),'--helper',str(wrapper),'--data',str(data),'--required','--lang','eng']
    cmd.append(str(a.image))
    env=os.environ|dict(QT_QPA_PLATFORM='offscreen',XDG_RUNTIME_DIR=str(runtime),TMPDIR=str(temp),XDG_CACHE_HOME=str(d/'cache'))
    for key in ('DISPLAY','WAYLAND_DISPLAY','DBUS_SESSION_BUS_ADDRESS','AT_SPI_BUS_ADDRESS'):env.pop(key,None)
    if mode=='landlock':
        cmd=['systemd-run','--user','--pipe','--wait','--collect','--quiet','--unit=text-fault-'+str(os.getpid()),'-p','SystemCallFilter=~landlock_create_ruleset landlock_add_rule landlock_restrict_self',
             '-p','UnsetEnvironment=DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS AT_SPI_BUS_ADDRESS','-p','RuntimeMaxSec=35','--setenv=XDG_RUNTIME_DIR='+str(runtime),'--setenv=TMPDIR='+str(temp),'--setenv=QT_QPA_PLATFORM=offscreen',*cmd]
    row.update(command=cmd,method=mode if isinstance(mode,str) else mode['mode'])
    roots=[Path('/tmp'),runtime,temp];before,opaque=inventory(roots);child=None;fd=None;parentfd=None;p=None;started=time.monotonic()
    try:
        p=subprocess.Popen(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
        parentfd=os.pidfd_open(p.pid)
        child=wait_until(lambda:json.loads((d/'child-id.json').read_text()) if (d/'child-id.json').exists() and (d/'child-id.json').stat().st_size else None,8)
        current=identity(child['pid']);assert current and current['startTicks']==child['startTicks']
        fd=os.pidfd_open(child['pid']);poll=select.poll();poll.register(fd,select.POLLIN)
        status=(Path('/proc')/str(child['pid'])/'status').read_text();base_filters=int(next(l.split(':')[1] for l in status.splitlines() if l.startswith('Seccomp_filters:')))
        row['child']=child;row['parent']=identity(child['parentPid'])
        if mode=='parent' and child['parentPid']!=p.pid:raise RuntimeError('parent identity differs from the owned runner')
        row['threadsBeforeExec']=[identity(int(t.name)) for t in (Path('/proc')/str(child['pid'])/'task').iterdir()]
        (d/'go').touch()
        if mode in ('kill','term','parent','data','cpu','fsize','nofile'):
            def confined():
                v=identity(child['pid'])
                if not v or v['startTicks']!=child['startTicks']:raise RuntimeError('helper exited before injection')
                s=(Path('/proc')/str(child['pid'])/'status').read_text();n=int(next(l.split(':')[1] for l in s.splitlines() if l.startswith('Seccomp_filters:')))
                return v if n>base_filters else None
            v=wait_until(confined,4)
            if mode not in ('data','nofile'):
                required_ticks=int(os.sysconf('SC_CLK_TCK')*1.1) if mode=='cpu' else os.sysconf('SC_CLK_TCK')//4
                v=wait_until(lambda: (v if (v:=identity(child['pid'])) and v['cpuTicks']>=required_ticks else None),4)
            row['atInjection']=v;row['threadsAtInjection']=[identity(int(t.name)) for t in (Path('/proc')/str(child['pid'])/'task').iterdir()]
            if mode in ('kill','term'):signal.pidfd_send_signal(fd,signal.SIGKILL if mode=='kill' else signal.SIGTERM)
            elif mode=='parent':signal.pidfd_send_signal(parentfd,signal.SIGKILL)
            else:
                if time.monotonic()-started>12:raise RuntimeError('insufficient parent deadline margin for stopped-child prlimit')
                signal.pidfd_send_signal(fd,signal.SIGSTOP)
                try:
                    wait_until(lambda:(v if (v:=identity(child['pid'])) and v['state'] in ('T','t') else None),1)
                    limit,value={'data':(resource.RLIMIT_DATA,32*1024*1024),'cpu':(resource.RLIMIT_CPU,1),'fsize':(resource.RLIMIT_FSIZE,1),'nofile':(resource.RLIMIT_NOFILE,5)}[mode]
                    old=resource.prlimit(child['pid'],limit,(value,value));row['prlimit']=dict(resource=mode,before=old,after=resource.prlimit(child['pid'],limit),method='lower after helper confinement; its startup would otherwise reset inherited limits')
                finally:signal.pidfd_send_signal(fd,signal.SIGCONT)
        stdout,stderr=p.communicate(timeout=30);row.update(parentExit=p.returncode,stderrBytes=len(stderr),stderrSha256=hashlib.sha256(stderr).hexdigest())
        if mode!='parent':
            answer=json.loads(stdout);row['parentError']=answer['error']
            if answer['error'] not in expected:raise RuntimeError('unexpected parent error, including success when fault was intended')
            if name=='fixture-control' and answer['chars']!=len(canary):raise RuntimeError('fixture control did not return its canned result')
        else:row['parentError']=None
        row['pidfdExitObserved']=bool(poll.poll(5000))
        try:os.waitpid(child['pid'],os.WNOHANG)
        except ChildProcessError:pass
        wait_until(lambda:not identity(child['pid']),5);row['childAndThreadsGone']=True
        if not row['pidfdExitObserved']:raise RuntimeError('helper exit was not observed')
        row['status']='measured'
    except Exception as e:row['reason']=str(e)
    finally:
        if p and p.poll() is None:
            try:
                if parentfd is not None:signal.pidfd_send_signal(parentfd,signal.SIGKILL)
                else:p.kill()
            except ProcessLookupError:pass
            if mode=='landlock':subprocess.run(['systemctl','--user','stop','text-fault-'+str(os.getpid())],capture_output=True,timeout=10)
            p.communicate(timeout=5);row['forcedParentCleanup']=True;row['status']='not_covered'
        if fd is not None:
            if child and (v:=identity(child['pid'])) and v['startTicks']==child['startTicks']:
                try:signal.pidfd_send_signal(fd,signal.SIGKILL)
                except ProcessLookupError:pass
                try:os.waitpid(child['pid'],os.WNOHANG)
                except ChildProcessError:pass
                try:wait_until(lambda:not identity(child['pid']),5)
                except RuntimeError:row['cleanupIncomplete']=True
                row['forcedChildCleanup']=True;row['status']='not_covered'
            os.close(fd)
        if parentfd is not None:os.close(parentfd)
        row['residue']=residue(roots,before,patterns)
        if opaque or row['residue']['changed'] or row['residue']['unreadable']:row['status']='requires_review'
    return row


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for arg in ('runner','helper','fixture','image','data','canary-file','work'):p.add_argument('--'+arg,type=Path)
    p.add_argument('--execute',action='store_true');p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    receipt=dict(status='not_run',cases=[dict(case=n,method=m,allowedParentErrors=e) for n,m,e in CASES],limits=['SIGTERM tests external termination; cooperative TextJob Cancel needs a driver cancel handle.','Every new/changed regular file under /tmp and private runtime/temp requires human review, even if it has no image header or exact canary match. This also catches unknown pixel/text encodings.'])
    with a.output.open('x') as out:
        if a.execute:
            for arg in ('runner','helper','fixture','image','data','canary_file','work'):
                if getattr(a,arg) is None:p.error('--execute requires --'+arg.replace('_','-'))
                setattr(a,arg,getattr(a,arg).resolve())
            if a.work.is_relative_to('/tmp') or a.image.is_relative_to('/tmp') or a.canary_file.is_relative_to('/tmp'):raise ValueError('work and references must be outside /tmp')
            a.work.mkdir(mode=0o700)
            if a.canary_file.name=='credentials.txt':raise ValueError('credentials.txt is never a canary source')
            canary=a.canary_file.read_text().strip()
            if len(canary)<24 or not canary.isascii() or not all(c.isprintable() for c in canary):raise ValueError('use a known synthetic printable ASCII canary of at least24 characters')
            with a.image.open('rb') as source:header=source.read(24)
            if header[:8]!=b'\x89PNG\r\n\x1a\n' or len(header)!=24:raise ValueError('use a corpus PNG')
            width,height=struct.unpack('>II',header[16:24])
            if width*height>24000000:raise ValueError('failure input exceeds24MP admission cap')
            patterns=[canary.encode()]
            resource.setrlimit(resource.RLIMIT_CORE,(0,0))
            if ctypes.CDLL(None,use_errno=True).prctl(36,1,0,0,0):raise OSError('cannot become child subreaper')
            receipt.update(status='incomplete',identities={str(f):sha(f) for f in (a.runner,a.helper,a.fixture,a.image,a.canary_file,a.data/'eng.traineddata')},cases=[])
            controls={'fixture':False,'real':False}
            for name,mode,expected in CASES:
                category='fixture' if isinstance(mode,dict) or mode=='bad-frame' else 'real'
                if name.endswith('-control') or controls[category]:row=exercise(a,name,mode,expected,patterns,canary)
                else:row=dict(case=name,status='not_covered',reason=category+' control was not measured; do not misattribute its failure to this fault')
                if name.endswith('-control'):controls[category]=row['status']=='measured'
                receipt['cases'].append(row);out.seek(0);json.dump(receipt,out,indent=2);out.truncate();out.flush()
            if any(sha(Path(f))!=digest for f,digest in receipt['identities'].items()):raise RuntimeError('input or binary changed during campaign')
            if all(c['status']=='measured' for c in receipt['cases']):receipt['status']='measured'
        out.seek(0);json.dump(receipt,out,indent=2);out.truncate()
    return 0 if receipt['status']=='measured' else 2
if __name__=='__main__':raise SystemExit(main())
