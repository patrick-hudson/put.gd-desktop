from pathlib import Path
from types import SimpleNamespace
import replay,json,subprocess,hashlib
root=replay.ROOT;candidate=root/'work/view-startup-latency/exp001';out=replay.HERE/'lazy-exp001-native'
head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=candidate,text=True).strip();assert head=='9d142919e5e6365227a4424212380e92f85285fe'
paths=['src/ui/SettingsWindow.cpp','src/ui/SettingsWindow.h','tests/gui/test_settings.cpp','tests/gui/test_ui_application.cpp']
changed=subprocess.check_output(['git','diff','--name-only'],cwd=candidate,text=True).splitlines();assert sorted(changed)==sorted(paths)
r=replay.Replay(SimpleNamespace(output=str(out),guest=None,phase='freeze',commit=head));r.freeze()
s=replay.read(out/'source.json');patch=out/'candidate.patch';patch.write_bytes(subprocess.check_output(['git','diff','--binary','HEAD','--',*paths],cwd=candidate));bundle=out/'source.bundle';r.command(['git','bundle','create',str(bundle),'HEAD'],cwd=candidate,timeout=120)
files={p:replay.sha(candidate/p) for p in paths}
for p,h in files.items():s['files'][p]['sha256']=h
s['candidate']={'worktree':str(candidate),'patch':str(patch),'patchSha256':replay.sha(patch),'bundle':str(bundle),'bundleSha256':replay.sha(bundle),'files':files,'dirty':True,'comparison':'Base44 committed tree plus exact four-path overlay, not relabelled as clean44.'}
# Preserve the original base manifest; candidate snapshot has its own marker.
(out/'base-source.json').write_bytes((out/'source.json').read_bytes());(out/'source.json').write_text(json.dumps(s,indent=2)+'\n');assert replay.read(out/'source.json')==s
inventory={'guests':{}}
for name in replay.GUESTS:
 a=replay.Replay(SimpleNamespace(output=str(out),guest=name,phase='prepare-current-source43-stop'))
 current=replay.Replay(SimpleNamespace(output=str(replay.HERE/'source43-attempt1'),guest=name,phase='candidate-wave-readonly-inventory'))
 resident,d=current.assert_current();assert not d['diagnostics']['ui'].get('pid');assert current.cli('diagnostics')['activeOperations']==0
 inventory['guests'][name]={'guest':{'processes':[{'comm':'putgd',**resident}],'manifest':d['build']}}
replay.save(out/'preparation-inventory.json',inventory)
replay.save(out/'prepared.json',{'candidate':s['candidate'],'sourceCommit':head,'existingOverlaysOnly':True,'nativeAcceptance':'pending','inventoryVerified':True})
print(json.dumps({'output':str(out),'baseCommit':head,'patchSha256':s['candidate']['patchSha256'],'files':len(files)}))
