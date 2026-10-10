#!/usr/bin/env python3
"""Reproduce only the fixed source53 numeric export. No runtime/guest actions."""
import hashlib,json,math,os,re,stat
from pathlib import Path
ROOT=Path('/home/patrick/work/put.gd')
HERE=Path(__file__).resolve().parent
OUT=HERE.parent
COMMIT='1ee42f32c61c3d93d88cc1d3dc624d9ad086bb70'
GATE=ROOT/'work/capture-parity/clean53/cinnamon-expansion-gate.json'
GATE_HASH='834071fcf867ac6be7a032a1e2a67228703046776820c854fef820387926fece'
BUILD_KEYS=('schemaVersion','version','baseVersion','packageVersion','gitCommit','gitTree','commitCount','dirty','sourceProvenance')
IDENTITY_NUM=('pid','startTicks','uid','device','inode','rssBytes','pssBytes','privateBytes','privateHugetlbBytes','swapBytes','cpuTicks','threads')
SAMPLE_NUM=('elapsedSeconds','sampleSeconds','processCount','uiProcessCount','escapedProcessCount','cpuTicks','rssBytes')
TOTALS=('rssBytes','pssBytes','privateBytes')
MEMORY_SUMMARIES=('finalTotals','peakSampledTotals','peakOpenTotals','finalClosedTotals','peakSampledRssBytesIncludingPrimary')


def require(v,code):
    if not v:raise ValueError(code)


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read(path,expected=None):
    p=Path(path);require(p.is_absolute() and not p.is_symlink() and stat.S_ISREG(p.lstat().st_mode),'regular_input_required')
    require(p.stat().st_size<=8*1024**2,'input_unbounded')
    raw=p.read_bytes();digest=hashlib.sha256(raw).hexdigest()
    require(expected is None or digest==expected,'original_hash_changed')
    return json.loads(raw),{'path':str(p),'sha256':digest}


def nums(d,keys):
    result={}
    for k in keys:
        if k in d:
            v=d[k];require(type(v) in (int,float) and math.isfinite(v),'nonfinite_or_nonnumeric')
            result[k]=v
    return result


def flags(d,keys):
    result={}
    for k in keys:
        if k in d:require(type(d[k]) is bool,'nonboolean');result[k]=d[k]
    return result


def hashes(d):
    require(isinstance(d,dict),'hash_object_required')
    return {k:v for k,v in d.items() if isinstance(v,str) and re.fullmatch('[0-9a-f]{64}',v)}


def build(d):
    require(isinstance(d,dict),'build_required')
    # Exact source53 metadata; no free-form fields or configuration exported.
    require(d.get('gitCommit')==COMMIT and d.get('commitCount')==53 and d.get('dirty') is False,'clean53_build_required')
    expected={'schemaVersion':1,'version':'0.1.1-dev.53+g1ee42f32c61c','baseVersion':'0.1.1',
      'packageVersion':'0.1.1~dev.53+g1ee42f32c61c','gitCommit':COMMIT,
      'gitTree':'e6584c6e2e002c2296855ad42b5dcd73e69a8ad7','commitCount':53,'dirty':False,'sourceProvenance':'git'}
    require(d==expected,'build_metadata_mismatch');return {k:d[k] for k in BUILD_KEYS}


def identity(d):
    result=nums(d,IDENTITY_NUM)
    if 'sha256' in d:require(re.fullmatch('[0-9a-f]{64}',d['sha256']),'invalid_hash');result['sha256']=d['sha256']
    if 'role' in d:require(d['role'] in ('resident','ui','capture','auxiliary','escaped','other'),'unknown_process_role');result['role']=d['role']
    result.update(flags(d,('inResidentTree',)))
    return result


def sample(d):
    result=nums(d,SAMPLE_NUM)
    if 'label' in d:
        require(re.fullmatch(r'(startup-warmup-\d+|cold-closed-after-warmup|cycle-[1-3]-(settings|history|activity)-open-\d+|cycle-[1-3]-closed-\d+)',d['label']),'sample_label_invalid')
        result['label']=d['label']
    if 'totals' in d:result['totals']=nums(d['totals'],TOTALS)
    if 'processes' in d:require(len(d['processes'])<=16,'processes_unbounded');result['processes']=[identity(x)for x in d['processes']]
    return result


