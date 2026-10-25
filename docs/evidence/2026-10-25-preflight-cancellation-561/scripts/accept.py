#!/usr/bin/env python3
"""Targeted #561 production resident check on a locked, visible native desktop."""
import argparse, datetime, hashlib, json, os, re, selectors, socket, struct, subprocess, sys, time, uuid, zlib
from pathlib import Path
from urllib.request import Request, build_opener, ProxyHandler
p=argparse.ArgumentParser();p.add_argument('--repo',required=True);p.add_argument('--bin',required=True);p.add_argument('--output',required=True);p.add_argument('--port',type=int,required=True);p.add_argument('--lock-session');p.add_argument('--vm-run-id');p.add_argument('--capture-rect');p.add_argument('--scale');a=p.parse_args()
repo=Path(a.repo).resolve();root=Path(a.output).resolve();binary=Path(a.bin).resolve()
sys.path[:0]=[str(repo/'tools/lab'),str(repo/'tests/performance')]
import native_acceptance as native
from record_run import session_environment
from rfb import RFB
from cover_player import coverage
from bench_ui_process import request, ProcessFamily, verify_socket_owner, EvidenceError
root.mkdir(mode=0o700);os.umask(0o077)
result={'scenario':'stalled preflight, cancel, retry','startedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'host':socket.gethostname(),'source':{},'scope':'Targeted native desktop functional regression; not release or full memory acceptance'}
children=[];resident=None;family=None;rfb=None;env=None;session_socket=str(root/'runtime/putgd/session.sock')
def save(name,value): (root/name).write_text(json.dumps(value,indent=2)+'\n')
def wait(fn,seconds=20):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        value=fn()
        if value:return value
        time.sleep(.05)
    raise RuntimeError('condition_deadline')
def line(proc,seconds=15):
    with selectors.DefaultSelector() as selector:
        selector.register(proc.stdout,selectors.EVENT_READ)
        if not selector.select(seconds):raise RuntimeError('child_readiness_deadline')
        return proc.stdout.readline().strip()
def spawn(argv,**kw):
    proc=subprocess.Popen(argv,start_new_session=True,**kw);children.append(proc);return proc

def shot(name):
    # Capture only after a host-side coverage check. Frames stay private on failure.
    path=root/(name+'-raw.png');w,h,pixels=rfb.screenshot(path)
    fraction=coverage(w,h,pixels,'bgrx')
    save(name+'-privacy.json',{'magentaFraction':fraction,'initialCaptureRectVerified':True,'size':[w,h]})
    subprocess.run(['python3',str(repo/'tools/lab/cover_player.py'),'check-frame',str(path),'--rect',','.join(map(str,capture_rect)),'--crop',str(root/(name+'.png'))],stdout=(root/(name+'-frame-gate.json')).open('w'),check=True)
    return name+'.png'
try:
    if a.vm_run_id:
        assert not a.lock_session
        result['vmRunId']=a.vm_run_id
        result['virtualization']=subprocess.check_output(['systemd-detect-virt'],text=True).strip()
        assert result['virtualization'] in ('kvm','qemu')
    else:
        lock=dict(line.split(': ',1) for line in (Path.home()/'putgd-lab/locks/desktop.lock').read_text().splitlines() if ': ' in line)
        assert lock['session']==a.lock_session
        assert datetime.datetime.now(datetime.timezone.utc)<datetime.datetime.fromisoformat(lock['expires'].replace('Z','+00:00'))
        result['lock']=lock
    facts,_=native.session();env,_=session_environment();result['session']=facts;result['qtPlatform']=env['QT_QPA_PLATFORM'];result['osRelease']=Path('/etc/os-release').read_text()
    gnome=Path(facts['shell']['exe']).name=='gnome-shell'
    capture_rect=[0,180,1920,900] if gnome else [0,0,1920,1000]
    if a.capture_rect:capture_rect=[int(v) for v in a.capture_rect.split(',')]
    assert len(capture_rect)==4
    result['captureRect']=capture_rect
    result['loadStart']=os.getloadavg()
    for command in (['plasmashell','--version'],['gnome-shell','--version'],['cinnamon','--version']):
        if Path('/usr/bin',command[0]).exists():result.setdefault('desktopVersions',{})[command[0]]=subprocess.run(command,capture_output=True,text=True,env=env).stdout.strip()
    for name in ('putgd','putgd-cli','putgd-ui'):
        result['source'][name]={'manifest':json.loads(subprocess.check_output([str(binary/name),'--version-json'])),'sha256':hashlib.sha256((binary/name).read_bytes()).hexdigest()}
    for name in ('config','data','cache','state','runtime'):(root/name).mkdir(mode=0o700)
    subprocess.run(['python3',str(repo/'tools/diagnostics/check_socket_paths.py'),'--socket',session_socket],stdout=(root/'socket-preflight.json').open('w'),check=True)
    cover=spawn(['python3',str(repo/'tools/lab/cover_player.py'),'play'],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=(root/'cover.log').open('w'),text=True)
    covered=json.loads(line(cover));assert covered.get('state')=='covered',covered
    subprocess.run(['python3',str(repo/'tools/lab/cover_player.py'),'precheck','--port',str(a.port)],env=env,stdout=(root/'cover-precheck.json').open('w'),check=False)
    preclient=RFB(a.port);w,h,pixels=preclient.frame();preclient.close()
    fraction=coverage(w,h,pixels,'bgrx',capture_rect,(255,0,255),24)
    save('capture-rect-precheck.json',{'rect':capture_rect,'fraction':fraction,'minimum':.98})
    assert fraction>=.98, 'capture_rectangle_not_covered'
    rfb=RFB(a.port);result['visuals']=[shot('covered')]
    fixture=spawn(['python3',str(repo/'tests/fixtures/s3_server.py')],stdout=subprocess.PIPE,stderr=(root/'fixture.log').open('w'),text=True)
    endpoint='http://127.0.0.1:'+line(fixture)
    def fixture_call(path,value=None):
        req=Request(endpoint+path,data=None if value is None else json.dumps(value).encode(),headers={'Content-Type':'application/json'})
        with build_opener(ProxyHandler({})).open(req,timeout=5) as response:return json.load(response)
    private_env=dict(env)
    if gnome or a.scale:private_env['QT_SCALE_FACTOR']=a.scale or '0.7'
    result['qtScaleFactor']=private_env.get('QT_SCALE_FACTOR','1')
    for key,name in [('XDG_CONFIG_HOME','config'),('XDG_DATA_HOME','data'),('XDG_CACHE_HOME','cache'),('XDG_STATE_HOME','state'),('XDG_RUNTIME_DIR','runtime')]:private_env[key]=str(root/name)
    resident=spawn([str(binary/'putgd'),'--background'],env=private_env,stdout=(root/'resident.log').open('w'),stderr=subprocess.STDOUT)
    wait(lambda:Path(session_socket).exists());family=ProcessFamily(resident.pid,binary/"putgd",binary/"putgd-ui");verify_socket_owner(session_socket,family)
    result['resident']=native.process(resident.pid)
    def call(method,params=None):return request(str(binary/'putgd-cli'),method,params,environment=private_env,socket=session_socket)
    result['capabilities']=call('capabilities')
    state=call('settings.get');doc=state['document'];profile=call('settings.developmentTemplate')['document']['profiles'][0]
    profile.update(id='preflight-fixture',label='Isolated destination cancellation fixture',credentials={'mode':'session','reference':None})
    profile['settings'].update(endpoint=endpoint,bucket='fixture',region='us-east-1',publicUrlBase=endpoint+'/fixture',objectPrefix='preflight561/',aclMode='public-read',requestTimeoutMs=120000,lowSpeedTimeSeconds=120)
    doc['profiles']=[profile];doc['preferences']['notifications']=False;doc['preferences']['launchAtLogin']=False
    for action in doc['actions'].values():
        if 'destination' in action:action.update(destination='upload',profileId=profile['id'],shortcut='')
    call('settings.patch',{'patch':doc,'expectedRevision':state['metadata']['observedRevision']})
    verify_socket_owner(session_socket,family)
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(20);connection.connect(session_socket)
        rid=str(uuid.uuid4());connection.sendall(json.dumps({'schemaVersion':1,'requestId':rid,'session':session_socket,'method':'setup.storeCredentials','params':{'profileId':profile['id'],'expectedRevision':call('settings.get')['metadata']['acceptedRevision'],'credentials':{'accessKey':'PUTGD_SYNTHETIC_ACCESS','secretKey':'putgd-synthetic-secret-not-a-real-key'}}}).encode()+b'\n')
        data=b''
        while not data.endswith(b'\n'):data+=connection.recv(65536)
        reply=json.loads(data);assert reply.get('ok') and reply['requestId']==rid
    setup=reply['result']['setupId'];job=wait(lambda:(r if (r:=call('setup.inspect',{'id':setup}))['state']!='running' else None));assert job['state']=='completed'
    call('ui.show',{'view':'settings'});time.sleep(.5);rfb.key('alt','f4');time.sleep(.5)
    # A synthetic PNG enters the production editor; only explicit submit publishes it.
    w,h=640,360
    raw=b''.join(b'\0'+bytes([25,120+(y%80),180])*w for y in range(h))
    def chunk(t,d):return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d))
    payload=root/'synthetic.png';payload.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
    fixture_call('/__rules',{'rules':[{'method':'HEAD','delay':30}]})
    opened=call('editor.open',{'source':'file','path':str(payload)});operation=opened['operationId'];result['operationId']=operation
    def editor_ready():
        try:return call('editor.inspect',{'id':operation})['phase']=='editing'
        except EvidenceError as error:
            if str(error)=='cli_editor_starting':return False
            raise
    wait(editor_ready)
    call('editor.submit',{'id':operation})
    wait(lambda:any(e['method']=='HEAD' for e in fixture_call('/__state')['events']))
    before=call('operation.inspect',{'id':operation});assert before['phase']=='preflight';save('preflight.json',before)
    call('ui.show',{'view':'history'});time.sleep(1)
    if gnome and env['QT_QPA_PLATFORM']=='xcb':
        owners=[row for row in native.resident_processes(root/'runtime') if Path(row['exe']).name=='putgd-ui']
        assert owners and all(native.same_process(row) for row in owners)
        pids={str(row['pid']) for row in owners};save('window-owners.json',owners)
        for row in subprocess.check_output(['wmctrl','-lp'],env=env,text=True).splitlines():
            fields=row.split(None,4)
            if len(fields)>=3 and fields[2] in pids:
                subprocess.run(['wmctrl','-i','-r',fields[0],'-b','remove,maximized_vert,maximized_horz'],env=env,check=True)
                subprocess.run(['wmctrl','-i','-r',fields[0],'-e','0,410,160,1100,700'],env=env,check=True)
        time.sleep(2)
    if not gnome:
        # Scope KWin geometry to authenticated private-profile helper PIDs only.
        owners=[row for row in native.resident_processes(root/'runtime') if Path(row['exe']).name=='putgd-ui']
        assert owners and all(native.same_process(row) for row in owners)
        pids=[row['pid'] for row in owners];save('window-owners.json',owners)
        geometry=('x:240,y:130,width:800,height:540' if capture_rect[2]==1280 else 'x:270,y:130,width:1000,height:650' if capture_rect[2]==1540 else 'x:410,y:130,width:1100,height:700')
        script=root/'geometry.js';script.write_text('const pids='+json.dumps(pids)+'; for (const w of workspace.windowList()) { if (pids.includes(w.pid)) { w.setMaximize(false,false); w.frameGeometry={'+geometry+'}; } }')
        script_name='i561-'+root.name
        def kwin(path,method,*args):
            return subprocess.check_output(['gdbus','call','--session','--dest','org.kde.KWin','--object-path',path,'--method',method,*args],env=env,text=True)
        loaded=kwin('/Scripting','org.kde.kwin.Scripting.loadScript',str(script),script_name)
        script_id=re.search(r'([0-9]+)',loaded).group(1)
        try:
            kwin('/Scripting/Script'+script_id,'org.kde.kwin.Script.run');time.sleep(.5)
        finally:kwin('/Scripting','org.kde.kwin.Scripting.unloadScript',script_name)
    result['uiProcesses']=family.sample()['processes']
    result['visuals'].append(shot('preflight'))
    t=time.monotonic();call('operation.cancel',{'id':operation})
    cancelled=wait(lambda:(r if (r:=call('operation.inspect',{'id':operation}))['state']=='cancelled' else None),10)
    result['cancelTerminalMs']=round((time.monotonic()-t)*1000,2);save('cancelled.json',cancelled)
    assert result['cancelTerminalMs']<5000
    time.sleep(.6);result['visuals'].append(shot('cancelled'))
    call('operation.retry',{'id':operation})
    retried=wait(lambda:(r if (r:=call('operation.inspect',{'id':operation}))['state'] in ('completed','partial','failed') else None),20)
    save('retried.json',retried);assert retried['state']=='completed'
    assert retried['items'][0]['candidate']==before['items'][0]['candidate']
    assert retried['items'][0]['uploadStatus']=='uploaded'
    time.sleep(.6);result['visuals'].append(shot('retried'))
    events=fixture_call('/__state')['events'];result['fixtureRequests']=[{'method':e['method'],'signatureValid':e['signatureValid']} for e in events]
    assert sum(e['method']=='PUT' for e in events)==1
    result['diagnostics']=call('diagnostics');result['status']='passed'
