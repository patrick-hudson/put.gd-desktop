"""Export owned bounded acceptance evidence with hashes before VM retirement."""
import sys,subprocess,shlex,io,tarfile,hashlib,json
from pathlib import Path
import control,guest_control
host,mode=sys.argv[1:3];home='/home/patrick' if host in control.HOSTS else '/home/lab';root=home+'/work/p382n/'+mode
code="import pathlib,json,hashlib; r=pathlib.Path("+repr(root)+"); print(json.dumps({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in r.iterdir() if p.is_file() and p.suffix in ('.json','.log')}))"
fn=control.run if host in control.HOSTS else guest_control.guest
receipt=fn(host,['python3','-c',code]);assert not receipt['exit'],receipt;expected=json.loads(receipt['stdout']);assert expected
args=['tar','-C',root,'-cf','-',*sorted(expected)]
if host in control.HOSTS:command=control.ssh(host)+[shlex.join(args)]
else:
 ssh=['ssh','-i',guest_control.BASE+host+'/identity','-o','IdentitiesOnly=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+guest_control.BASE+host+'/known_hosts','-o','BatchMode=yes','-p',str(guest_control.GUESTS[host]),'lab@127.0.0.1',shlex.join(args)]
 command=control.ssh('f44')+[shlex.join(ssh)]
r=subprocess.run(command,capture_output=True,timeout=120);assert not r.returncode,r.stderr
out=Path('work/i382n/evidence')/host/mode/'remote';assert not out.exists();out.mkdir()
with tarfile.open(fileobj=io.BytesIO(r.stdout)) as tar:
 for member in tar.getmembers():
  assert member.isfile() and member.name in expected and Path(member.name).name==member.name,member.name
  data=tar.extractfile(member).read();assert hashlib.sha256(data).hexdigest()==expected[member.name]
  (out/member.name).write_bytes(data)
assert set(expected)=={p.name for p in out.iterdir()}
(out.parent/'export-receipt.json').write_text(json.dumps({'host':host,'mode':mode,'remoteRoot':root,'verifiedFiles':expected,'allHashesVerified':True},indent=2)+'\n');print(host,mode,'exported',len(expected),'verified files')
