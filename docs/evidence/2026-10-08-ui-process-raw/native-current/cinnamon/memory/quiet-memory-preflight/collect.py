#!/usr/bin/env python3
from pathlib import Path
import importlib.util,json,hashlib,shlex,datetime
root=Path('/home/patrick/work/put.gd');spec=importlib.util.spec_from_file_location('lab',root/'tools/lab/lab.py');lab=importlib.util.module_from_spec(spec);spec.loader.exec_module(lab)
for desktop,guestoutput in [('cinnamon','/home/lab/putgd-lab/u11-memory-post-native-97ca3a5'),('kde','/home/lab/putgd-u11-97ca3a521714/memory-observation')]:
 out=root/'work/u11/native'/desktop/'memory';out.mkdir(mode=0o700);run=root/'work/lab-runs'/('current-'+desktop+'-a3d70dc')
 for name in ['results.json','progress.json','samples.jsonl']:lab.get(run,guestoutput+'/'+name,out/name)
 if desktop=='cinnamon':
  for guestname,localname in [('receipt.json','quiet-guard.json'),('operation-counts.jsonl','quiet-guards.jsonl'),('sampler.stdout','observer.log'),('sampler.stderr','observer.stderr')]:lab.get(run,guestoutput+'-guard/'+guestname,out/localname)
  mapping='/home/lab/putgd-lab/u11-mapping-after-sample.py';idle='/home/lab/putgd-lab/u11-bench-idle.py';mappingoutput='/home/lab/putgd-lab/u11-resident-mappings.json'
  lab.put(run,root/'work/u11/native/kde/mapping-after-sample.py',mapping);lab.put(run,root/'tests/performance/bench_idle.py',idle)
  args=['python3',mapping,'--results',guestoutput+'/results.json','--bench-idle',idle,'--bench-idle-sha256',lab.hash_file(root/'tests/performance/bench_idle.py'),'--output',mappingoutput]
 else:
  for guestname,localname in [('quiet-operation-monitor.json','quiet-guard.json'),('memory-driver.log','observer.log')]:lab.get(run,'/home/lab/putgd-u11-97ca3a521714/'+guestname,out/localname)
  args=json.loads((root/'work/u11/native/kde/memory-prepared.json').read_text())['postObservationMappingCommand'];mappingoutput='/home/lab/putgd-u11-97ca3a521714/resident-mappings.json'
 lab.ssh(run,shlex.join(args)+'\n');lab.get(run,mappingoutput,out/'footprint.json');(out/'footprint-command.json').write_text(json.dumps({'command':args,'scope':'Separate post-interval attribution only'},indent=2)+'\n')
 print(desktop,'collected',flush=True)
