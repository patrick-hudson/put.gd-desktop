#!/usr/bin/env python3
import collections, hashlib, json, re, shlex, subprocess, sys, time
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/lab'))
import lab

import argparse, uuid
parser=argparse.ArgumentParser(description='Synthetic post-use RSS and idle heap cleanup diagnostic on a disposable Cinnamon VNC lab; no cloud calls or physical budget claim')
for name in ('run','app','cli','fixture','output'):
    parser.add_argument('--'+name,required=True)
parser.add_argument('--cycles',type=int,default=5)
parser.add_argument('--settle-seconds',type=float,default=5)
args=parser.parse_args()
if args.cycles<2 or args.cycles>100 or args.settle_seconds<3 or args.settle_seconds>60:
    parser.error('cycles must be2..100 and settle-seconds3..60')
RUN=Path(args.run).resolve(strict=True)
info=lab.state(RUN)
if info['profile']!='cinnamon-x11' or info['guest_user']!='lab':
    parser.error('requires a disposable Cinnamon lab with its dedicated lab user')
APP,CLI,FIXTURE=[Path(value).resolve(strict=True) for value in (args.app,args.cli,args.fixture)]
EVIDENCE=Path(args.output).resolve()
EVIDENCE.mkdir(mode=0o700,exist_ok=False)
GUEST='/home/lab/putgd-lab/payload-memory-'+uuid.uuid4().hex[:12]

results = {'schemaVersion': 1, 'scope': 'Production post-use allocator reclamation diagnostic; synthetic local actions on disposable Cinnamon VNC lab; not physical cold-idle acceptance',
           'profile': 'cinnamon-x11', 'vncPort': 5900+info['vnc_display'], 'sshPort': info['ssh_port'], 'ok': False,
           'cycles': [], 'samples': [], 'protocol': {'coldSeconds': 5, 'settleSeconds': args.settle_seconds,
           'cycleCount': args.cycles, 'desktopSize': [1280, 800], 'clipboardImageSize': [96, 64],
           'trimControl': 'production one-shot quiet-period reclamation; no temporary hooks', 'guestRoot': GUEST}}

def save(name, value):
    (EVIDENCE / name).write_text(json.dumps(value, indent=2) + '\n')

def cli(method, params=None):
    command = shlex.quote(GUEST + '/bin/putgd-cli') + ' ' + shlex.quote(method) + ' ' + shlex.quote(json.dumps(params or {}))
    value = json.loads(lab.guest_session(RUN, command + '\n'))
    if not value.get('ok'):
        raise RuntimeError('Probe CLI error: ' + value.get('error', {}).get('code', 'unknown'))
    return value['result']

def wait_json(remote, name, predicate):
    end = time.monotonic() + 15
    while time.monotonic() < end:
        try:
            lab.get(RUN, remote, EVIDENCE / name)
            value = json.loads((EVIDENCE / name).read_text())
            if predicate(value):
                return value
        except lab.LabError:
            pass
        time.sleep(.1)
    raise RuntimeError('Synthetic fixture acknowledgement timed out: ' + name)

def witness(name):
    value = lab.screenshot(RUN, 'payload-allocator-' + name)
    picture = Image.open(value['path'])
    picture.save(EVIDENCE / (name + '.png'))
    save(name + '-witness.json', value)
    return picture

def tasks():
    script = '''python3 - <<'GUESTPY'
import json,pathlib
pid=int(pathlib.Path('/home/lab/putgd-lab/payload-allocator/app.pid').read_text())
print(json.dumps({str(p.name):p.joinpath('comm').read_text().strip() for p in sorted(pathlib.Path('/proc',str(pid),'task').iterdir())}))
GUESTPY
'''
    return json.loads(lab.ssh(RUN, script.replace('/home/lab/putgd-lab/payload-allocator',GUEST)))

def drained():
    end = time.monotonic() + 15
    while time.monotonic() < end:
        recent = cli('operation.recent')['operations']
        assert all(op['state'] in ('completed', 'cancelled', 'failed', 'partial') for op in recent), 'Probe operation still active'
        time.sleep(.15)
        current = tasks()
        if current == cold_tasks:
            return current
        time.sleep(.1)
    raise RuntimeError('Transient probe threads did not return to the cold task set')