def summary(d):
    result=nums(d,('sampleCount','medianActivationAcknowledgementMilliseconds','peakSampledRssBytesIncludingPrimary'))
    for k in MEMORY_SUMMARIES:
        if isinstance(d.get(k),dict):result[k]=nums(d[k],TOTALS)
    if 'coverage' in d:
        result['coverage']=nums(d['coverage'],('sampleSpanSeconds','maximumSampleGapSeconds','maximumAllowedGapSeconds','minimumSampleCount'))
        result['coverage'].update(flags(d['coverage'],('adequate',)))
    result.update(flags(d,('oneProcessThroughout','finalTotalRssAtOrBelow40MiB','coldIdleBudgetEstablished',
                           'cold40MiBTargetEstablished','renderedTrayAcceptanceEstablished')))
    return result


def ui(d):
    r=nums(d,('pid','checkpointBytes'))
    if 'state' in d:require(d['state'] in ('ready','stopped','starting'),'ui_state_unknown');r['state']=d['state']
    e=d.get('lastExit',{})
    r['lastExit']=nums(e,('generation','code'))
    if 'status' in e:require(e['status'] in ('normal','crash'),'exit_status_unknown');r['lastExit']['status']=e['status']
    return r


def parameters(d):
    r=nums(d,('seconds','interval','cycles','warmup_seconds','visible_seconds','quiet_seconds','timeout'))
    if 'views' in d:require(d['views']==['settings','history','activity'],'views_changed');r['views']=d['views']
    return r


def timing(d):
    r=nums(d,('schemaVersion','p95PaintMs','warmupInvocations'));r.update(flags(d,('budgetPassed',)))
    r['errorPresent']=bool(d.get('error'));r['samples']=[]
    for x in d['samples']:
        n=nums(x,('inputIssuedMs','mapMs','paintAfterInputMs','paintMs'))
        n['driverCosts']=nums(x['driverCosts'],('eventProcessingMs','hotkeyInjectionMs','inputInjectionMs','mapEventDrainMs',
               'mapOwnershipQueriesMs','paintQueriesMs','pollIterations','pollSleepMs'))
        n['windowWitnesses']=[]
        for w in x['windowWitnesses']:
            v=nums(w,('firstWhiteObservedMs','mapEventObservedMs','mapValidatedMs','ownerPid','probeCount','probeMaxMs','probeTotalMs'))
            require(re.fullmatch('[0-9]+',w['ownerStartTicks']),'owner_start_invalid');v['ownerStartTicks']=w['ownerStartTicks']
            n['windowWitnesses'].append(v)
        r['samples'].append(n)
    r['displays']=[nums(x,('height','width','scale','x','y'))for x in d['displays']]
    r['syntheticDrag']=nums(d['syntheticDrag'],('startX','startY','endX','endY'))
    return r


def operations(d):
    require(isinstance(d,list) and len(d)==35,'operations35_required')
    result=[]
    for x in d:
        require(re.fullmatch('[0-9a-f-]{36}',x.get('id','')) and x.get('state')=='cancelled'
                and x.get('copyStatus')=='no_result' and x.get('error',{}).get('code')=='cancelled',
                'cancelled_noresult_required')
        result.append({k:x[k]for k in ('id','state','copyStatus')})
    return {'operations':result}


def primary_cold(d):
    r=nums(d,('schemaVersion','warmupSeconds','initialRssBytes','elapsedSeconds','idleCpuPercentOneCore',
      'finalRssBytes','peakSampledRssBytes'));r.update(flags(d,('budgetPassed',)))
    r['appSha256']=hashes({'app':d['appSha256']})['app'];r['samples']=[sample(x)for x in d['samples']]
    for name in ('initialFootprint','finalFootprint'):
        require(len(d[name])<=2048,'footprint_unbounded')
        r[name+'Numeric']=[nums(row,('rssBytes','pssBytes','privateBytes'))for row in d[name]]
    return r


def family(d):
    require(d['sourceRevision']==COMMIT,'family_source_mismatch')
    r=nums(d,('schemaVersion','seconds','familyPollSeconds','observerExitCode','cleanupDiscoveryErrorCount'))
    r.update(flags(d,('passed','cold40MiBTargetEstablished','identitiesUnchanged')))
    r['sourceRevision']=d['sourceRevision'];r['observerSha256']=d['observerSha256'];r['harnesses']=hashes(d['harnesses'])
    r['samples']=[sample(x)for x in d['samples']];r['summary']=summary(d['summary'])
    r['residentIdentity']=identity(d['residentIdentity']);r['exitedProcesses']=[identity(x)for x in d['exitedProcesses']]
    r['observedResidentDescendants']=[identity(x)for x in d['observedResidentDescendants']]
    e=d['primaryEvidence'];r['primaryEvidence']=nums(e,('elapsedSeconds','idleCpuPercentOneCore','operationCount'))
    r['primaryEvidence'].update(flags(e,('legacyBudgetPassed','coldIdleUninterrupted')))
    r['primaryEvidence'].update(hashes({k:e[k]for k in ('resultsSha256','sidecarSha256')}))
    return r