except BaseException as e:
    result['status']='failed';result['failure']=type(e).__name__+': '+str(e)
    if rfb:
        try:result.setdefault('visuals',[]).append(shot('failure'))
        except Exception as visual:result['failureVisual']=str(visual)
finally:
    if resident and resident.poll() is None:
        try:call('ui.quit');resident.wait(timeout=15)
        except Exception:pass
    if 'cover' in locals() and cover.poll() is None:
        try:cover.communicate('quit\n',timeout=5)
        except Exception:pass
    for proc in reversed(children):
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
    if rfb:rfb.close()
    result['cleanup']={'children':[{'pid':proc.pid,'returncode':proc.poll()} for proc in children], 'socketExists':Path(session_socket).exists(),'remainingProfileProcesses':native.resident_processes(root/'runtime')}
    if result['cleanup']['remainingProfileProcesses'] or result['cleanup']['socketExists']:
        result['status']='failed';result['cleanupFailure']='private resident/helper/socket remains'
    result['finishedUtc']=datetime.datetime.now(datetime.timezone.utc).isoformat();save('results.json',result)
print(json.dumps({'status':result['status'],'cancelTerminalMs':result.get('cancelTerminalMs'),'failure':result.get('failure'),'output':str(root)}))
sys.exit(0 if result['status']=='passed' else 1)
