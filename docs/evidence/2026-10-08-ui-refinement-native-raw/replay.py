#!/usr/bin/env python3
"""Explicit, receipt-producing native UI replay phases. Preparation is inert."""
from pathlib import Path
import argparse, datetime, hashlib, json, os, re, shlex, subprocess, sys, tarfile, tempfile, time
ROOT=Path('/home/patrick/work/put.gd')
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'tools/lab'))
import lab
GUESTS={'kde':('kde-wayland',22234,5934),'gnome':('gnome-wayland',22236,5936),'cinnamon':('cinnamon-x11',22238,5938)}
TERMINAL=['completed','failed','cancelled','partial','interrupted']
def now(): return datetime.datetime.now(datetime.timezone.utc).isoformat()
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path,value):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    with path.open('x') as f: f.write(json.dumps(value,indent=2)+'\n')
def read(path): return json.loads(Path(path).read_text())
def run_for(name):
    run=ROOT/'work/lab-runs'/('current-'+name+'-a3d70dc');s=lab.state(run)
    profile,ssh,vnc=GUESTS[name]
    assert s['profile']==profile and s['ssh_port']==ssh and s['vnc_display']+5900==vnc
    assert Path(s['overlay']).resolve()==run/'disk.qcow2'
    assert (run/'qmp.sock').is_socket() # Existing overlay only; never launch/reboot/stop VM.
    return run