def sample(label, revision):
    memory=cli('diagnostics')['memory']
    assert memory['allocatorReclamationSupported'] and not memory['allocatorReclamationPending'], 'Reclamation has not settled'
    script = '''python3 - <<'GUESTPY'
import collections,json,os,pathlib,re
pid=int(pathlib.Path('/home/lab/putgd-lab/payload-allocator/app.pid').read_text())
root=pathlib.Path('/proc',str(pid))
assert pathlib.Path(os.readlink(root/'exe')).name=='putgd'
groups=collections.defaultdict(lambda:collections.defaultdict(int))
keys={'Size','Rss','Pss','Shared_Clean','Shared_Dirty','Private_Clean','Private_Dirty','Anonymous','Swap','Locked'}
name='[anonymous]'
for line in (root/'smaps').read_text().splitlines():
 if re.match(r'^[0-9a-f]+-[0-9a-f]+ ',line):
  parts=line.split(None,5);name=pathlib.Path(parts[5]).name if len(parts)==6 else '[anonymous]'
 elif ':' in line:
  key,value=line.split(':',1)
  if key in keys:groups[name][key+'KiB']+=int(value.split()[0])
statm=list(map(int,(root/'statm').read_text().split()))
print(json.dumps({'pid':pid,'uid':root.stat().st_uid,'pageBytes':os.sysconf('SC_PAGE_SIZE'),'statmPages':statm,
 'rssBytes':statm[1]*os.sysconf('SC_PAGE_SIZE'),'smapsByBasename':groups,
 'smapsTotals':{key:sum(group.get(key,0) for group in groups.values()) for key in {k for group in groups.values() for k in group}},
 'tasks':{str(p.name):p.joinpath('comm').read_text().strip() for p in sorted((root/'task').iterdir())}}))
GUESTPY
'''
    measured = json.loads(lab.ssh(RUN, script.replace('/home/lab/putgd-lab/payload-allocator',GUEST)))
    assert measured['tasks'] == cold_tasks, 'Sample includes transient worker threads'
    value = {'label': label, 'revision': revision, 'memory': memory, 'process': measured}
    results['samples'].append(value)
    save('sample-' + label + '.json', value)
    save('results.json', results)
    print(json.dumps({'sample': label, 'rssMiB': measured['rssBytes'] / 1048576, 'memory': memory}), flush=True)
    return value

def terminal(operation_id):
    end = time.monotonic() + 20
    while time.monotonic() < end:
        op = cli('operation.inspect', {'id': operation_id})
        if op['state'] in ('completed','cancelled','failed','partial') and op.get('copyStatus') in ('copied','no_result','failed','suppressed'):
            assert op['state'] == 'completed' and op['copyStatus'] == 'copied', 'Synthetic local action did not complete/copy'
            return op
        time.sleep(.1)
    raise RuntimeError('Synthetic local operation did not settle')

def action(kind, cycle, expected_size):
    accepted = cli('action.invoke', {'action': kind})
    operation_id = accepted['operationId']
    op = terminal(operation_id)
    save(f'cycle-{cycle}-{kind}-operation.json', op)
    assert len(op['items']) == 1 and op['items'][0]['uploadStatus'] == 'local_only'
    target = EVIDENCE / f'cycle-{cycle}-{kind}-artifact.png'
    lab.get(RUN, op['items'][0]['local']['path'], target)
    picture = Image.open(target)
    assert picture.size == expected_size, 'Unexpected synthetic image geometry'
    if kind == 'clipboard':
        assert picture.convert('RGB').getcolors() == [(6144, (220,30,40))]
    else:
        for position, rgb in [((10,10),(220,30,40)),((330,10),(20,180,70)),((10,180),(35,70,220)),((330,180),(235,190,20)),((1200,700),(255,255,255))]:
            assert picture.convert('RGB').getpixel(position) == rgb, 'Desktop artifact did not contain the full synthetic fixture'
    return {'action': kind, 'operationId': operation_id, 'width': picture.width, 'height': picture.height,
            'state': op['state'], 'copyStatus': op['copyStatus'], 'artifactSha256': hashlib.sha256(target.read_bytes()).hexdigest()}

original = lab.cli(RUN, 'settings.get')
original_revision = original['metadata']['acceptedRevision']
assert all(op['state'] in ('completed','cancelled','failed','partial') for op in lab.cli(RUN, 'operation.recent')['operations'])
results['originalConfigRevision'] = original_revision
document = lab.cli(RUN,'settings.developmentTemplate')['document']
document['profiles']=[]
document['actions']={key:{'destination':'local','profileId':None,'shortcut':''} for key in document['actions']}
document['preferences']={'notifications':False,'launchAtLogin':False}
assert document['profiles'] == [] and all(value['destination'] == 'local' and value['profileId'] is None for value in document['actions'].values())
document['archive']['root'] = GUEST + '/archive'
save('isolated-config.json', document)
identity={'baseRevision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
          'scope':'Production Release binaries with uncommitted source hashes; no temporary allocator instrumentation',
          'files':{name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in [('putgd',APP),('putgd-cli',CLI),('fixture',FIXTURE)]},
          'source':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in ['src/app/OperationService.cpp','src/app/OperationService.h','src/main.cpp','CMakeLists.txt']},
          'controllerSha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'command':sys.argv}
