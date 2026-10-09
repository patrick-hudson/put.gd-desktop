python3 - <<'PY'
import hashlib,json,pathlib,subprocess
items=[]
for name in ('putgd','putgd-ui','putgd-cli'):
 path=pathlib.Path('/usr/bin')/name
 result=subprocess.run([str(path),'--version'],capture_output=True,text=True,check=True)
 text=result.stdout.strip()
 if not text.startswith(name+' '):raise RuntimeError('Unexpected version envelope')
 items.append({'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
               'version':text.split(' ',1)[1],'versionOutput':text,'exitCode':result.returncode})
print(json.dumps(items))
PY
