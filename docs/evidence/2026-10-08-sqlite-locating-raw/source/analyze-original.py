import hashlib,json,math,pathlib,shutil,statistics
root=pathlib.Path('/tmp/putgd-sqlite-latency-rollback-20261008T193548Z-ee5c51ea')
out=pathlib.Path('/home/patrick/work/put.gd/work/sqlite-latency-probe-evidence')
out.mkdir(mode=0o700,exist_ok=True);(out/'raw').mkdir(mode=0o700,exist_ok=True)
manifest=json.loads((root/'manifest.json').read_text())
shutil.copy2(root/'manifest.json',out/'raw'/'manifest.json')
def stats(values):
    return {'count':len(values),'min':min(values),'median':statistics.median(values),
     'p95NearestRank':sorted(values)[math.ceil(.95*len(values))-1],'max':max(values)}
rows=[]
for case in manifest['caseResults']:
    rawpath=pathlib.Path(case['output'])/'results.json'
    data=json.loads(rawpath.read_text())
    target=out/'raw'/pathlib.Path(case['output']).name;target.mkdir(mode=0o700,exist_ok=True)
    shutil.copy2(rawpath,target/'results.json')
    for key in ('stdout','stderr'):shutil.copy2(root/case[key],out/'raw'/case[key])
    scored=[s for s in data['samples'] if not s['warmup']]
    names=sorted(set(k for s in scored for p in s['phases'] for k in p if k.endswith('Ns')))
    phase={k:stats([sum(p.get(k,0) for p in s['phases'])/1e6 for s in scored]) for k in names}
    memory=dict(data['memory']);memory['firstScanRssGrowthKiB']=memory['rssAfterScanKiB']-memory['rssBeforeKiB']
    memory['shrinkRssDropKiB']=memory['rssAfterScanKiB']-memory['rssAfterShrinkKiB']
    memory['rescanRssGrowthFromInitialKiB']=memory['rssAfterRescanKiB']-memory['rssBeforeKiB']
    reader=data['controlledLock']['readerThenWriter'];writer=data['controlledLock']['writer']
    rows.append({'ordinal':case['ordinal'],'repeat':case['repeat'],'mode':case['mode'],'cacheKiB':case['cacheKiB'],
     'sourceResultSha256':case['resultsSha256'],'wallMs':stats([s['wallNs']/1e6 for s in scored]),
     'phaseSumMsPerThreeStateIteration':phase,'memory':memory,
     'controlledLock':{'injectedHoldMs':100,'writerHeldThroughCommitMs':(writer['releasedNs']-writer['heldStartNs'])/1e6,
      'readerUserVersionMs':reader['userVersionNs']/1e6,'readerOpenMs':reader['openNs']/1e6,
      'readerPermissionsMs':reader['permissionsNs']/1e6,'followingExecuteMs':reader['executeNs']/1e6,
      'writerCommitMs':writer['commitNs']/1e6},
     'stderrBytes':(root/case['stderr']).stat().st_size,'valid':case['valid']})
growth={str(cache):stats([r['memory']['firstScanRssGrowthKiB'] for r in rows if r['cacheKiB']==cache]) for cache in (2000,512)}
summary={'schemaVersion':1,'runRoot':str(root),'runSession':37398,'runExitCode':0,'caseCount':len(rows),
 'rawSamples':sum(c['rawSamples'] for c in manifest['caseResults']),'scoredSamples':len(rows)*30,
 'order':manifest['plannedOrder'],'allCasesValid':all(r['valid'] for r in rows),
 'elapsedUtc':[manifest['startUtc'],manifest['endUtc']],'binarySha256':manifest['binarySha256'],
 'sourceSha256':manifest['sourceSha256'],'cmakeSha256':manifest['cmakeSha256'],
 'sqliteIdentity':{'version':data['sqliteVersion'],'sourceId':data['sqliteSourceId'],'qtVersion':data['qtVersion'],'compileOptions':data['compileOptions']},
 'libraryIdentity':manifest['loadedFiles'],'firstScanGrowthKiBByCache':growth,'rows':rows,
 'findings':[
 'Commit is the dominant tail in these simplified rollback/FULL serial cases; retained connections/prepared statement reuse do not consistently improve whole iteration timing.',
 'Reopen healthy setup is sub-millisecond across three states; prepare cost is sub-millisecond and disappears as an explicit prepare phase under bounded statement reuse.',
 'The controlled exclusive writer reproduces a user_version wait while open/chmod stay short. It is injected lock overlap, not the observed product fsync stall.',
 'The 512 KiB cap lowers normalized first-scan RSS in both repeats; product paginated-history memory/latency remains unmeasured.',
 'shrink_memory reduced RSS in one of twelve cases, and not in eleven; RSS is not SQLite allocation accounting. No allocator trim tested.'
 ],
 'limits':[
 'No product, compositor, clipboard, user history/config/data, WAL or physical idle gate exercised; no 150 ms or 40 MiB acceptance claim.',
 'Thirty scored iterations and two reversed-order repeats are locating evidence with filesystem tails, not a selected product winner.',
 'QSqlQuery scan/result allocations and glibc retention can influence process RSS; sqlite3_db_status was not measured.',
 'The prepared source reused reader beginNs for the following write begin duration; controlled-lock absolute reader-start timestamp is unavailable. Individual phase durations and writer start/commit/release remain usable; source was not changed or rerun.',
 'SQLite compile options do not list ENABLE_MEMORY_MANAGEMENT; optional global sqlite3_release_memory would therefore not be a supported plan. shrink_memory connection-local API remains available.',
 'Fresh same-mode connection does not by itself remove a competing writer; subsequent product accepted/acquiring writes can still wait.'
 ],
 'identityUnchanged':manifest['inputIdentityUnchanged'],'runtimeQuiet':True}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
files={}
for path in (out/'raw').rglob('*'):
    if path.is_file():files[str(path.relative_to(out))]={'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
files['summary.json']={'bytes':(out/'summary.json').stat().st_size,'sha256':hashlib.sha256((out/'summary.json').read_bytes()).hexdigest()}
(out/'raw-hashes.json').write_text(json.dumps(files,indent=2)+'\n')
print(json.dumps({'summarySha256':files['summary.json']['sha256'],'firstScanGrowthKiBByCache':growth,
 'medianPrepareRangeMs':[min(r['phaseSumMsPerThreeStateIteration']['prepareNs']['median'] for r in rows if r['mode']!='statements'),
 max(r['phaseSumMsPerThreeStateIteration']['prepareNs']['median'] for r in rows if r['mode']!='statements')],
 'lockVersionRangeMs':[min(r['controlledLock']['readerUserVersionMs'] for r in rows),max(r['controlledLock']['readerUserVersionMs'] for r in rows)],
 'lockOpenMaxMs':max(r['controlledLock']['readerOpenMs'] for r in rows),
 'lockPermissionsMaxMs':max(r['controlledLock']['readerPermissionsMs'] for r in rows),
 'shrinkDropsKiB':[r['memory']['shrinkRssDropKiB'] for r in rows]}))
