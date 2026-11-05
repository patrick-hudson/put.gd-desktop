#!/usr/bin/env python3
"""#559 production editor/History burst on a locked, visible native desktop.
Only synthetic PNGs and a loopback S3 fixture. No real credentials are read.
Run: queued_burst.py SOURCE BUILD NEW_SHORT_RUN VNC_PORT
"""
import hashlib,json,os,socket,struct,subprocess,sys,time,urllib.request,zlib
from pathlib import Path
source,build,out=map(lambda p:Path(p).resolve(),sys.argv[1:4]); port=int(sys.argv[4])
sys.path.insert(0,str(source/'tools/lab'))
import native_acceptance as native
from rfb import RFB
sys.path.insert(0,str(source/'tests/performance'))
from bench_ui_process import ProcessFamily
os.umask(0o077);out.mkdir(mode=0o700)
report={'scenario':'submitted-editor-queued-burst-559','passed':False,'source':'9f382fd0b07d7c1237d8a47a25b87ee3d6129535','run':str(out),'startedUtc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'checks':{}}
def save(name,value): (out/name).write_text(json.dumps(value,indent=2)+'\n')
def wait(fn,seconds=30):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        value=fn()
        if value:return value
        time.sleep(.15)
    raise RuntimeError('bounded wait expired')
def check(name,condition):
    report['checks'][name]=bool(condition)
    if not condition:raise RuntimeError('check failed: '+name)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
runtime=out/'r';sock=runtime/'putgd/session.sock'
pre=subprocess.run([sys.executable,str(source/'tools/diagnostics/check_socket_paths.py'),'--socket',str(sock)],capture_output=True,text=True,check=True)
save('socket-preflight.json',json.loads(pre.stdout))
facts,env=native.session();save('session.json',facts)
report['host']=socket.gethostname();report['osRelease']=Path('/etc/os-release').read_text();report['session']=facts
for name in ['r','config','data','cache','state']:(out/name).mkdir(mode=0o700)
appenv=dict(env,XDG_RUNTIME_DIR=str(runtime),XDG_CONFIG_HOME=str(out/'config'),XDG_DATA_HOME=str(out/'data'),XDG_CACHE_HOME=str(out/'cache'),XDG_STATE_HOME=str(out/'state'),PIPEWIRE_RUNTIME_DIR=env['XDG_RUNTIME_DIR'])
report['build']=json.loads(subprocess.check_output([str(build/'putgd'),'--version-json'],env=appenv))
report['qtVersion']=subprocess.check_output(['pkg-config','--modversion','Qt6Core'],text=True).strip()
report['binaries']={name:sha(build/name) for name in ['putgd','putgd-cli','putgd-ui']}
fixture=cover=resident=None;rfb=None
calls=[]
def cli(method,params=None,allow=False):
    cp=subprocess.run([str(build/'putgd-cli'),'--socket',str(sock),method,json.dumps(params or {})],env=appenv,capture_output=True,text=True,timeout=25)
    value=json.loads(cp.stdout)
    calls.append({'method':method,'params':params or {},'response':value})
    if not allow and not value.get('ok'):raise RuntimeError(method+': '+value.get('error',{}).get('code','failed'))
    return value.get('result') if value.get('ok') else None

def wire(method,params):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(25);s.connect(str(sock));s.sendall(json.dumps({'schemaVersion':1,'requestId':'559-setup','session':str(sock),'method':method,'params':params}).encode()+b'\n')
        data=b''
        while not data.endswith(b'\n'):data+=s.recv(65536)
    result=json.loads(data)
    if not result.get('ok'):raise RuntimeError('synthetic credential setup refused')
    return result['result']

def http(path,body=None):
    data=None if body is None else json.dumps(body).encode()
    return json.load(urllib.request.urlopen(urllib.request.Request(endpoint+path,data=data),timeout=10))
def shot(name):
    rfb.pointer(rfb.width-30,30);time.sleep(.3)
    path=out/(name+'.png');rfb.screenshot(str(path))
    # Full-screen cover behind the app keeps other desktop content out of exports.
    gate=subprocess.run([sys.executable,str(source/'tools/lab/cover_player.py'),'check-frame',str(path),'--min-fraction','0.5'],env=env,capture_output=True,text=True)
    save(name+'-privacy-gate.json',{'exit':gate.returncode,'stdout':gate.stdout,'stderr':gate.stderr})
    check(name+'Privacy',gate.returncode==0)
    report.setdefault('visuals',[]).append({'file':path.name,'sha256':sha(path)})
try:
    # Cover keeps the existing session and all its windows intact.
    cover=subprocess.Popen([sys.executable,str(source/'tools/lab/cover_player.py'),'play','--title','put.gd #559 synthetic acceptance'],env=env,stdin=subprocess.PIPE,stdout=(out/'cover.log').open('w'),stderr=subprocess.STDOUT,text=True)
    wait(lambda:'"covered"' in (out/'cover.log').read_text(),15)
    gate=subprocess.run([sys.executable,str(source/'tools/lab/cover_player.py'),'precheck','--port',str(port)],env=env,capture_output=True,text=True)
    save('cover-precheck.json',{'exit':gate.returncode,'stdout':gate.stdout,'stderr':gate.stderr});check('coverPrecheck',gate.returncode==0)
    rfb=RFB(port);shot('00-covered')
    fixture=subprocess.Popen([sys.executable,str(source/'tests/fixtures/s3_server.py')],env=dict(os.environ,PUTGD_TEST_TIME_SCALE='20'),stdout=subprocess.PIPE,stderr=(out/'fixture.log').open('w'),text=True)
    endpoint='http://127.0.0.1:'+fixture.stdout.readline().strip()
    resident=subprocess.Popen([str(build/'putgd'),'--background'],env=appenv,stdout=(out/'resident.log').open('w'),stderr=subprocess.STDOUT)
    wait(lambda:sock.exists());wait(lambda:cli('capabilities',allow=True))
    family=ProcessFamily(resident.pid,build/'putgd',build/'putgd-ui')
    report['memoryBefore']=family.sample()
    save('diagnostics.json',cli('diagnostics'))
    cfg=cli('settings.get');doc=cfg['document']
    doc['profiles']=[{'id':'q559','label':'Synthetic queued burst','provider':'s3','credentials':{'mode':'session','reference':None},'settings':{'bucket':'fixture','region':'us-east-1','endpoint':endpoint,'objectPrefix':'q559/','storageClass':'STANDARD','aclMode':'public-read','publicUrlBase':endpoint+'/fixture','addressing':'path','connectTimeoutMs':5000,'requestTimeoutMs':180000,'lowSpeedBytesPerSecond':1,'lowSpeedTimeSeconds':120}}]
    for a in doc['actions'].values():a.update(destination='upload',profileId='q559',shortcut='')
    doc['archive']['keepUploads']=False;doc['archive']['root']=str(out/'archive')
    doc['limits']['concurrentUploads']=1;doc['limits']['pendingOperations']=12
    doc['preferences']['notifications']=False;doc['preferences']['uploadClipboardBehavior']='list'
    # Only configuration references are written; fixture-only key material stays in memory.
    (out/'config/putgd/config.json').write_text(json.dumps(doc));cli('settings.reload')
    cfg=cli('settings.get')
    setup=wire('setup.storeCredentials',{'profileId':'q559','expectedRevision':cfg['metadata']['acceptedRevision'],'credentials':{'accessKey':'PUTGD_SYNTHETIC_ACCESS','secretKey':'putgd-synthetic-secret-not-a-real-key'}})
    settled=wait(lambda:(lambda x:x if x.get('state')!='running' else None)(cli('setup.inspect',{'id':setup['setupId']})))
    save('setup-result.json',settled)
    w,h=320,200;raw=b''.join(b'\0'+b''.join(bytes((x*255//w,y*255//h,160)) for x in range(w)) for y in range(h))
    def chunk(t,d):return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d))
    png=out/'synthetic.png';png.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
    http('/__rules',{'rules':[{'method':'PUT','barrier':'queued-burst','remaining':1}]})
    ids=[]
    for n in range(6):
        eid=cli('editor.open',{'source':'file','path':str(png)})['operationId'];ids.append(eid)
        wait(lambda:(lambda x:x if x and x.get('phase')=='editing' else None)(cli('editor.inspect',{'id':eid},allow=True)))
        if n==0:shot('01-editor')
        cli('editor.submit',{'id':eid})
        wait(lambda:not cli('capabilities')['editor']['open'])
        if n==0:wait(lambda:any(e['method']=='PUT' for e in http('/__state')['events']))
        else:wait(lambda:cli('operation.inspect',{'id':eid})['phase']=='queued')
    queued=[cli('operation.inspect',{'id':eid}) for eid in ids[1:]]
    check('fiveSubmittedQueued',len(queued)==5 and all(x['state']=='acquiring' and x['phase']=='queued' and not x['items'] for x in queued))
    check('editorClosed',not cli('capabilities')['editor']['open'])
    save('queued-operations.json',queued);save('queued-recent.json',cli('operation.recent'))
    import sqlite3
    with sqlite3.connect(out/'data/putgd/history.sqlite') as db:
        rows=[json.loads(row[0]) for row in db.execute('SELECT data FROM operations')]
    save('queued-journal.json',rows);check('persistedHandoff',all(next(x for x in rows if x['id']==eid)['state']=='acquiring' for eid in ids[1:]))
    cli('ui.show',{'view':'history'});time.sleep(2);shot('02-queued-history')
    report['memoryQueued']=family.sample()
    # Cancel exactly the final queued operation; the other submissions must drain.
    cli('operation.cancel',{'id':ids[-1]});http('/__release',{'barrier':'queued-burst'})
    wait(lambda:all(cli('operation.inspect',{'id':eid})['state'] in ('completed','cancelled','failed') for eid in ids),90)
    final=[cli('operation.inspect',{'id':eid}) for eid in ids];save('final-operations.json',final)
    check('drainedFiveCancelledOne',[x['state'] for x in final]==['completed']*5+['cancelled'])
    events=http('/__state')['events'];save('fixture-summary.json',[{k:e[k] for k in ('method','bucket','key','bodyBytes','bodySha256','signatureValid')} for e in events])
    check('fivePuts',sum(e['method']=='PUT' for e in events)==5)
    time.sleep(2);shot('03-drained-history');report['memoryAfter']=family.sample();report['operationIds']=ids
    report['passed']=True
except Exception as e:
    report['error']=str(e)
    print(type(e).__name__+': '+str(e),flush=True)
finally:
    if resident:
        try:cli('ui.quit');resident.wait(timeout=20)
        except Exception:
            resident.terminate();resident.wait(timeout=10)
    if fixture:fixture.terminate();fixture.wait(timeout=10)
    if cover:
        try:cover.communicate('quit\n',timeout=10)
        except Exception:cover.terminate();cover.wait(timeout=10)
    if rfb:rfb.close()
    report['cleanup']={'residentExited':resident is None or resident.poll() is not None,'fixtureExited':fixture is None or fixture.poll() is not None,'coverExited':cover is None or cover.poll() is not None}
    save('calls.json',calls);save('result.json',report)
    print(json.dumps({'passed':report['passed'],'run':str(out),'error':report.get('error')}),flush=True)
sys.exit(0 if report['passed'] else 1)
