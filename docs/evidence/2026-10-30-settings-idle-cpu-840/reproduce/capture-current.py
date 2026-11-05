import pathlib,sys,time,json,datetime
sys.path.insert(0,'/home/user/lab-driver/tools/lab');import lab
run=pathlib.Path(sys.argv[1]);label=sys.argv[2];profile='/tmp/s840'+label.replace('-','');deadline=time.monotonic()+180
while True:
 try:shown=json.loads(lab.ssh(run,'cat '+profile+'/shown.json'));break
 except (RuntimeError,ValueError):
  if time.monotonic()>deadline:raise
  time.sleep(1)
time.sleep(max(0,shown['wallTime']+5-time.time()))
shot=lab.screenshot(run,label);value={'shown':shown,'screenshot':shot,'capturedAtUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'runId':json.loads((run/'state.json').read_text())['id']}
p=pathlib.Path('/home/user/840-'+label)/'visual.json';p.write_text(json.dumps(value,indent=2)+'\n');print(json.dumps(value),flush=True)
