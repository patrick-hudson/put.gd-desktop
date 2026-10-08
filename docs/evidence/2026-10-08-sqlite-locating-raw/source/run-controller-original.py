import datetime, hashlib, json, os, pathlib, subprocess, time, uuid
os.umask(0o077)
binary=pathlib.Path('/tmp/putgd-sqlite-latency-probe-build/sqlite-latency-probe')
source=pathlib.Path('/home/patrick/work/put.gd/work/sqlite-latency-probe/probe.cpp')
cmake=source.with_name('CMakeLists.txt')
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
assert sha(binary)=='d4419f7c36fee1b21fd6435f72ff1d36946ec4b86432880e7bcf0605c89051fb'
stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
root=pathlib.Path('/tmp')/('putgd-sqlite-latency-rollback-'+stamp+'-'+uuid.uuid4().hex[:8])
root.mkdir(mode=0o700)
base=[(m,c) for c in (2000,512) for m in ('reopen','connection','statements')]
order=[(1,m,c) for m,c in base]+[(2,m,c) for m,c in reversed(base)]
manifest={'schemaVersion':1,'scope':'Serial standalone Qt SQL locating only. Fresh private synthetic paths/processes. Rollback DELETE, synchronous FULL; no WAL/product/desktop/config/user data.',
 'runRoot':str(root),'sourceSha256':sha(source),'cmakeSha256':sha(cmake),'binarySha256':sha(binary),
 'binary':str(binary),'repeatOrder':'forward, then exact reverse','plannedOrder':order,
 'timeoutPerCaseSeconds':45,'totalCapSeconds':600,'caseResults':[],'loadedFiles':{},
 'startUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'state':'running'}
def save():
    temp=root/'manifest.tmp'
    temp.write_text(json.dumps(manifest,indent=2)+'\n')
    temp.chmod(0o600)
    os.replace(temp,root/'manifest.json')
save()
print('runRoot='+str(root),flush=True)
deadline=time.monotonic()+600
for ordinal,(repeat,mode,cache) in enumerate(order,1):
    if time.monotonic()>=deadline:
        manifest['state']='total_deadline';save();break
    case=root/f'{ordinal:02d}-repeat{repeat}-{mode}-cache{cache}'
    outlog=root/(case.name+'.stdout.log');errlog=root/(case.name+'.stderr.log')
    argv=[str(binary),str(case),mode,str(cache),'--lock']
    result={'ordinal':ordinal,'repeat':repeat,'mode':mode,'cacheKiB':cache,'argv':argv,'output':str(case),
      'stdout':outlog.name,'stderr':errlog.name,'loadedFiles':[]}
    start=time.monotonic()
    with outlog.open('wb') as stdout,errlog.open('wb') as stderr:
        child=subprocess.Popen(argv,stdout=stdout,stderr=stderr)
        result['pid']=child.pid
        observed=set()
        while child.poll() is None:
            try:
                lines=pathlib.Path(f'/proc/{child.pid}/maps').read_text().splitlines()
                for line in lines:
                    fields=line.split(maxsplit=5)
                    if len(fields)!=6:continue
                    p=fields[5]
                    if p.startswith('/') and ('/lib' in p or p==str(binary)) and not p.endswith(' (deleted)'):
                        observed.add(p)
            except (FileNotFoundError,PermissionError,ProcessLookupError):pass
            if time.monotonic()-start>=45 or time.monotonic()>=deadline:
                child.terminate()
                try:child.wait(timeout=3)
                except subprocess.TimeoutExpired:child.kill();child.wait()
                result['timeout']=True;break
            time.sleep(.025)
        result['exitCode']=child.wait()
    result['wallSeconds']=time.monotonic()-start
    result['loadedFiles']=sorted(observed)
    for p in observed:
        if p not in manifest['loadedFiles']:
            try:manifest['loadedFiles'][p]={'sha256':sha(pathlib.Path(p)),'bytes':pathlib.Path(p).stat().st_size}
            except (FileNotFoundError,PermissionError):manifest['loadedFiles'][p]={'hashUnavailable':True}
    result['stdoutSha256']=sha(outlog);result['stderrSha256']=sha(errlog)
    raw=case/'results.json'
    if raw.exists():
        result['resultsSha256']=sha(raw)
        try:
            data=json.loads(raw.read_text())
            result.update({'state':data.get('state'),'sqliteVersion':data.get('sqliteVersion'),
                'sqliteSourceId':data.get('sqliteSourceId'),'qtVersion':data.get('qtVersion'),
                'actualJournal':data.get('actualJournal'),'synchronous':data.get('synchronous'),
                'integrityVerified':data.get('integrityVerified'),'rawSamples':len(data.get('samples',[])),
                'probeError':data.get('error')})
            valid=(result['exitCode']==0 and data.get('state')=='completed'
                and data.get('actualJournal')=='delete' and data.get('synchronous')==2
                and data.get('integrityVerified') is True and len(data.get('samples',[]))==35)
        except Exception as error:
            valid=False;result['parseError']=type(error).__name__
    else:valid=False;result['missingResults']=True
    result['valid']=valid
    manifest['caseResults'].append(result);save()
    print(f"case={ordinal:02d} repeat={repeat} mode={mode} cache={cache} exit={result['exitCode']} valid={valid} wall={result['wallSeconds']:.3f}s",flush=True)
    if not valid:
        manifest['state']='failed_case';save();break
else:manifest['state']='completed'
manifest['endUtc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
manifest['binarySha256After']=sha(binary);manifest['sourceSha256After']=sha(source)
manifest['inputIdentityUnchanged']=(manifest['binarySha256After']==manifest['binarySha256'] and manifest['sourceSha256After']==manifest['sourceSha256'])
save()
print('terminal='+manifest['state'],flush=True)
raise SystemExit(0 if manifest['state']=='completed' and manifest['inputIdentityUnchanged'] else 1)
