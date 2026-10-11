#!/usr/bin/env python3
from pathlib import Path
import json,subprocess,time,shlex,xml.etree.ElementTree as ET
from common import OUT,RUN,now,save,sha,source,load_lab
lab=load_lab();m=source();guest='/home/lab/putgd-u11-'+m['commit'][:12];assert not (OUT/'build-receipt.json').exists();assert not json.loads((OUT/'build-wrapper-interruption.json').read_text())['guestBuildProcessesAfterInterruption']
lab.get(RUN,guest+'/dev.xml',OUT/'dev.xml');suite=ET.parse(OUT/'dev.xml').getroot();assert int(suite.attrib['tests'])==40 and int(suite.attrib['failures'])==0
commands=['cd '+shlex.quote(guest+'/source'),'cmake --build --preset release --target putgd putgd-cli putgd-ui putgd_s3_module putgd-desktop-fixture','cpack --config build/release/CPackConfig.cmake -G DEB -B '+shlex.quote(guest+'/packages-recovery')]
script='set -eu\n'+'\n'.join(commands)+'\n';(OUT/'recovery-build.sh').write_text(script);lab.put(RUN,OUT/'recovery-build.sh',guest+'/recovery-build.sh');started=time.monotonic();receipt={'schemaVersion':1,'startedUtc':now(),'commit':m['commit'],'guestDirectory':guest,'archiveSha256':m['archiveSha256'],'originalWrapperExit':143,'originalEvidence':'build-wrapper-interruption.json','commands':commands,'devTests':suite.attrib,'phase':'recovered standard release verification/package; native acceptance pending'}
with (OUT/'recovery-build.log').open('w') as log:result=subprocess.run(lab.ssh_args(RUN)+['sh '+shlex.quote(guest+'/recovery-build.sh')],stdout=log,stderr=subprocess.STDOUT)
receipt.update(exitCode=result.returncode,elapsedSeconds=time.monotonic()-started);save(OUT/'build-receipt.json',receipt);result.check_returncode()
remote=lab.ssh(RUN,'find '+shlex.quote(guest+'/packages-recovery')+" -maxdepth 1 -name '*.deb' -print\n").splitlines();assert len(remote)==1
package=OUT/Path(remote[0]).name;lab.get(RUN,remote[0],package);receipt['package']={'path':str(package),'sha256':sha(package)}
lab.get(RUN,guest+'/source/build/release/build-identity/build-info.json',OUT/'build-info.json');assert json.loads((OUT/'build-info.json').read_text())['gitCommit']==m['commit']
receipt['finishedUtc']=now();save(OUT/'build-receipt.json',receipt);print(json.dumps(receipt),flush=True)
