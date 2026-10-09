from pathlib import Path
import sys, importlib.util, argparse,time,json
p=Path('/home/patrick/work/put.gd/work/ui-refinement/native-replay/replay.py')
spec=importlib.util.spec_from_file_location('replay',p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
a=argparse.Namespace(output=str(p.parent/'lazy-exp001-native'),guest='cinnamon',phase='diagnostic-region')
r=m.Replay(a);r.assert_current(); assert r.cli('diagnostics')['activeOperations']==0
s=r.cli('settings.get');before=r.cli('operation.recent');ids={x['id'] for x in before['operations']}
patch={'actions':{x:{'destination':'local','shortcut':f'Ctrl+Shift+{i}'} for i,x in enumerate(('region','window','desktop','clipboard'),1)}}
reply=r.cli('settings.patch',{'expectedRevision':s['metadata']['acceptedRevision'],'patch':patch})
m.save(r.attempt/'configuration.json',{'patch':patch,'reply':reply,'scope':'synthetic local only diagnostic'})
m.lab.pointer(r.run,150,182,1280,800,True);time.sleep(.2);m.lab.pointer(r.run,150,182,1280,800,False);time.sleep(.2)
m.lab.key(r.run,['ctrl','shift','1']);time.sleep(1)
m.save(r.attempt/'selector.json',{'diagnostics':r.cli('diagnostics'),'operations':r.cli('operation.recent'),'nativeWindows':r.session('command -v xwininfo\nxwininfo -root -tree\n'),'witness':r.witness('diagnostic-selector-ready')})
m.lab.pointer(r.run,150,182,1280,800,True);time.sleep(.2)
m.lab.pointer(r.run,230,242,1280,800);time.sleep(.2)
m.save(r.attempt/'drag.json',{'pointer':{'start':[150,182],'end':[230,242],'viewport':[1280,800],'pressPauseSeconds':.2,'movePauseSeconds':.2},'witness':r.witness('diagnostic-selector-drag')})
m.lab.pointer(r.run,230,242,1280,800,False);time.sleep(.2)
deadline=time.monotonic()+15
op=None
while time.monotonic()<deadline:
 found=[x for x in r.cli('operation.recent')['operations'] if x['id'] not in ids]
 assert len(found)<=1
 if found:
  op=r.cli('operation.inspect',{'id':found[0]['id']})
  if op['state'] in m.TERMINAL:break
 time.sleep(.2)
m.save(r.attempt/'result.json',{'operation':op,'diagnostics':r.cli('diagnostics'),'witness':r.witness('diagnostic-region-result'),'boundedDiagnostic':True})
print(json.dumps({'attempt':str(r.attempt),'operationId':op['id'] if op else None,'state':op['state'] if op else None,'copyStatus':op.get('copyStatus') if op else None}))
