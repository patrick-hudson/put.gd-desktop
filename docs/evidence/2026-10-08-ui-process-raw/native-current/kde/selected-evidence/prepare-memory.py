#!/usr/bin/env python3
"""Prepare a verified quiet observation command; does not execute the benchmark."""
import json,shlex
from pathlib import Path
from common import OUT,RUN,now,save,sha,source,load_lab,assert_resident,ui
lab=load_lab();manifest=source();identity=assert_resident(lab);state=ui(lab);assert state['pid']==0 and state['state']=='stopped'
acceptance=OUT/'receipt.json';native=json.loads(acceptance.read_text());assert native['source']['commit']==manifest['commit'];assert native['functionalWaveComplete']
build=json.loads((OUT/'build-receipt.json').read_text());guest=build['guestDirectory'];uid=int(lab.ssh(RUN,'id -u\n').strip());guest_acceptance=guest+'/native-acceptance.json'
lab.put(RUN,acceptance,guest_acceptance)
benchmark=guest+'/source/tests/performance/bench_ui_process.py';output=guest+'/memory-observation'
command=['python3',benchmark,'--mode','split','--app','/usr/bin/putgd','--helper','/usr/bin/putgd-ui','--cli','/usr/bin/putgd-cli','--socket',f'/run/user/{uid}/putgd/session.sock','--source-revision',manifest['commit'],'--source-archive',guest+'/source.tar','--acceptance-receipt',guest_acceptance,'--pid',str(identity['pid']),'--start-ticks',str(identity['startTicks']),'--seconds','120','--interval','5','--label','kde-after-u11-functional-wave','--output',output]
receipt={'timestampUtc':now(),'status':'prepared; not executed; root quiescence signal required','sourceCommit':manifest['commit'],'resident':identity,'ui':state,'acceptanceReceiptSha256':sha(acceptance),'benchmarkSha256':sha(Path('/home/patrick/work/put.gd/tests/performance/bench_ui_process.py')),'guestCommand':command,'sshCommand':lab.ssh_args(RUN)+[shlex.join(command)],'guestOutput':output,'parameters':{'durationSeconds':120,'intervalSeconds':5,'identity':'PID/start ticks, executable SHA, source revision and socket SO_PEERCRED','scope':'Total observed process family; quiet post-functional wave, not physical-machine budget proof'}}
attribution=OUT/'mapping-after-sample.py';lab.put(RUN,attribution,guest+'/mapping-after-sample.py')
receipt['postObservationMappingCommand']=['python3',guest+'/mapping-after-sample.py','--results',output+'/results.json','--bench-idle',guest+'/source/tests/performance/bench_idle.py','--bench-idle-sha256',sha(Path('/home/patrick/work/put.gd/tests/performance/bench_idle.py')),'--output',guest+'/resident-mappings.json']
receipt['postObservationMappingScriptSha256']=sha(attribution)
monitor=OUT/'monitor-quiet.py';lab.put(RUN,monitor,guest+'/monitor-quiet.py')
monitor_plan={'samplerCommand':command,'resident':identity,'socket':f'/run/user/{uid}/putgd/session.sock','cli':'/usr/bin/putgd-cli','monitorOutput':guest+'/quiet-operation-monitor.json','driverLog':guest+'/memory-driver.log'}
save(OUT/'guest-memory-plan.json',monitor_plan);lab.put(RUN,OUT/'guest-memory-plan.json',guest+'/memory-plan.json')
receipt['quietMonitorScriptSha256']=sha(monitor);receipt['quietMonitorCommand']=['python3',guest+'/monitor-quiet.py','--plan',guest+'/memory-plan.json'];receipt['sshCommand']=lab.ssh_args(RUN)+[shlex.join(receipt['quietMonitorCommand'])]
receipt['parameters']['operationGuard']='Operation IDs/count/terminal/copy states before, every 5 seconds and after; changes invalidate only this observation; app and GUI stay running'
save(OUT/'memory-prepared.json',receipt);print(json.dumps(receipt))