def views(d):
    require(d['sourceRevision']==COMMIT,'view_source_mismatch')
    r=nums(d,('schemaVersion','sampleCount','operationCount','completeViewCycles','elapsedSeconds'))
    r.update(flags(d,('passed',)));r['parameters']=parameters(d['parameters']);r['sourceRevision']=d['sourceRevision']
    r['harnessSha256']=d['harnessSha256'];r['residentBuild']=build(d['residentBuild']);r['residentIdentity']=identity(d['residentIdentity'])
    app_hash=d['binaries']['app']['sha256'];require(re.fullmatch('[0-9a-f]{64}',app_hash),'view_app_hash_required')
    r['residentIdentity']['sha256']=app_hash
    r['residentIdentityHashSource']='binaries.app.sha256'
    r['samples']=[sample(x)for x in d['samples']];r['summary']=summary(d['summary']);r['events']=[]
    r['exitedProcesses']=[identity(x)for x in d['exitedProcesses']]
    for e in d['events']:
        require(e['kind'] in ('view-shown','all-views-closed'),'event_unknown')
        row=nums(e,('elapsedSeconds','cycle','acknowledgementMilliseconds','mappedReadyMilliseconds','closeMilliseconds'))
        row['kind']=e['kind']
        if 'view' in e:require(e['view']in('settings','history','activity'),'view_unknown');row['view']=e['view']
        row.update(flags(e,('acknowledgementIsShown','helperAbsent','helperExitRequired','residentAlive')))
        if 'ui' in e:row['ui']=ui(e['ui'])
        r['events'].append(row)
    return r


def supplement(d):
    require(d['sourceCommit']==COMMIT and d['sourceCount']==53,'supplement_source_mismatch')
    r=nums(d,('schemaVersion','sourceCount','exitCode','queryCount','elapsedSeconds'))
    r.update(flags(d,('passed','knownOwnedCleanupComplete')));r['sourceCommit']=d['sourceCommit']
    r['failurePresent']=d.get('failure') is not None;r['cleanupFailurePresent']=d.get('cleanupFailure') is not None
    r['rawFrozenResult']=d['rawFrozenResult'];r['cycles']=[]
    for x in d['cycles']:
        row=nums(x,('cycle','cliMilliseconds'));row['resident']=identity(x['resident'])
        row['socketPeer']=nums(x['socketPeer'],('pid','uid','gid','device','inode'))
        s=x['snapshot'];row['snapshot']={'build':build(s['build']),'ui':ui(s['ui'])}
        row['snapshot'].update(flags(s,('operationsReady','journalReady','trayAvailable')))
        row['snapshot'].update(nums(s,('activeOperations',)));r['cycles'].append(row)
    return r


def failed(d):
    r=nums(d,('schemaVersion','primaryDurationSeconds','primaryPeakRssMiB','primaryCpuPercentOneCore',
       'residentSampleCount','observedResidentDescendants','sampledSeconds','peakSampledFamilyRssBytes','sampleCount'))
    r.update(flags(d,('wholeFamilyAcceptance','primaryLegacyBudgetPassed','accepted','causeConfirmed','primaryCompleted')))
    r['sourceCommit']=d['sourceCommit'];require(re.fullmatch('[0-9a-f]{40}',r['sourceCommit']),'failed_source_invalid')
    require(d['errorType']in('FileNotFoundError','ProcessLookupError'),'failed_type_unknown');r['errorType']=d['errorType']
    r.update(hashes({k:d[k]for k in ('familySha256','primarySha256','sidecarSha256') if k in d}))
    if 'outputs' in d:r['outputFingerprints']=d['outputs']
    r['acceptanceQualifier']='Invalid collector attempt; low sampled RSS is not target proof. Diagnosis is not exact original-call attribution.'
    return r


def write(name,value,original=None):
    target=OUT/name;require(not target.exists(),'preserve_existing_export')
    if original:value={'projectionSchemaVersion':1,'transformed':True,'original':original,'data':value}
    target.write_text(json.dumps(value,indent=2,allow_nan=False)+'\n');os.chmod(target,0o644)
    return {'file':name,'sha256':sha(target),'bytes':target.stat().st_size,**({'original':original}if original else {})}


