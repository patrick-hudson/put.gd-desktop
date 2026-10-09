from pathlib import Path
import subprocess,json,hashlib,datetime
root=Path('/home/patrick/work/put.gd/work/view-startup-latency/exp001');out=root/'work/base45-root-validation';commands=[]
for name,argv in [('configure',['cmake','--preset','dev']),('build',['cmake','--build','--preset','dev']),('tests',['ctest','--preset','dev','-R','^(gui_tray|gui_settings|gui_application|settings_contract|ui_process_integration|ui_split_services_integration)$','--output-junit',str(out/'tests.xml')])]:
 commands.append({'name':name,'argv':argv});(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 with (out/(name+'.stdout')).open('w') as a,(out/(name+'.stderr')).open('w') as b:r=subprocess.run(argv,cwd=root,stdout=a,stderr=b)
 (out/(name+'.result.json')).write_text(json.dumps({'exitCode':r.returncode,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}));r.check_returncode()
print('Base45 six affected host suites passed')
