from pathlib import Path
import datetime, hashlib, importlib.util, json, subprocess, time
ROOT=Path('/home/patrick/work/put.gd')
OUT=ROOT/'work/u11/native/kde'
RUN=ROOT/'work/lab-runs/current-kde-a3d70dc'
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path,value):Path(path).write_text(json.dumps(value,indent=2)+'\n')
def source():return json.loads((OUT.parent/'source.json').read_text())
def load_lab(require_freeze=True):
 if require_freeze:
  manifest=source()['files']
  for name in ['tools/lab/lab.py','tools/lab/gtk_clipboard_fixture.py','tools/lab/profiles/kde-wayland.yaml','tests/desktop/common.yaml']:
   assert sha(ROOT/name)==manifest[name]['sha256'],f'Frozen source differs: {name}'
 spec=importlib.util.spec_from_file_location('u11_kde_lab',ROOT/'tools/lab/lab.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
 info=module.state(RUN)
 assert info['profile']=='kde-wayland' and info['ssh_port']==22234 and info['vnc_display']==34
 assert Path(info['overlay'])==RUN/'disk.qcow2'
 return module
def ui(lab):
 snapshot=lab.cli(RUN,'ui.snapshot')
 return snapshot['diagnostics']['ui']
def assert_resident(lab):
 expected=json.loads((OUT/'install.json').read_text())['runningApp']
 assert lab.verify_running_app(RUN)==expected
 return expected
def wait_ui(lab,state,timeout=15):
 deadline=time.monotonic()+timeout
 while True:
  value=ui(lab)
  if value['state']==state:return value
  if time.monotonic()>=deadline:raise RuntimeError(f'UI did not reach {state}: {value}')
  time.sleep(.1)
def helper_identity(lab,pid):
 script='''import pathlib,hashlib,json,os
p=pathlib.Path('/proc')/str(%d)
assert p.stat().st_uid==os.getuid()
a=(p/'stat').read_text().rsplit(')',1)[1].split()
out={'pid':int(p.name),'parentPid':int(a[1]),'startTicks':int(a[19]),'sha256':hashlib.sha256((p/'exe').read_bytes()).hexdigest()}
assert out['startTicks']==int((p/'stat').read_text().rsplit(')',1)[1].split()[19])
print(json.dumps(out))
'''%pid
 result=subprocess.run(lab.ssh_args(RUN)+['python3','-'],input=script,text=True,capture_output=True,check=True)
 value=json.loads(result.stdout)
 assert value['parentPid']==assert_resident(lab)['pid']
 package=json.loads((RUN/'package.json').read_text())
 assert value['sha256']==package['installedBinarySha256']['/usr/bin/putgd-ui']
 return value