def main():
    gate,original=read(GATE,GATE_HASH);require(gate['passed'] is True and gate['sourceCommit']==COMMIT,'gate_not_passed')
    refs={r['path']:r['sha256']for r in gate['receipts']};require(len(refs)==96,'gate_reference_count_changed')
    manifest={'schemaVersion':1,'sourceCommit':COMMIT,'sourceCount':53,'transformed':True,'gateOriginal':original,
      'originalGateFingerprints':gate['receipts'],'exports':[],'qualifiers':[
      'Original view runs retain only normal-zero generations 1 and 2; generation 3 status is missing.',
      'Separate unchanged-workload supplements add three read-only queries per repeat and their own timing noise.',
      'No source54, GNOME/KDE, post-upload/populated-history, permanent plateau, mixed-DPI or full U9 certification.',
      'Transformed numeric exports omit raw config, paths/mappings, payloads, logs, source/binary bytes, argv and environment.']}
    def export(name,path,fn,gate_bound=True):
        p=Path(path);p=p if p.is_absolute() else ROOT/p
        require(not gate_bound or str(p)in refs,'raw_not_in_final_gate')
        raw,ori=read(p,refs.get(str(p)) if gate_bound else None)
        manifest['exports'].append(write(name,fn(raw),ori))
    export('timing.json','/tmp/pg53-cap1/results.json',timing)
    export('timing-parameters.json','/tmp/pg53-cap1/parameters.json',lambda d:{**nums(d,('schemaVersion','warmupInvocations','sampleCount','timeoutSeconds')),
      'harnessSha256':d['harnessSha256'],'binarySha256':hashes(d['binarySha256']),'captureHelperVersion':d['captureHelperVersion']})
    export('timing-operations.json','/tmp/pg53-cap1/operations.json',operations)
    for repeat in (1,2):
        export('cold-'+str(repeat)+'.json','/tmp/pg53-cold'+str(repeat)+'/results.json',primary_cold)
        export('family-'+str(repeat)+'.json','/tmp/pg53-family'+str(repeat)+'/results.json',family)
        for label,prefix in [('original','views'),('supplement','vex')]:
            export('views-'+label+'-'+str(repeat)+'.json','/tmp/pg53-'+prefix+str(repeat)+'/results.json',views)
            path=Path('/tmp/pg53-'+prefix+str(repeat)+'/samples.jsonl')
            require(path.stat().st_size<=8*1024**2,'samples_unbounded')
            original={'path':str(path),'sha256':sha(path)}
            rows=[sample(json.loads(line))for line in path.read_text().splitlines()];require(len(rows)==42,'view_sample_rows_changed')
            manifest['exports'].append(write('views-'+label+'-family-'+str(repeat)+'.json',{'samples':rows},original))
        export('view-exits-'+str(repeat)+'.json',ROOT/'work/capture-parity/clean53/view-exit-supplement'/('repeat'+str(repeat))/'results.json',supplement)
    for v in ('51','52'):
        export('failed-source'+v+'.json','work/capture-parity/clean'+v+'/cold1-failed-checkpoint.json',failed,False)
        p=ROOT/('work/capture-parity/clean'+v+'/cold1/result.json')
        data,ori=read(p);manifest['exports'].append(write('failed-source'+v+'-exit.json',nums(data,('exitCode',)),ori))
    gate_projection={k:gate[k] for k in ('schemaVersion','sourceCommit','sourceCount','version','passed','nativeFunctionalPassed',
                       'physicalTimingPassed','p95PaintMilliseconds','cold','views','normalExitSupplements','limitations')}
    manifest['exports'].append(write('gate-summary.json',gate_projection,manifest['gateOriginal']))
    for row in manifest['exports']:
        raw=row['original']
        row['binding']='final-gate-receipt' if raw['path'] in refs or raw==manifest['gateOriginal'] else ('prior-failed-checkpoint' if row['file'].startswith('failed-source') else 'additional-numeric-detail')
    for name in ('export.py','verify.py','test_verify.py'):
        manifest.setdefault('tools',[]).append({'file':'tools/'+name,'sha256':sha(HERE/name)})
    write('manifest.json',manifest)
    print(json.dumps({'exported':len(manifest['exports']),'bytes':sum(x['bytes']for x in manifest['exports']),
                      'sourceCommit':COMMIT,'runtimePerformed':False}))

if __name__=='__main__':main()