save('source-build-identity.json', identity)
(EVIDENCE / 'controller.py').write_bytes(Path(__file__).read_bytes())
started = False
try:
    lab.ssh(RUN, 'mkdir -p ' + GUEST + '/bin ' + GUEST + '/config/putgd/putgd ' + GUEST + '/fixture ' + GUEST + '/archive\n')
    for source, name in [(APP,'putgd'),(CLI,'putgd-cli'),(FIXTURE,'putgd-desktop-fixture')]:
        lab.put(RUN, source, GUEST + '/bin/' + name)
    lab.put(RUN, EVIDENCE/'isolated-config.json', GUEST+'/config/putgd/putgd/config.json')
    launch = '''set -eu
pkill -u lab -x putgd || true
for attempt in $(seq 1 100); do if ! pgrep -u lab -x putgd >/dev/null; then break; fi; sleep 0.05; done
export XDG_CONFIG_HOME=/home/lab/putgd-lab/payload-allocator/config
export XDG_DATA_HOME=/home/lab/putgd-lab/payload-allocator/data
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/putgd
chmod 755 /home/lab/putgd-lab/payload-allocator/bin/*
nohup /home/lab/putgd-lab/payload-allocator/bin/putgd --background > /home/lab/putgd-lab/payload-allocator/app.log 2>&1 < /dev/null &
echo $! > /home/lab/putgd-lab/payload-allocator/app.pid
nohup /home/lab/putgd-lab/payload-allocator/bin/putgd-desktop-fixture --output /home/lab/putgd-lab/payload-allocator/fixture > /home/lab/putgd-lab/payload-allocator/fixture.log 2>&1 < /dev/null &
echo $! > /home/lab/putgd-lab/payload-allocator/fixture.pid
'''
    launch=launch.replace('/home/lab/putgd-lab/payload-allocator',GUEST)
    (EVIDENCE/'launch-guest.sh').write_text(launch)
    started = True
    lab.guest_session(RUN, launch)
    ready = wait_json(GUEST+'/fixture/ready.json', 'fixture-ready.json', lambda value: value.get('qt_platform') == 'xcb')
    end = time.monotonic()+15
    while True:
        try:
            metadata = cli('settings.get')['metadata']
            assert metadata['path'].startswith(GUEST) and metadata['validation']['ok'] and metadata['acceptedRevision']
            capabilities = cli('capabilities')
            break
        except (lab.LabError,AssertionError,RuntimeError):
            if time.monotonic()>end:raise
            time.sleep(.1)
    save('active-config-metadata.json', metadata)
    save('capabilities.json', capabilities)
    results['nativeVersions'] = {'qt': ready['qt_version'], 'desktop': ready['desktop'], 'session': ready['session'],
        'packages': lab.ssh(RUN, "dpkg-query -W -f='${Package} ${Version}\\n' cinnamon muffin 'libqt6*' libc6\n").splitlines()}
    # Use the actual compositor's public EWMH fullscreen request. No product/fixture code changes.
    fullscreen = '''python3 - <<'GUESTPY'
import ctypes,pathlib,re,subprocess
tree=subprocess.check_output(['xwininfo','-root','-tree'],text=True)
line=next(line for line in tree.splitlines() if 'put.gd independent synthetic paint / paste fixture' in line)
wid=int(re.search(r'0x[0-9a-f]+',line).group(0),16)
class Data(ctypes.Union):_fields_=[('b',ctypes.c_char*20),('s',ctypes.c_short*10),('l',ctypes.c_long*5)]
class Message(ctypes.Structure):_fields_=[('type',ctypes.c_int),('serial',ctypes.c_ulong),('send_event',ctypes.c_int),('display',ctypes.c_void_p),('window',ctypes.c_ulong),('message_type',ctypes.c_ulong),('format',ctypes.c_int),('data',Data)]
class Event(ctypes.Union):_fields_=[('client',Message),('pad',ctypes.c_long*24)]
x=ctypes.CDLL('libX11.so.6');x.XOpenDisplay.restype=ctypes.c_void_p;x.XDefaultRootWindow.argtypes=[ctypes.c_void_p];x.XDefaultRootWindow.restype=ctypes.c_ulong
x.XInternAtom.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_int];x.XInternAtom.restype=ctypes.c_ulong;x.XSendEvent.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_int,ctypes.c_long,ctypes.POINTER(Event)];x.XFlush.argtypes=[ctypes.c_void_p]
d=x.XOpenDisplay(None);event=Event();event.client.type=33;event.client.send_event=1;event.client.display=d;event.client.window=wid;event.client.message_type=x.XInternAtom(d,b'_NET_WM_STATE',0);event.client.format=32;event.client.data.l[0]=1;event.client.data.l[1]=x.XInternAtom(d,b'_NET_WM_STATE_FULLSCREEN',0);event.client.data.l[3]=1
assert x.XSendEvent(d,x.XDefaultRootWindow(d),0,(1<<20)|(1<<19),ctypes.byref(event));x.XFlush(d)
pathlib.Path('/home/lab/putgd-lab/payload-allocator/fixture.xid').write_text(hex(wid))
GUESTPY
'''
    fullscreen=fullscreen.replace('/home/lab/putgd-lab/payload-allocator',GUEST)
    (EVIDENCE/'fullscreen-guest.py.sh').write_text(fullscreen)
    lab.guest_session(RUN, fullscreen)
    end=time.monotonic()+10
    while True:
        native=lab.guest_session(RUN, 'xwininfo -id $(cat '+GUEST+'/fixture.xid)\n')
        if all(text in native for text in ['Absolute upper-left X:  0','Absolute upper-left Y:  0','Width: 1280','Height: 800','Map State: IsViewable']):break
        if time.monotonic()>end:raise RuntimeError('Synthetic fixture did not cover the desktop')
        time.sleep(.1)
    (EVIDENCE/'fullscreen-native-geometry.txt').write_text(native)
    lab.pointer(RUN,150,200,1280,800,True);time.sleep(.05);lab.pointer(RUN,150,200,1280,800,False)
    picture=witness('fullscreen-synthetic-fixture')
    assert picture.size==(1280,800)
    for position,rgb in [((10,10),(220,30,40)),((330,10),(20,180,70)),((10,180),(35,70,220)),((330,180),(235,190,20)),((1200,700),(255,255,255))]:
        assert picture.convert('RGB').getpixel(position)==rgb, 'Synthetic fullscreen fixture is not visible'
    time.sleep(5)
    cold_tasks=tasks()
    results['coldTasks']=cold_tasks
    sample('cold',0)
    for cycle in range(1,args.cycles+1):
        previous_reclaims=cli('diagnostics')['memory']['allocatorReclamationCount']
        desktop=action('desktop',cycle,(1280,800))
        lab.ssh(RUN,'rm -f '+GUEST+'/fixture/source.json\n')
        lab.key(RUN,['ctrl','alt','shift','i'])
        wait_json(GUEST+'/fixture/source.json',f'cycle-{cycle}-fixture-source.json',lambda value:value.get('synthetic') and value.get('key')==73)
        clipboard=action('clipboard',cycle,(96,64))
        complete={'cycle':cycle,'operations':[desktop,clipboard],'drainedTasks':drained()}
        time.sleep(args.settle_seconds)
        complete['sample']=sample('cycle-'+str(cycle),cycle)
        assert complete['sample']['memory']['allocatorReclamationCount']>previous_reclaims, 'Completed image actions did not trigger automatic idle reclamation'
        results['cycles'].append(complete)
        save('results.json',results)
    sample('final-drained-idle',args.cycles+1)
    results['summary']={'coldRssBytes':results['samples'][0]['process']['rssBytes'],'finalDrainedRssBytes':results['samples'][-1]['process']['rssBytes'],'cycleRssBytes':[item['sample']['process']['rssBytes'] for item in results['cycles']],'maxObservedReclamationUsec':max(item['memory']['lastAllocatorReclamationUsec'] for item in results['samples'])}
    witness('final-fullscreen-synthetic-fixture')
    results['ok']=True
