import json,os,subprocess,sys
from pathlib import Path
from tools.lab import desktop_lock
row,host,run=sys.argv[1:4]
attempt=os.environ.get('TIER1_ATTEMPT','a')
desktop,guest={'cin':('mint-cinnamon-x11','cinnamon-x11'),'gno':('ubuntu-gnome-wayland','gnome-wayland'),'kde':('kubuntu-kde-wayland','kde-wayland')}[row]
root=Path('/home/patrick/t1r2e');session='codex-tier1b-r2-6801919c'
env=dict(os.environ,PUTGD_TIER1_SESSION=session)
lock=desktop_lock.Desktop(Path.home()/'putgd-lab/locks',host)
base=[sys.executable,'-m','tools.lab.surface_driver.run','--desktop',desktop,'--host',host,'--placement','guest','--guest',guest,'--run',run,'--package','deb','--catalog','/home/patrick/t1cat','--build','cc9d9410d6e982dd93d58bcfda82f112c44a82f6','--profiles','/home/patrick/t1profiles','--campaign','tier1-phase-b-rerun']
for dest in (sys.argv[4:] or ['s3','proton']):
 lock.heartbeat(session)
 short='r'+row[0]+dest[0]+attempt
 with (root/(row+'-'+dest+attempt+'-sockets.json')).open('x') as log:
  subprocess.run([sys.executable,'tools/diagnostics/check_socket_paths.py','--socket','/dev/shm/'+short+'/runtime/putgd/session.sock'],stdout=log,check=True)
 command=base+['--destination',dest,'--recipes','outcomes_live.'+dest+'_region_upload','--session-root','/dev/shm/'+short,'--out',str(root/(row+'-'+dest+attempt)),'--coverage',str(root/(row+'-'+dest+'-coverage.json'))]
 if dest=='proton':
  with (root/(row+'-proton-check-before-'+attempt+'.json')).open('x') as log:
   subprocess.run([sys.executable,'tools/lab/proton_lab.py','check','--session',session,'--owner','tier1-phase-b'],env=env,stdout=log,stderr=subprocess.DEVNULL,check=True)
  command=[sys.executable,'tools/lab/proton_lab.py','lend',run,'--session',session,'--owner','tier1-phase-b','--purpose','Corrected Tier1 Phase B real region and browser byte proof','--']+command
 with (root/(row+'-'+dest+attempt+'.log')).open('x') as log:
  result=subprocess.run(command,env=env,stdout=log,stderr=log)
 print(json.dumps(dict(row=row,destination=dest,controllerExit=result.returncode)),flush=True)
 if dest=='proton':
  with (root/(row+'-proton-final-check-'+attempt+'.json')).open('x') as log:
   result=subprocess.run([sys.executable,'tools/lab/proton_lab.py','check','--session',session,'--owner','tier1-phase-b'],env=env,stdout=log,stderr=subprocess.DEVNULL)
  print(json.dumps(dict(row=row,finalCheckExit=result.returncode)),flush=True)
  if result.returncode:raise SystemExit(2)