class Replay:
    def __init__(self,args):
        self.args=args;self.out=Path(args.output).resolve()
        assert self.out.is_relative_to(HERE), 'Output must stay in owned scratch area'
        assert re.fullmatch('[a-zA-Z0-9-]+',self.out.name), 'Use ASCII alphanumeric/hyphen output name'
        self.name=getattr(args,'guest',None);self.run=run_for(self.name) if self.name else None
        self.dest=self.out/self.name if self.name else self.out
        self.dest.mkdir(parents=True,exist_ok=True)
        self.phase=args.phase
        stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%f')
        self.attempt=self.dest/'attempts'/(stamp+'-'+self.phase);self.attempt.mkdir(parents=True)
        save(self.attempt/'request.json',{'utc':now(),'argv':sys.argv,'runnerSha256':sha(__file__),'labSha256':sha(lab.__file__)})
        self.n=0
    def command(self,argv,*,text=None,cwd=None,timeout=60):
        self.n+=1;p=self.attempt/f'{self.n:02d}'
        if text is not None: (p.with_suffix('.stdin')).write_text(text)
        save(p.with_suffix('.command.json'),{'argv':[str(x) for x in argv],'cwd':str(cwd) if cwd else None,'stdinFile':p.with_suffix('.stdin').name if text is not None else None,'startedUtc':now()})
        with p.with_suffix('.stdout').open('w') as stdout,p.with_suffix('.stderr').open('w') as stderr:
            r=subprocess.run(argv,input=text,text=True,cwd=cwd,stdout=stdout,stderr=stderr,timeout=timeout)
        save(p.with_suffix('.result.json'),{'exitCode':r.returncode,'finishedUtc':now()})
        r.check_returncode();return p.with_suffix('.stdout').read_text()
    def python(self,code,timeout=60): return json.loads(self.command(lab.ssh_args(self.run)+['python3','-'],text=code,timeout=timeout))
    def shell(self,code,timeout=60): return self.command(lab.ssh_args(self.run)+['sh','-s'],text=code,timeout=timeout)
    def session(self,code):
        # Exact same native environment discovery as lab.guest_session. No general environment dumps.
        user=lab.state(self.run)['guest_user'];profile=lab.load_profile(GUESTS[self.name][0])
        pre=f'''set -eu
pid=$(pgrep -u {shlex.quote(user)} -x {shlex.quote(profile['session_process'])} | head -n 1)
[ -n "$pid" ]
for name in DISPLAY XAUTHORITY DBUS_SESSION_BUS_ADDRESS XDG_SESSION_TYPE XDG_CURRENT_DESKTOP WAYLAND_DISPLAY; do
 value=$(tr '\\0' '\\n' < /proc/$pid/environ | sed -n "s/^$name=//p")
 [ -z "$value" ] || export "$name=$value"
done
export QT_QPA_PLATFORM={shlex.quote(profile['qt_platform'])}
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export XDG_CONFIG_HOME=$HOME/putgd-lab/config
export XDG_DATA_HOME=$HOME/putgd-lab/data
'''
        return self.shell(pre+code)
    def cli(self,method,params=None):
        reply=json.loads(self.session('putgd-cli '+shlex.quote(method)+' '+shlex.quote(json.dumps(params or {}))+'\n'))
        assert reply['ok'], reply.get('error',{}).get('code');return reply['result']
    def put(self,local,remote):
        argv,endpoint=lab.scp_connection(self.run)
        return self.command([str(x) for x in argv]+[str(local),endpoint+remote])
    def get(self,remote,local):
        assert not Path(local).exists(), 'Preserve earlier download'
        argv,endpoint=lab.scp_connection(self.run)
        return self.command([str(x) for x in argv]+[endpoint+remote,str(local)])
    def source(self):
        s=read(self.out/'source.json');assert sha(s['archive'])==s['archiveSha256']
        if s.get('candidate'):
            assert sha(s['candidate']['patch'])==s['candidate']['patchSha256']
            assert sha(s['candidate']['bundle'])==s['candidate']['bundleSha256']
        return s
    def identity(self):
        value=self.python(IDENTITY)
        assert len(value)==1, 'Exactly one owned resident required'
        return value[0]
    def assert_current(self):
        installed=read(self.dest/'installed.json');started=read(self.dest/'start.json')
        value=self.identity();assert value==started['resident']
        assert value['sha256']==installed['binarySha256']['/usr/bin/putgd']
        d=self.cli('ui.snapshot');assert d['build']==installed['manifest']
        assert d['build']['gitCommit']==self.source()['commit']
        return value,d
    def witness(self,label):
        assert re.fullmatch('[a-z0-9-]+',label)
        from PIL import Image
        path=self.dest/'views'/(label+'.png');assert not path.exists(),'Use a unique witness label'
        path.parent.mkdir(exist_ok=True)
        # Unique QMP destination preserves every older native capture in run directory.
        name='ui-refinement-'+self.out.name+'-'+label
        assert not (self.run/(name+'-head0.ppm')).exists()
        result=lab.screenshot(self.run,name);im=Image.open(result['path']);im.save(path)
        value={'utc':now(),'path':str(path),'sha256':sha(path),'nativeQmp':result,'desktopPixels':list(im.size),'vncPort':GUESTS[self.name][2],'visualReview':'pending'}
        save(path.with_suffix('.json'),value);return value
    def freeze(self):
        assert re.fullmatch('[0-9a-f]{40}',self.args.commit)
        commit=self.command(['git','rev-parse',self.args.commit+'^{commit}'],cwd=ROOT).strip();assert commit==self.args.commit
        tree=self.command(['git','rev-parse',commit+'^{tree}'],cwd=ROOT).strip()
        count=int(self.command(['git','rev-list','--count',commit],cwd=ROOT))
        archive=self.out/'source.tar';assert not archive.exists()
        self.command(['git','archive','--format=tar','-o',str(archive),commit],cwd=ROOT)
        files={}
        with tarfile.open(archive) as t:
            for m in t.getmembers():
                assert not m.name.startswith(('work/','.aws/','.context/')) and Path(m.name).name!='credentials.txt'
                if m.isfile():files[m.name]={'type':'file','mode':m.mode,'sha256':hashlib.sha256(t.extractfile(m).read()).hexdigest()}
                elif m.issym():files[m.name]={'type':'symlink','target':m.linkname}
        save(self.out/'source.json',{'schemaVersion':1,'utc':now(),'commit':commit,'tree':tree,'commitCount':count,'archive':str(archive),'archiveSha256':sha(archive),'files':files})
    def stop(self):
        assert not (self.dest/'stopped.json').exists()
        # Use fresh identity plus the read-only preparation inventory to reject any replacement.
        inventory_path=self.out/'preparation-inventory.json'
        inventory=read(inventory_path if inventory_path.is_file() else HERE/'preparation-inventory-localhost.json')['guests'][self.name]['guest']
        expected=[{k:p[k] for k in ('pid','startTicks','sha256')} for p in inventory['processes'] if p['comm']=='putgd']
        actual=self.identity();assert expected==[actual], 'Resident changed since preparation; re-inventory deliberately'
        snapshot=self.cli('ui.snapshot');assert snapshot['build']==inventory['manifest']
        assert not snapshot['diagnostics']['ui'].get('pid'), 'Close the owned UI normally before stopping'
        assert self.cli('diagnostics')['activeOperations']==0
        # SQL returns ID/state only and checks the ENTIRE old synthetic journal, no payload inspection.
        states=self.python('''import os,json,sqlite3,pathlib
p=pathlib.Path('/home/lab/putgd-lab/data/putgd/putgd/history.sqlite')
assert p.is_file() and p.stat().st_uid==os.getuid()
with sqlite3.connect(p.as_uri()+'?mode=ro',uri=True) as c:
 rows=[{'id':x[0],'state':x[1]} for x in c.execute("SELECT json_extract(data,'$.id'),json_extract(data,'$.state') FROM operations")]
 deletions=[{'id':x[0],'state':x[1]} for x in c.execute("SELECT json_extract(data,'$.id'),json_extract(data,'$.state') FROM deletions")]
print(json.dumps({'operations':rows,'deletions':deletions}))
''')
        assert all(x['state'] in TERMINAL for x in states['operations']+states['deletions'])
        save(self.dest/'previous-state.json',{'resident':actual,'manifest':inventory['manifest'],'terminalStates':states})
        prior=self.dest/'previous-live-markers';prior.mkdir()
        for name in ('package.json','versions.json','diagnostics.json','state.json'):
            p=self.run/name
            if p.is_file(): (prior/name).write_bytes(p.read_bytes())
        backup='/home/lab/putgd-lab-before-ui-refinement-'+self.out.name
        assert re.fullmatch('/home/lab/putgd-lab-before-ui-refinement-[a-zA-Z0-9-]+',backup)
        code=STOP.replace('EXPECTED',repr(actual)).replace('BACKUP',repr(backup))
        result=self.python(code,timeout=30)
        save(self.dest/'stopped.json',{'utc':now(),'previousResident':actual,'previousManifest':inventory['manifest'],'terminalStates':states,'preservedGuestProfile':backup,'stopResult':result})
    def all_stopped(self):
        for name in GUESTS: assert (self.out/name/'stopped.json').is_file(), 'Stop all old residents before build'
    def host_build(self):
        self.all_stopped();s=self.source();out=self.out/'host';out.mkdir()
        if s.get('candidate'):
            src=Path(s['candidate']['worktree']);assert src.is_relative_to(ROOT/'work/view-startup-latency')
            for name,digest in s['candidate']['files'].items():assert sha(src/name)==digest
            self.command(['cmake','--build','--preset','release'],cwd=src,timeout=1800)
            self.command(['cpack','--config','build/release/CPackConfig.cmake','-G','DEB','-B',str(out/'packages')],cwd=src,timeout=180)
            packages=list((out/'packages').glob('*.deb'));assert len(packages)==1
            manifest=read(src/'build/release/build-identity/build-info.json');assert manifest['gitCommit']==s['commit'] and manifest['dirty'] is True
            save(out/'build.json',{'utc':now(),'sourceCommit':s['commit'],'candidatePatchSha256':s['candidate']['patchSha256'],'manifest':manifest,'package':str(packages[0]),'packageSha256':sha(packages[0]),'nativeAcceptance':'pending'})
            return
        src=out/'source';src.mkdir();self.command(['tar','-xf',s['archive'],'-C',str(src)])
        sdk=ROOT/'build';assert (sdk/'sdk/lib/cmake/AWSSDK/AWSSDKConfig.cmake').is_file()
        for preset in ('dev','release'):
            argv=configure(preset,s,sdk/'sdk',sdk/'dependencies/aws-sdk-cpp')
            self.command(argv,cwd=src,timeout=180)
            self.command(['cmake','--build','--preset',preset],cwd=src,timeout=3600)
            if preset=='dev':self.command(['ctest','--preset','dev','--output-junit',str(out/'dev.xml')],cwd=src,timeout=1800)
        self.command(['cpack','--config','build/release/CPackConfig.cmake','-G','DEB','-B',str(out/'packages')],cwd=src,timeout=180)
        packages=list((out/'packages').glob('*.deb'));assert len(packages)==1
        manifest=read(src/'build/release/build-identity/build-info.json');assert manifest['gitCommit']==s['commit']
        save(out/'build.json',{'utc':now(),'sourceCommit':s['commit'],'manifest':manifest,'package':str(packages[0]),'packageSha256':sha(packages[0]),'nativeAcceptance':'pending'})
    def guest_build(self):
        assert self.name in ('kde','gnome'), 'Cinnamon requires exact host package: no guest toolchain provisioning'
        self.all_stopped();s=self.source();guest='/home/lab/putgd-ui-refinement-'+self.out.name
        self.shell('mkdir '+shlex.quote(guest)+'\n')
        self.put(s['archive'],guest+'/source.tar');self.put(self.out/'source.json',guest+'/source.json')
        if s.get('candidate'):
            self.put(s['candidate']['bundle'],guest+'/source.bundle');self.put(s['candidate']['patch'],guest+'/candidate.patch')
        verify='''import pathlib,hashlib,json,subprocess
p=pathlib.Path.cwd();s=json.loads((p/'source.json').read_text());assert hashlib.sha256((p/'source.tar').read_bytes()).hexdigest()==s['archiveSha256']
if s.get('candidate'):
 assert hashlib.sha256((p/'source.bundle').read_bytes()).hexdigest()==s['candidate']['bundleSha256']
 assert hashlib.sha256((p/'candidate.patch').read_bytes()).hexdigest()==s['candidate']['patchSha256']
 subprocess.run(['git','clone','source.bundle','source'],check=True,stdout=subprocess.DEVNULL)
 subprocess.run(['git','-C','source','checkout','--detach',s['commit']],check=True,stdout=subprocess.DEVNULL)
 subprocess.run(['git','-C','source','apply','../candidate.patch'],check=True)
else:
 (p/'source').mkdir();subprocess.run(['tar','-xf','source.tar','-C','source'],check=True)
for name,item in s['files'].items():
 f=p/'source'/name
 if item['type']=='file':assert f.is_file() and not f.is_symlink() and hashlib.sha256(f.read_bytes()).hexdigest()==item['sha256'] and f.stat().st_mode&0o777==item['mode'],name
 else:assert f.is_symlink() and str(f.readlink())==item['target'],name
print(json.dumps({'verified':True,'files':len(s['files'])}))
'''
        self.command(lab.ssh_args(self.run)+['cd '+shlex.quote(guest)+' && python3 -'],text=verify)
        script='set -eu\ncd '+shlex.quote(guest+'/source')+'\n'
        for preset in ('dev','release'):
            script+=shlex.join(configure(preset,s,Path('/home/lab/putgd-memory-build/sdk'),Path('/home/lab/putgd-memory-build/dependencies/aws-sdk-cpp')))+'\n'
            script+='cmake --build --preset '+preset+'\n'
            if preset=='dev':script+='ctest --preset dev --output-junit '+shlex.quote(guest+'/dev.xml')+'\n'
        script+='cpack --config build/release/CPackConfig.cmake -G DEB -B '+shlex.quote(guest+'/packages')+'\n'
        self.shell(script,timeout=7200)
        remote=self.shell('find '+shlex.quote(guest+'/packages')+" -maxdepth 1 -name '*.deb' -print\n").splitlines();assert len(remote)==1
        package=self.dest/Path(remote[0]).name;assert not package.exists();self.get(remote[0],package)
        self.get(guest+'/dev.xml',self.dest/'dev.xml')
        self.get(guest+'/source/build/release/build-identity/build-info.json',self.dest/'build-info.json')
        manifest=read(self.dest/'build-info.json');assert manifest['gitCommit']==s['commit']
        save(self.dest/'build.json',{'utc':now(),'sourceCommit':s['commit'],'manifest':manifest,'guestDirectory':guest,'package':str(package),'packageSha256':sha(package),'nativeAcceptance':'pending'})
    def install(self):
        self.all_stopped();s=self.source();b=read(self.out/'host/build.json' if self.name=='cinnamon' else self.dest/'build.json')
        assert b['sourceCommit']==s['commit'] and sha(b['package'])==b['packageSha256']
        self.python(NO_APP)
        before=self.python(INSTALLED)
        with tempfile.TemporaryDirectory(prefix='putgd-ui-refinement-') as tmp:
            self.command(['dpkg-deb','--extract',b['package'],tmp]);base=Path(tmp)
            manifest=read(base/'usr/share/putgd/build-info.json');assert manifest==b['manifest']
            expected={'/usr/bin/'+p.name:sha(p) for p in (base/'usr/bin').iterdir() if p.is_file() and p.name.startswith('putgd')}
            extension=sha(base/'usr/share/gnome-shell/extensions/putgd@put.gd/extension.js')
        if self.name=='gnome':assert extension==before['extensionSha256'], 'Extension changed: stop and plan visible Shell reload separately'
        remote='/tmp/putgd-ui-refinement-'+self.out.name+'.deb';self.put(b['package'],remote)
        simulation=self.shell('LC_ALL=C apt-get --simulate --no-download --no-remove install '+shlex.quote(remote)+'\n',timeout=60)
        changed=re.findall(r'^Inst (\S+)',simulation,re.M)
        assert changed==['putgd'] and not re.search(r'^Remv ',simulation,re.M), 'Local package installation would change other packages'
        self.shell('LC_ALL=C sudo -n apt-get --no-remove -y install '+shlex.quote(remote)+'\n',timeout=180)
        after=self.python(INSTALLED)
        assert after['manifest']==manifest and after['binarySha256']=={p:expected[p] for p in after['binarySha256']}
        assert all(v==Path(p).name+' '+manifest['version'] for p,v in after['versions'].items())
        assert after['extensionSha256']==extension
        save(self.dest/'installed.json',after)
        # Preserve old markers above, then truthfully update only live current markers.
        lab.write_json(self.run/'package.json',{'sha256':b['packageSha256'],'filename':Path(b['package']).name,'installedBinarySha256':expected,'sourceCommit':s['commit'],'updatedByReplay':str(self.dest/'installed.json')})
        oldversions=read(self.run/'versions.json') if (self.run/'versions.json').exists() else {}
        oldversions['packages']=[p for p in oldversions.get('packages',[]) if not p.startswith('putgd ')] + ['putgd '+manifest['packageVersion']]
        oldversions['putgdBuild']=manifest;oldversions['updatedUtc']=now();lab.write_json(self.run/'versions.json',oldversions)
        save(self.dest/'install.json',{'utc':now(),'beforeInstalledManifest':before['manifest'],'installed':after,'extensionReload':'Existing Shell retained; hash unchanged' if self.name=='gnome' else 'not required','package':b})
    def start(self):
        assert not (self.dest/'start.json').exists();self.python(NO_APP);installed=read(self.dest/'installed.json')
        self.session('mkdir "$HOME/putgd-lab"\nnohup putgd-launcher --background > "$HOME/putgd-lab/app.log" 2>&1 < /dev/null &\n')
        d=wait(lambda:self.cli('ui.snapshot') if self.cli('diagnostics').get('journalReady') else None,30)
        assert d['build']==installed['manifest'];identity=self.identity();assert identity['sha256']==installed['binarySha256']['/usr/bin/putgd']
        assert not self.cli('operation.recent')['operations']
        save(self.dest/'start.json',{'utc':now(),'resident':identity,'build':d['build'],'session':'visible native '+GUESTS[self.name][0],'vncPort':GUESTS[self.name][2]})
        lab.write_json(self.run/'diagnostics.json',self.cli('diagnostics'))
        self.witness('fresh-start') # Fresh synthetic-only setup; caller closes it visibly.
    def configure(self):
        _,d=self.assert_current();assert not d['diagnostics']['ui'].get('pid'),'Close every view before CLI configuration'
        settings=self.cli('settings.get');patch={'actions':{a:{'destination':'local','profileId':None,'shortcut':''} for a in ('region','window','desktop','clipboard')},'profiles':[],'archive':{'root':'/home/lab/putgd-lab/archive','keepUploads':False},'preferences':{'notifications':False,'launchAtLogin':False}}
        if self.args.kind=='configured':
            profile=self.cli('settings.developmentTemplate')['document']['profiles'][0]
            profile.update(id='synthetic-ui-only',label='Synthetic destination (no upload)')
            profile['settings'].update(bucket='synthetic-ui-only',endpoint='https://example.invalid',publicUrlBase='https://example.invalid/ui-only',objectPrefix='synthetic-ui-only/')
            profile['credentials']={'mode':'session','reference':None};patch['profiles']=[profile]
        result=self.cli('settings.patch',{'expectedRevision':settings['metadata']['observedRevision'],'patch':patch})
        after=self.cli('settings.get');assert all(x['destination']=='local' for x in after['document']['actions'].values())
        assert len(after['document']['profiles'])==(self.args.kind=='configured')
        save(self.attempt/'configuration.json',{'utc':now(),'kind':self.args.kind,'patch':patch,'reply':result,'acceptedRevision':after['metadata']['acceptedRevision'],'cloudActionsEnabled':False})
    def show(self):
        _,before=self.assert_current();assert not before['diagnostics']['ui'].get('pid'),'Close current helper normally first'
        reply=self.cli('ui.show',{'view':self.args.view});d=wait(lambda:self.cli('ui.snapshot'),15,lambda x:x['diagnostics']['ui'].get('pid',0)>0)
        pid=d['diagnostics']['ui']['pid'];identity=self.python(HELPER.replace('PID',str(pid)))
        assert identity['parentPid']==self.identity()['pid'] and identity['sha256']==read(self.dest/'installed.json')['binarySha256']['/usr/bin/putgd-ui']
        time.sleep(.7)
        save(self.attempt/'shown.json',{'utc':now(),'view':self.args.view,'reply':reply,'helper':identity,'trigger':'production CLI ui.show; native visible window','witness':self.witness(self.args.label)})
    def close(self):
        resident,d=self.assert_current();pid=d['diagnostics']['ui'].get('pid',0);assert pid>0
        before=self.python(HELPER.replace('PID',str(pid)))
        lab.key(self.run,['alt','f4'])
        after=wait(lambda:self.cli('ui.snapshot'),15,lambda x:x['diagnostics']['ui'].get('pid',0)==0)
        assert after['diagnostics']['ui']['lastExit']['status']=='normal' and after['diagnostics']['ui']['lastExit']['code']==0
        assert after['diagnostics']['trayAvailable'] and self.identity()==resident
        self.shell('test ! -e /proc/'+str(pid)+'\n')
        save(self.attempt/'closed.json',{'utc':now(),'gesture':['alt','f4'],'helperBefore':before,'helperAfter':after['diagnostics']['ui'],'residentUnchanged':resident,'witness':self.witness(self.args.label)})
    def step(self):
        self.assert_current();values=self.args.values
        if self.args.kind=='click':
            x,y=map(int,values);lab.pointer(self.run,x,y,1280,800,True);time.sleep(.05);lab.pointer(self.run,x,y,1280,800,False)
        elif self.args.kind=='hover':
            x,y=map(int,values);lab.pointer(self.run,x,y,1280,800);time.sleep(1)
        elif self.args.kind=='drag':
            x,y,a,b=map(int,values);lab.pointer(self.run,x,y,1280,800,True)
            for i in range(1,11):lab.pointer(self.run,round(x+(a-x)*i/10),round(y+(b-y)*i/10),1280,800);time.sleep(.03)
            lab.pointer(self.run,a,b,1280,800,False)
        elif self.args.kind=='key':
            if values==['ctrl','v']:
                # Fresh independent paste witness; retain previous verification receipts.
                self.python("import pathlib,os,json\np=pathlib.Path('/home/lab/putgd-lab/result-paste-fixture/paste.json');old=None\nif p.exists():\n assert not p.is_symlink() and p.stat().st_uid==os.getuid()\n old={'mtimeNs':p.stat().st_mtime_ns,'bytes':p.stat().st_size};p.unlink()\nprint(json.dumps({'previousMarker':old,'freshPasteRequired':True}))\n")
            lab.key(self.run,values)
        elif self.args.kind!='shot':raise ValueError(self.args.kind)
        time.sleep(.5);save(self.attempt/'step.json',{'utc':now(),'kind':self.args.kind,'values':values,'witness':self.witness(self.args.label)})
    def populate(self):
        _,d=self.assert_current();assert not d['diagnostics']['ui'].get('pid')
        settings=self.cli('settings.get');assert all(x['destination']=='local' for x in settings['document']['actions'].values())
        # Captures only fresh synthetic lab desktop; no historical payload or cloud access.
        results=[]
        for _ in range(3):
            reply=self.cli('action.invoke',{'action':'desktop'});oid=reply['operationId']
            op=wait(lambda:self.cli('operation.inspect',{'id':oid}),30,lambda x:x['state'] in TERMINAL)
            assert op['state']=='completed' and all(x['uploadStatus']=='local_only' for x in op['items'])
            results.append({'id':oid,'state':op['state'],'itemIds':[x['artifact']['id'] for x in op['items']]})
        history=self.cli('history.list',{'limit':50});assert history['total']>=3
        save(self.dest/'synthetic-operations.json',{'utc':now(),'operations':results,'historyIds':[x['id'] for x in history['records']],'scope':'Real desktop source → local archive; no remote provider request'})
    def matrix(self):
        self.assert_current();assert not self.cli('ui.snapshot')['diagnostics']['ui'].get('pid')
        source=self.source();assert sha(ROOT/'tools/lab/lab.py')==source['files']['tools/lab/lab.py']['sha256']
        cases=[('qt',x) for x in ('region','window','desktop','clipboard-text','clipboard-image','clipboard-files','region-cancel-escape','region-cancel-right-click','region-cancel-zero-area')]+[('gtk',x) for x in ('clipboard-text','clipboard-image','clipboard-files')]
        results=[]
        for case_index,(client,scenario) in enumerate(cases,1):
            # A fresh control-directory proxy shares ONLY the current overlay socket/SSH identity.
            # Lab generic screenshots/results cannot overwrite any older run-directory evidence.
            # Unix sockets have a short pathname limit even through symlinks.
            # Keep the persistent control proxy short; receipts link its exact path.
            proxy=ROOT/'work/lab-runs'/('ui43-'+self.name+'-'+self.attempt.name.split('-')[0]+'-'+str(case_index))
            assert len(os.fsencode(proxy/'qmp.sock'))<104
            proxy.mkdir()
            lab.write_json(proxy/'state.json',lab.state(self.run))
            (proxy/'identity').symlink_to(self.run/'identity');(proxy/'qmp.sock').symlink_to(self.run/'qmp.sock')
            (proxy/'package.json').write_bytes((self.run/'package.json').read_bytes())
            argv=[sys.executable,str(ROOT/'tools/lab/lab.py'),'run','--run',str(proxy),'--scenario',scenario,'--clipboard-client',client,'--timeout','90','--keep']
            result=json.loads(self.command(argv,cwd=ROOT,timeout=180));results.append({'client':client,'scenario':scenario,'result':result,'controlDirectory':str(proxy)})
            save(self.attempt/('matrix-'+client+'-'+scenario+'.json'),results[-1])
            assert result['ok'], 'Native case failed; preserve attempt, do not silently retry'
            self.assert_current();assert not self.cli('ui.snapshot')['diagnostics']['ui'].get('pid')
        save(self.dest/'matrix.json',{'utc':now(),'sourceCommit':source['commit'],'cases':results,'proceduralPassed':True,'visualReview':'pending','scope':'Installed current-source real native four-action Qt/GTK matrix plus visible region cancellation; local synthetic only'})
    def observe(self):
        resident,d=self.assert_current();s=self.cli('settings.get')
        save(self.attempt/'observed.json',{'utc':now(),'resident':resident,'ui':d['diagnostics']['ui'],'acceptedRevision':s['metadata']['acceptedRevision'],'observedRevision':s['metadata']['observedRevision'],'profileIds':[p['id'] for p in s['document']['profiles']],'actionOutputs':{a:x['destination'] for a,x in s['document']['actions'].items()},'activeOperations':self.cli('diagnostics')['activeOperations']})
    def resize(self):
        assert self.name=='cinnamon', 'Wayland geometry must use visible window borders/QMP drag with a reviewed screenshot'
        _,d=self.assert_current();pid=d['diagnostics']['ui'].get('pid',0);assert pid>0
        width,height=self.args.width,self.args.height;assert (width,height) in ((1024,700),(1200,700))
        text=self.session('wmctrl -lpG\n')
        windows=[x.split(maxsplit=8) for x in text.splitlines()]
        owned=[x for x in windows if len(x)==9 and int(x[2])==pid];assert len(owned)==1
        wid=owned[0][0]
        self.session('wmctrl -ir '+shlex.quote(wid)+' -b remove,maximized_vert,maximized_horz\nwmctrl -ir '+shlex.quote(wid)+' -e 0,5,35,'+str(width)+','+str(height)+'\n')
        time.sleep(.4);after=self.session('wmctrl -lpG\n');row=next(x.split(maxsplit=8) for x in after.splitlines() if x.startswith(wid+' '))
        assert list(map(int,row[5:7]))==[width,height]
        save(self.attempt/'resize.json',{'utc':now(),'logicalClientSize':[width,height],'pidOwnedWindow':wid,'method':'Native EWMH resize of already discovered PID-owned helper window','witness':self.witness(self.args.label)})
    def fixture(self):
        self.assert_current()
        if self.args.kind=='start':
            self.session('mkdir "$HOME/putgd-lab/result-paste-fixture"\nnohup putgd-desktop-fixture --output "$HOME/putgd-lab/result-paste-fixture" > "$HOME/putgd-lab/result-paste-fixture.log" 2>&1 < /dev/null &\n')
            time.sleep(1.2);self.witness(self.args.label)
        elif self.args.kind=='invoke-text':
            # Caller focuses fixture and visibly sends Ctrl+Alt+Shift+T first.
            marker=self.python("import pathlib\nprint(pathlib.Path('/home/lab/putgd-lab/result-paste-fixture/source.json').read_text())\n")
            assert marker=={'synthetic':True,'key':84}
            settings=self.cli('settings.get');assert all(x['destination']=='local' for x in settings['document']['actions'].values())
            reply=self.cli('action.invoke',{'action':'clipboard'});oid=reply['operationId']
            op=wait(lambda:self.cli('operation.inspect',{'id':oid}),30,lambda x:x['state'] in TERMINAL)
            assert op['state']=='completed' and len(op['items'])==1 and op['items'][0]['uploadStatus']=='local_only'
            save(self.dest/'clipboard-operation.json',{'utc':now(),'operationId':oid,'itemId':op['items'][0]['artifact']['id'],'state':op['state'],'copyStatus':op['copyStatus'],'expectedClipboardText':'putgd synthetic clipboard text','scope':'Known synthetic Qt clipboard text → production native import → local archive'})
            self.witness(self.args.label)
        else:
            # Caller selects recorded Activity parent OR child, clicks actual Copy result,
            # focuses fixture and sends Ctrl+V. No operation-ID copy control is invented.
            result=self.python("import pathlib\nprint(pathlib.Path('/home/lab/putgd-lab/result-paste-fixture/paste.json').read_text())\n")
            fixture=read(self.dest/'clipboard-operation.json');assert result.get('text')==fixture['expectedClipboardText']
            op=self.cli('operation.inspect',{'id':fixture['operationId']});assert op['state']=='completed' and op['copyStatus']=='copied'
            save(self.attempt/'activity-result-copy.json',{'utc':now(),'selectionKind':self.args.selection,'parentOperationId':fixture['operationId'],'itemId':fixture['itemId'],'copyStatus':op['copyStatus'],'independentPasteMatchesExpectedText':True,'witness':self.witness(self.args.label),'limitation':'Selected parent/item identity requires visible row/full-ID tooltip review; clipboard contains result text, not an operation ID'})

