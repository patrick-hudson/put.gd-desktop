#!/usr/bin/env python3
from pathlib import Path
import datetime,hashlib,json,xml.etree.ElementTree as ET
ROOT=Path('/home/patrick/work/put.gd')
OUT=ROOT/'work/ui-refinement/native-replay/nativecore-exp001-c49-cross'
P=OUT/'gnome'
def read(p):return json.loads(Path(p).read_text())
def ref(p):
 p=Path(p).resolve();assert p.is_file()
 assert p.name not in ('credentials.txt','password')
 return {'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size}
def refs(paths):return [ref(p) for p in sorted(paths)]
def one(pattern):
 found=list(P.glob(pattern));assert len(found)==1,(pattern,len(found));return found[0]
source=read(OUT/'source.json');build=read(P/'build.json');installed=read(P/'installed.json');started=read(P/'start.json');matrix=read(P/'matrix.json')
assert source['commit']==build['sourceCommit']==started['build']['gitCommit']=='676f6e6d6e0b026dcdd1b4ef2ae7dabc8a29ded3'
assert installed['manifest']==started['build']==build['manifest']
assert started['resident']['sha256']==installed['binarySha256']['/usr/bin/putgd']
assert matrix['proceduralPassed'] and len(matrix['cases'])==12 and all(c['result']['ok'] for c in matrix['cases'])
assert all(c['result']['runningApp']==started['resident'] for c in matrix['cases'])
xml=ET.parse(P/'dev.xml').getroot();assert xml.attrib['tests']=='47' and xml.attrib['failures']=='0' and xml.attrib['skipped']=='0'
views=[]
for name in ('settings','history','activity'):
 shown=next(p for p in P.glob('attempts/*-show/shown.json') if read(p)['view']==name)
 sd=read(shown)
 closed=next(p for p in P.glob('attempts/*-close/closed.json') if read(p)['witness']['path'].endswith('/native-'+name+'-close.png'))
 cd=read(closed);assert cd['helperBefore']==sd['helper'] and cd['residentUnchanged']==started['resident']
 assert cd['helperAfter']['pid']==0 and cd['helperAfter']['lastExit']['status']=='normal' and cd['helperAfter']['lastExit']['code']==0
 views.append({'view':name,'helper':sd['helper'],'shown':ref(shown),'closed':ref(closed),'openWitness':ref(sd['witness']['path']),'closeWitness':ref(cd['witness']['path']),'normalExitCode':0,'visualObservation':'Native GNOME chrome and blue application controls painted correctly; no corruption in reviewed witness','renderReviewScope':'Current-version screenshot and ordinary close, no full UI feature certification'})
memoryPath=one('attempts/*-memory/memory.json');memory=read(memoryPath);r=memory['result'];assert memory['parameters']['durationSeconds']==300 and memory['parameters']['resident']==started['resident']
assert len(r['samples'])==61 and all(s['processCount']==1 and s['uiProcessCount']==0 and s['escapedProcessCount']==0 for s in r['samples'])
smapsPath=one('attempts/*-smaps-attribution/smaps-attribution.json');smaps=read(smapsPath);assert smaps['resident']==started['resident'] and smaps['sourceCommit']==source['commit']
failures=[]
for p in sorted(P.glob('attempts/*/failed.json')):
 d=read(p);failure={'receipt':ref(p),'phase':d['phase'],'errorType':d['errorType']}
 if d['phase']=='unlock-wake':failure['diagnosis']='Default sandbox denied existing QMP Unix socket access before any input; explicit sandbox escalation succeeded on the unchanged wake command.'
 elif d['phase']=='configure':failure['diagnosis']='First-start Settings helper was open; guard rejected before any settings write. Normally closed helper then unchanged configure command passed.'
 else:raise AssertionError('Unexpected failure needs diagnosis')
 failure['attemptFiles']=refs(x for x in p.parent.iterdir() if x.is_file())
 failures.append(failure)
assert len(failures)==2
visual={'schemaVersion':1,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'reviewer':'Independent delegated GNOME replay agent','matrixProceduralPassed':True,'nativeWitnesses':refs((P/'views/matrix').glob('*.png')),'observations':['All 12 retained final native witnesses show the independent Qt or GTK synthetic fixture correctly painted on the GNOME desktop.','These final witnesses show post-paste or post-cancel state; they do not establish overlay appearance or capture latency.','Settings, populated History and populated Activity have native chrome and blue controls with no paint corruption in the reviewed witnesses.','Activity table text truncates within its current columns at 1280x800; this is recorded visually, without treating it as a new candidate49 regression.'],'limits':['Single-head local-only synthetic actions.','No remote provider operation, capture latency or full compositor-profile certification.','Display became inactive/locked during the long build; inactive fresh-start witness is preserved and visibly reviewed re-unlock precedes all matrix acceptance.']}
with (P/'visual-review.json').open('x') as f:json.dump(visual,f,indent=2)
receiptFiles=[P/n for n in ('build.json','build-info.json','dev.xml','installed.json','install.json','start.json','capture-helper-install.json','stopped.json','previous-state.json','matrix.json','configure-retry-reason.json','visual-review.json')]
receiptFiles += [OUT/'source.json',OUT/'prepared.json',Path(source['archive']),Path(build['package'])]
commands=refs(P.glob('attempts/*/*.command.json'))+refs(P.glob('attempts/*/request.json'))
commands+=refs(P.glob('reunlock-after-build/gnome/attempts/*/*.command.json'))+refs(P.glob('reunlock-after-build/gnome/attempts/*/request.json'))
summary={'schemaVersion':1,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'guest':'gnome-wayland','sourceCommit':source['commit'],'version':installed['manifest']['version'],'resident':started['resident'],'installedBinarySha256':installed['binarySha256'],'vncPort':5936,'scope':'Installed candidate49 current-source GNOME Wayland single-head local-only regression and post-native-use quiescent VM memory observation. Graphical fallback unchanged; no ephemeral Cinnamon capture helper used.','build':{'toolchain':'Normal distro C++20/Qt6 Widgets CMake presets and Ninja; dynamically linked distro Qt; verified pinned official AWS C++ SDK','devTests':{'total':47,'passed':47,'failed':0,'skipped':0,'raw':ref(P/'dev.xml')},'devReleaseAndPackageCompleted':True},'functional':{'matrixCases':12,'passed':12,'failed':0,'matrix':ref(P/'matrix.json'),'views':views,'visualReview':ref(P/'visual-review.json')},'memory':{'scope':memory['parameters']['scope'],'durationSeconds':300,'sampleCount':len(r['samples']),'peakRssBytes':r['peakRssBytes'],'finalRssBytes':r['finalRssBytes'],'finalPssBytes':r['finalPssBytes'],'finalPrivateBytes':r['finalPrivateBytes'],'cpuPercentOneCore':r['cpuPercentOneCore'],'familyCount':1,'uiAndCaptureHelpersDuringSampling':0,'parameters':ref(memoryPath.parent/'memory-parameters.json'),'rawResult':ref(memoryPath),'basenameMappingAttribution':ref(smapsPath),'mappingGroups':smaps['groups'],'anonymousAttributionLimit':'Anonymous mappings alone do not identify payload ownership or allocator cause; no forced allocator release or environment change was made.'},'claims':{'fortyMiBGuestCertification':False,'latencyPassed':False,'keepIntegrationOrReleaseAcceptance':False,'fullProfileCertification':False},'prerequisite':ref(ROOT/'work/ui-refinement/native-replay/nativecore-exp001-c49/cinnamon/gates.json'),'receipts':refs(receiptFiles),'commandAndRequestHashes':commands,'preservedFailures':failures,'visibleRestore':{'maskedFocus':ref(P/'views/console-wake-after-build.png'),'unlockedCurrentSettings':ref(P/'reunlock-after-build/gnome/views/console-after-unlock.png'),'inactiveStartPreserved':ref(P/'views/fresh-start.png')},'scratchCollectorAndWrapper':refs([P/'matrix-preflight.py',P/'collect-smaps.py',Path(__file__)])}
with (P/'summary.json').open('x') as f:json.dump(summary,f,indent=2)
print(json.dumps({'summary':ref(P/'summary.json'),'version':summary['version'],'cases':12,'viewsNormalExit':3,'memory':{k:v for k,v in summary['memory'].items() if k.endswith('Bytes') or k in ('durationSeconds','sampleCount','cpuPercentOneCore')}}))
