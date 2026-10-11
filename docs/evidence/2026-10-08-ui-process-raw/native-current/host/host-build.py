#!/usr/bin/env python3
from pathlib import Path
import json,hashlib,subprocess,tarfile,time
out=Path(__file__).resolve().parent;m=json.loads((out/'source.json').read_text());archive=Path(m['archive']);assert hashlib.sha256(archive.read_bytes()).hexdigest()==m['archiveSha256']
host=out/'host';src=host/'source';src.mkdir(parents=True,exist_ok=False)
subprocess.run(['tar','-xf',str(archive),'-C',str(src)],check=True)
checks=[]
def call(name,argv):
 t=time.monotonic()
 with (host/(name+'.log')).open('w') as log:r=subprocess.run(argv,cwd=src,stdout=log,stderr=subprocess.STDOUT)
 checks.append({'label':name,'argv':argv,'exitCode':r.returncode,'elapsedSeconds':time.monotonic()-t})
 (host/'commands.json').write_text(json.dumps(checks,indent=2)+'\n');r.check_returncode()
sdk=Path('/home/patrick/work/put.gd/build')
call('configure',['cmake','--preset','release',f'-DCMAKE_PREFIX_PATH={sdk}/sdk',f'-DPUTGD_AWS_RUNTIME_DIR={sdk}/sdk/lib',f'-DPUTGD_AWS_SOURCE_DIR={sdk}/dependencies/aws-sdk-cpp','-DPUTGD_LAB_FIXTURE=ON','-DCMAKE_INSTALL_PREFIX=/usr',f'-DPUTGD_SOURCE_COMMIT={m["commit"]}',f'-DPUTGD_SOURCE_TREE={m["tree"]}',f'-DPUTGD_SOURCE_COMMIT_COUNT={m["commitCount"]}'])
call('build',['cmake','--build','--preset','release','--target','putgd','putgd-cli','putgd-ui','putgd_s3_module','putgd-desktop-fixture'])
call('package',['cpack','--config','build/release/CPackConfig.cmake','-G','DEB','-B',str(host/'packages')])
packages=list((host/'packages').glob('*.deb'));assert len(packages)==1
manifest=json.loads((src/'build/release/build-identity/build-info.json').read_text());assert manifest['gitCommit']==m['commit']
products={}
for name in ('putgd','putgd-cli','putgd-ui'):
 p=src/'build/release'/name;v=subprocess.check_output([str(p),'--version'],text=True).strip();assert v==name+' '+manifest['version'];products[name]={'path':str(p),'version':v,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
receipt={'status':'built','sourceCommit':m['commit'],'archiveSha256':m['archiveSha256'],'manifest':manifest,'executables':products,'package':{'path':str(packages[0]),'sha256':hashlib.sha256(packages[0].read_bytes()).hexdigest()},'commands':checks,'nativeAcceptance':'pending'}
(host/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt))
