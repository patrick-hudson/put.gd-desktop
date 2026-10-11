import sys,pathlib,json,time,datetime
sys.path.insert(0,'/home/patrick/work/put.gd/tools/lab');import lab
r=pathlib.Path('/home/patrick/work/put.gd/work/lab-runs/current-kde-a3d70dc');o=pathlib.Path('/home/patrick/work/put.gd/work/cross-desktop-current/kde');results=[]
lab.key(r,['alt','f4']);time.sleep(.5)
lab.guest_session(r,"pkill -x putgd-desktop-f || true\npkill -f '^python3 /home/lab/putgd-lab/gtk_clipboard_fixture.py --output ' || true\n")
identity=lab.verify_running_app(r)
for cycle in (1,2):
 for view in ('settings','history','activity'):
  shown=lab.cli(r,'ui.show',{'view':view});time.sleep(.8)
  lab.pointer(r,640,22,1280,800,True);lab.pointer(r,640,22,1280,800,False);time.sleep(.2)
  opened=lab.screenshot(r,f'view-confirm-{cycle}-{view}-open')
  lab.key(r,['alt','f4']);time.sleep(.5)
  closed=lab.screenshot(r,f'view-confirm-{cycle}-{view}-closed')
  alive=lab.verify_running_app(r)
  results.append({'cycle':cycle,'view':view,'uiResponse':shown,'opened':opened,'closed':closed,'residentIdentityUnchanged':alive==identity})
(o/'views-smoke-confirmed.json').write_text(json.dumps({'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'sourceCommit':'a3d70dc2a6c6866fc6e38dd5e001553fb5c157a2','runningApp':identity,'actions':results,'automatedOk':all(x['residentIdentityUnchanged'] for x in results),'visualReview':'Pending direct witness review; production CLI opens each view and visible Alt+F4 hides it. No settings edited.'},indent=2)+'\n')
print('View activation/close sequence complete, identity unchanged:',all(x['residentIdentityUnchanged'] for x in results))
