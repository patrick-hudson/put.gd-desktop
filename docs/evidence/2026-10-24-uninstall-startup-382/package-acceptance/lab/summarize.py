"""Reduce verified lifecycle receipts to a sanitized reviewable result; no new app runs."""
from pathlib import Path
import json,sys,hashlib
COMMIT='d5b561ddb94f8255b8bc754c1382552d983a6fa9'
host,mode,artifact=sys.argv[1:4];root=Path('work/i382n/evidence')/host/mode

selection=json.loads((root/'receipt-selection.json').read_text()) if (root/'receipt-selection.json').exists() else {}
def receipt(name):
 selected=selection.get(name,name+'.jsonl');assert Path(selected).name==selected
 d=json.loads((root/selected).read_text());assert d['exit']==0,d
 return json.loads(d['stdout'])
completion=json.loads((root/'completion.json').read_text());assert completion['sourceCommit']==COMMIT and completion['functionalPassed']
review=json.loads((root/'visual-review.json').read_text());assert review['reviewed'] and review['accepted']
identity=receipt('identity');login=receipt('check-present-enabled');ui=receipt('status-frame-login-settings');build=login['snapshot']['build'];assert build['gitCommit']==COMMIT and build['dirty'] is False
memory=receipt('memory-180');phases={}
for phase in ['inactive','active','disabled']:
 d=receipt('check-absent-'+phase);assert d['pass'] and d['startupJournalErrors']==[]
 phases[phase]={'sessionId':d['sessionId'],'session':d['session'],'residentAbsent':not d['residents'],'entryAbsent':d['entry'] is None,'scriptAbsent':d['scriptSha256'] is None,'savedPreference':d['preference'],'startupJournalErrors':d['startupJournalErrors'],'startupJournal':d['journal'],'failedUnits':d['failedUnits']}
 if phase in ('inactive','active'):
  a=receipt('retention-after-'+phase);assert a['unchanged'];phases[phase]['retention']=a
ui_processes=[p for p in ui['residents'] if Path(p['exe']).name=='putgd-ui'];assert ui_processes
platform='wayland' if login['session']['Type']=='wayland' else 'xcb'
assert all(any(('libq'+platform) in v for v in p['qtPlatformPlugins']) for p in ui_processes),ui_processes
artifact=Path(artifact);download=json.loads((artifact/'download-receipt.json').read_text());package=next(p for p in artifact.iterdir() if p.suffix in ('.deb','.rpm','.AppImage'));digest=hashlib.file_digest(package.open('rb'),'sha256').hexdigest();assert digest==download['members'][package.name]['sha256']
launch=receipt('launch-initial');assert launch['installedPackageVerification']['exit']==0
summary={'scope':'Targeted Linux managed startup-link uninstall lifecycle; development CI artifacts, not a stable-release campaign','target':host,'mode':mode,'build':build,'sourceTree':build['gitTree'],'artifact':{'file':package.name,'sha256':digest,'bytes':package.stat().st_size,'ciRun':download['run'],'artifactId':download['artifactId']},'os':identity['os'],'desktopVersions':identity['versions'],'kernel':identity['kernel'],'displayEnvironment':identity['displayEnvironment'],'socketPreflight':launch['socketPreflight'],'installedPackageVerification':launch['installedPackageVerification'],'enabledLogin':{'sessionId':login['sessionId'],'session':login['session'],'pass':login['pass'],'startupJournalErrors':login['startupJournalErrors'],'residents':login['residents']},'uiProcesses':ui_processes,'memory':memory,'memoryAssessment':completion['memoryAssessment'],'removalPhases':phases,'activeRemovalObservation':receipt('wait-removed'),'explicitDisable':receipt('disable')['snapshot']['diagnostics']['loginStartup'],'cleanup':receipt('cleanup'),'exportReceipt':json.loads((root/'export-receipt.json').read_text()),'visualReview':review,'completion':completion}
(root/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(host,mode,'sanitized summary verified')