except Exception as error:
    results['error']=type(error).__name__+': '+str(error)
    raise
finally:
    try:
        if started:
            input_error = None
            try:
                lab.input_events(RUN,[{'type':'btn','data':{'button':'left','down':False}}])
            except Exception as error:
                results['ok'] = False
                results['inputCleanupErrorType'] = type(error).__name__
                input_error = error
            results['restoredConfig'] = False
            try:
                restore='''set -eu
pkill -u lab -x putgd || true
for attempt in $(seq 1 100); do if ! pgrep -u lab -x putgd >/dev/null; then break; fi; sleep 0.05; done
pkill -u lab -f '^/home/lab/putgd-lab/payload-allocator/bin/putgd-desktop-fixture ' || true
nohup putgd-launcher --background > /home/lab/putgd-lab/app.log 2>&1 < /dev/null &
'''
                lab.guest_session(RUN,restore.replace('/home/lab/putgd-lab/payload-allocator',GUEST))
                end=time.monotonic()+10
                while True:
                    try:
                        restored=lab.cli(RUN,'settings.get')
                        if restored['metadata']['acceptedRevision']==original_revision:
                            results['restoredConfig']=True;break
                    except lab.LabError:pass
                    if time.monotonic()>end:raise RuntimeError('Original resident/config did not restore')
                    time.sleep(.1)
            except Exception as error:
                results['ok'] = False
                results['restoreErrorType'] = type(error).__name__
                raise
            if input_error is not None:
                raise input_error
    finally:
        save('results.json',results)
        print(json.dumps({key:value for key,value in results.items() if key not in ('samples','cycles')},indent=2),flush=True)