IDENTITY='''from pathlib import Path
import hashlib,json,os
rows=[]
for p in Path('/proc').iterdir():
 try:
  if p.name.isdecimal() and p.stat().st_uid==os.getuid() and (p/'comm').read_text().strip()=='putgd':
   before=int((p/'stat').read_text().rsplit(')',1)[1].split()[19]);h=hashlib.sha256((p/'exe').read_bytes()).hexdigest();assert before==int((p/'stat').read_text().rsplit(')',1)[1].split()[19]);rows.append({'pid':int(p.name),'startTicks':before,'sha256':h})
 except FileNotFoundError:pass
print(json.dumps(rows))
'''
HELPER='''from pathlib import Path
import os,json,hashlib
p=Path('/proc')/str(PID);assert p.stat().st_uid==os.getuid() and (p/'comm').read_text().strip()=='putgd-ui'
s=(p/'stat').read_text().rsplit(')',1)[1].split();r={'pid':PID,'startTicks':int(s[19]),'parentPid':int(s[1]),'sha256':hashlib.sha256((p/'exe').read_bytes()).hexdigest()};assert r['startTicks']==int((p/'stat').read_text().rsplit(')',1)[1].split()[19]);print(json.dumps(r))
'''
NO_APP='''from pathlib import Path
import os,json
for p in Path('/proc').iterdir():
 try:
  if p.name.isdecimal() and p.stat().st_uid==os.getuid():assert (p/'comm').read_text().strip() not in ('putgd','putgd-ui'),'Old app/UI running'
 except FileNotFoundError:pass
print(json.dumps({'noOldApp':True}))
'''
STOP='''from pathlib import Path
import hashlib,json,os,signal,time
expected=EXPECTED;p=Path('/proc')/str(expected['pid']);old=Path('/home/lab/putgd-lab');backup=Path(BACKUP)
assert old.is_dir() and not old.is_symlink() and old.stat().st_uid==os.getuid() and not backup.exists()
fd=os.pidfd_open(expected['pid'])
try:
 actual={'pid':expected['pid'],'startTicks':int((p/'stat').read_text().rsplit(')',1)[1].split()[19]),'sha256':hashlib.sha256((p/'exe').read_bytes()).hexdigest()}
 assert actual==expected and p.stat().st_uid==os.getuid() and (p/'comm').read_text().strip()=='putgd'
 signal.pidfd_send_signal(fd,signal.SIGTERM)
finally:os.close(fd)
end=time.monotonic()+15
while p.exists():
 if time.monotonic()>end:raise RuntimeError('Guarded old resident did not exit normally')
 time.sleep(.1)
for q in Path('/proc').iterdir():
 try:
  if q.name.isdecimal() and q.stat().st_uid==os.getuid():assert (q/'comm').read_text().strip() not in ('putgd','putgd-ui'),'Old app/UI remains'
 except FileNotFoundError:pass
old.rename(backup) # Entire synthetic directory, including manual prior captures; no traversal/payload reads.
print(json.dumps({'stopped':expected,'preservedAt':str(backup),'method':'PID/start/hash guarded pidfd SIGTERM'}))
'''
INSTALLED='''from pathlib import Path
import hashlib,json,subprocess
files=['/usr/bin/putgd','/usr/bin/putgd-cli','/usr/bin/putgd-ui'];ext=Path('/usr/share/gnome-shell/extensions/putgd@put.gd/extension.js')
print(json.dumps({'manifest':json.loads(Path('/usr/share/putgd/build-info.json').read_text()),'binarySha256':{p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in files},'versions':{p:subprocess.check_output([p,'--version'],text=True).strip() for p in files},'extensionSha256':hashlib.sha256(ext.read_bytes()).hexdigest()}))
'''
def configure(preset,s,sdk,aws):
    result=['cmake','--preset',preset,'-DCMAKE_PREFIX_PATH='+str(sdk),'-DPUTGD_AWS_RUNTIME_DIR='+str(sdk/'lib'),'-DPUTGD_AWS_SOURCE_DIR='+str(aws),'-DPUTGD_LAB_FIXTURE=ON','-DCMAKE_INSTALL_PREFIX=/usr']
    if not s.get('candidate'):result+=['-DPUTGD_SOURCE_COMMIT='+s['commit'],'-DPUTGD_SOURCE_TREE='+s['tree'],'-DPUTGD_SOURCE_COMMIT_COUNT='+str(s['commitCount'])]
    return result
