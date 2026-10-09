python3 - <<'PY'
import hashlib,json,pathlib,shutil
root=pathlib.Path.home()/'putgd-lab/fixture'
assert (root/'ready.json').is_file()
size=512*1024*1024
free=shutil.disk_usage(root).free
assert free>3*size+512*1024*1024
path=root/'synthetic-1.txt'
original=path.read_bytes();assert original==b'putgd synthetic file 1\n'
block=(b'putgd U11 synthetic continuity\n'*40000)[:1024*1024]
assert len(block)==1024*1024
digest=hashlib.sha256()
with path.open('wb') as stream:
 for index in range(size//len(block)):
  stream.write(block);digest.update(block)
print(json.dumps({'path':str(path),'size':size,'sha256':digest.hexdigest(),'freeBeforeBytes':free,
                  'streamBlockBytes':len(block),'blockPattern':'putgd U11 synthetic continuity newline'}))
PY
