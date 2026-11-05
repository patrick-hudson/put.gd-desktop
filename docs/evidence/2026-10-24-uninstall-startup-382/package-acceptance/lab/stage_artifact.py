"""Verify exact CI artifact identity before copying its package to an acceptance host."""
import sys,json,hashlib,subprocess,shlex
from pathlib import Path
import control,guest_control
COMMIT='d5b561ddb94f8255b8bc754c1382552d983a6fa9';TREE='797721fa0672581562a8675d0d2010eced7c6bd4'
folder=Path(sys.argv[1]);targets=sys.argv[2].split(',')
build=json.loads((folder/'build-info.json').read_text());assert build['gitCommit']==COMMIT and build['gitTree']==TREE and build['dirty'] is False,build
files=[p for p in folder.iterdir() if p.suffix in ('.deb','.rpm','.AppImage')];assert len(files)==1,files
p=files[0];digest=hashlib.file_digest(p.open('rb'),'sha256').hexdigest();receipt=json.loads((folder/'download-receipt.json').read_text());assert receipt['run']==40068126465 and receipt['members'][p.name]['sha256']==digest
if p.suffix!='.AppImage':
 audit=json.loads((folder/'native-audit.json').read_text())['package'];assert audit['file']==p.name and audit['sha256']==digest and audit['size']==p.stat().st_size,audit
rows=[]
for host in targets:
 dest=('/home/patrick' if host in control.HOSTS else '/home/lab')+'/work/p382n/'+p.name
 if host in control.HOSTS:
  r=subprocess.run(control.ssh(host)+['cat > '+shlex.quote(dest)],input=p.read_bytes(),capture_output=True,timeout=120);assert not r.returncode,r.stderr
  check=control.run(host,['sha256sum',dest],timeout=30)
 else:
  # Binary stdin passes through authenticated workstation->builder->guest SSH.
  key=guest_control.BASE+host+'/identity';pin=guest_control.BASE+host+'/known_hosts'
  args=['ssh','-i',key,'-o','IdentitiesOnly=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+pin,'-o','BatchMode=yes','-p',str(guest_control.GUESTS[host]),'lab@127.0.0.1','cat > '+shlex.quote(dest)]
  r=subprocess.run(control.ssh('f44')+[shlex.join(args)],input=p.read_bytes(),capture_output=True,timeout=120);assert not r.returncode,r.stderr
  check=guest_control.guest(host,['sha256sum',dest],timeout=30)
 assert not check['exit'] and check['stdout'].split()[0]==digest,check
 rows.append({'host':host,'remotePath':dest,'sha256':digest,'build':build,'artifactRun':receipt['run'],'artifactId':receipt['artifactId']});print(host,dest,'verified',flush=True)
(folder/('upload-'+sys.argv[2].replace(',','-')+'.json')).write_text(json.dumps(rows,indent=2)+'\n')
