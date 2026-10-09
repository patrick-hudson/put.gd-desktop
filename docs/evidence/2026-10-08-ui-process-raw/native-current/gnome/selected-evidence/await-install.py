from pathlib import Path
import json,subprocess,sys,time
p=Path(__file__).resolve().parent;deadline=time.monotonic()+3600
while True:
 r=p/'build-receipt.json'
 if r.exists():
  b=json.loads(r.read_text())
  if b['exitCode']:raise RuntimeError('Build failed; install withheld')
  if b.get('package'):break
 if time.monotonic()>deadline:raise RuntimeError('Build receipt deadline; install withheld')
 time.sleep(3)
with (p/'install-wrapper.log').open('w') as f:r=subprocess.run([sys.executable,str(p/'install.py')],stdout=f,stderr=subprocess.STDOUT)
(p/'install-exit').write_text(str(r.returncode)+'\n');print('Install phase exit',r.returncode,flush=True);sys.exit(r.returncode)
