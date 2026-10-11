#!/usr/bin/env python3
"""Build a bounded local receipt manifest after measurements, never payload export."""
from pathlib import Path
import hashlib,json,datetime,xml.etree.ElementTree as ET
HERE=Path(__file__).resolve().parent
def read(p):return json.loads(p.read_text())
def ref(p):
 p=p.resolve();assert p.is_file() and p.name not in ('credentials.txt','password')
 return {'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
source=read(HERE.parent/'source.json');started=read(HERE/'start.json');installed=read(HERE/'installed.json');matrix=read(HERE/'matrix.json')
assert source['commit']==started['build']['gitCommit']==installed['manifest']['gitCommit']=='676f6e6d6e0b026dcdd1b4ef2ae7dabc8a29ded3'
assert started['resident']['sha256']==installed['binarySha256']['/usr/bin/putgd']
assert matrix['proceduralPassed'] and len(matrix['cases'])==12 and all(c['result']['ok'] for c in matrix['cases'])
views={}
for name in ('settings','history','activity'):
 shown=[p for p in HERE.glob('attempts/*/shown.json') if read(p)['view']==name];assert len(shown)==1
 closed=[p for p in HERE.glob('attempts/*/closed.json') if Path(read(p)['witness']['path']).name==name+'-closed.png'];assert len(closed)==1
 c=read(closed[0]);assert c['helperAfter']['pid']==0 and c['helperAfter']['lastExit']['status']=='normal' and c['helperAfter']['lastExit']['code']==0 and c['residentUnchanged']==started['resident']
 views[name]={'shown':ref(shown[0]),'closed':ref(closed[0]),'openWitness':ref(HERE/'views'/f'{name}-open.png'),'closedWitness':ref(HERE/'views'/f'{name}-closed.png'),'review':'Reviewed actual rendered native view by core_clipboard_owner; root review separate','helperNormalExit':True}
mem=list(HERE.glob('attempts/*/memory.json'));maps=list(HERE.glob('attempts/*/mapping-summary.json'));assert len(mem)==len(maps)==1
memory=read(mem[0]);result=memory['result'];assert memory['parameters']['durationSeconds']==300 and len(result['samples'])==61
assert all(s['processCount']==1 and s['uiProcessCount']==0 and s['escapedProcessCount']==0 for s in result['samples'])
failures=[ref(p) for p in HERE.glob('attempts/*/failed.json')]
commands=[]
for p in HERE.glob('attempts/*/*.command.json'):
 commands.append(ref(p))
tests=ET.parse(HERE/'dev.xml').getroot();assert tests.attrib['failures']=='0'
summary={'schemaVersion':1,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'guest':'kde-wayland','sourceCommit':source['commit'],'version':started['build']['version'],'resident':started['resident'],'vncPort':5934,'scope':'Single-head local-only current source49 Qt/GTK native regression, actual views and post-use quiescent family measurement; unchanged graphical fallback','source':ref(HERE.parent/'source.json'),'build':ref(HERE/'build.json'),'tests':{'receipt':ref(HERE/'dev.xml'),'count':int(tests.attrib['tests']),'failures':int(tests.attrib['failures'])},'installed':ref(HERE/'installed.json'),'captureHelperInstall':ref(HERE/'capture-helper-install.json'),'start':ref(HERE/'start.json'),'old46Stop':ref(HERE/'stopped.json'),'matrix':{'receipt':ref(HERE/'matrix.json'),'passed':12,'cases':[{'client':c['client'],'scenario':c['scenario'],'result':c['result']['ok'],'finalWitness':{'path':c['result']['witness']['path'],'sha256':c['result']['witness']['sha256']}} for c in matrix['cases']]},'views':views,'memory':{'receipt':ref(mem[0]),'context':ref(HERE/'measurement-context.json'),'samples':61,'peakRssMiB':result['peakRssBytes']/1048576,'finalRssMiB':result['finalRssBytes']/1048576,'finalPssMiB':result['finalPssBytes']/1048576,'finalPrivateMiB':result['finalPrivateBytes']/1048576,'cpuPercentOneCore':result['cpuPercentOneCore']},'mappingSummary':ref(maps[0]),'preservedFailures':failures,'failureDiagnosis':'Fresh helper close attempted while the VM had auto-locked during build; inactive witness and timeout preserved; visible masked-field restore and distinctly labeled normal close succeeded','failureRecovery':{'diagnosis':[ref(p) for p in HERE.glob('attempts/*/diagnosis.json')],'unlockedWitness':ref(HERE/'views/post-build-unlocked.png'),'freshNormalClose':[ref(p) for p in HERE.glob('attempts/*/closed.json') if Path(read(p)['witness']['path']).name=='fresh-start-unlocked-closed.png']},'commandMetadataHashes':commands,'limits':{'coldMemoryCertification':False,'fortyMiBCertification':False,'fullDesktopProfileCertification':False,'latencyPassed':False,'integrationKeepClaim':False,'remoteStorageTested':False,'allMatrixPixelsReviewed':False,'mappingCategories':'Basename heuristic; not anonymous allocation call-site attribution'},'collectors':{'familyRunner':ref(Path('/home/patrick/work/put.gd/work/resident-native-boundary/cross-desktop-replay.py')),'socketPreflightMatrix':ref(HERE/'run-preflighted-matrix.py'),'mapping':ref(HERE/'collect-mapping-baselines.py'),'summary':ref(Path(__file__))}}
out=HERE/'summary.json';assert not out.exists();out.write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({'summary':ref(out),'memory':summary['memory'],'tests':summary['tests']}))