def wait(fn,seconds,predicate=bool):
    end=time.monotonic()+seconds
    while True:
        try:
            v=fn()
            if predicate(v):return v
        except (subprocess.CalledProcessError,AssertionError,lab.LabError):pass
        if time.monotonic()>end:raise RuntimeError('Bounded readiness check timed out')
        time.sleep(.2)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True)
    sub=p.add_subparsers(dest='phase',required=True)
    q=sub.add_parser('freeze');q.add_argument('--commit',required=True)
    sub.add_parser('host-build')
    for phase in ('stop','guest-build','install','start','configure','show','close','step','populate','fixture','observe','resize','matrix'):
        q=sub.add_parser(phase);q.add_argument('--guest',choices=list(GUESTS),required=True)
        if phase=='configure':q.add_argument('kind',choices=['empty','configured'])
        if phase=='show':q.add_argument('view',choices=['settings','history','activity','about']);q.add_argument('label')
        if phase=='close':q.add_argument('label')
        if phase=='step':q.add_argument('label');q.add_argument('kind',choices=['click','hover','drag','key','shot']);q.add_argument('values',nargs='*')
        if phase=='resize':q.add_argument('label');q.add_argument('width',type=int);q.add_argument('height',type=int)
        if phase=='fixture':q.add_argument('kind',choices=['start','invoke-text','verify-result-paste']);q.add_argument('label');q.add_argument('--selection',choices=['parent','child'],default='parent')
    args=p.parse_args();r=Replay(args)
    try:
        getattr(r,args.phase.replace('-','_'))();save(r.attempt/'complete.json',{'utc':now(),'phase':args.phase,'proceduralComplete':True,'nativeVisualAcceptance':'Requires image review; no automated acceptance claim'})
    except BaseException as e:
        save(r.attempt/'failed.json',{'utc':now(),'phase':args.phase,'errorType':type(e).__name__,'error':str(e)});raise
if __name__=='__main__':main()
