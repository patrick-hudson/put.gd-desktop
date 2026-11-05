import os,sys,json,time,subprocess,hashlib
from pathlib import Path
R=Path.home()/'work/ico25'
sys.path.insert(0,str(R));from rfb import RFB
pid=subprocess.check_output(['pgrep','-x','cinnamon'],text=True).strip()
e={k.decode():v.decode() for x in Path('/proc/'+pid+'/environ').read_bytes().split(b'\0') if b'=' in x for k,v in [x.split(b'=',1)]}
for k in ('APPDIR','APPIMAGE','QT_QPA_PLATFORM'):e.pop(k,None)
for k,d in [('XDG_CONFIG_HOME','cfg'),('XDG_CACHE_HOME','cache'),('XDG_STATE_HOME','state'),('XDG_RUNTIME_DIR','rt')]:
 p=R/d;p.mkdir(mode=0o700,exist_ok=True);e[k]=str(p)
e['XDG_DATA_HOME']=str(Path.home()/'.local/share')
def cmd(a):return subprocess.run(a,env=e,text=True,capture_output=True)
cli=R/'squashfs-root/usr/bin/putgd-cli'
e['LD_LIBRARY_PATH']=str(R/'squashfs-root/usr/lib')+':'+str(R/'squashfs-root/usr/lib/putgd')
if sys.argv[1]=='launch':
 subprocess.run([sys.executable,str(R/'check_socket_paths.py'),'--socket',str(R/'rt/putgd/session.sock'),'/tmp/.X11-unix/X0','/run/user/1000/bus'],check=True)
 with (R/'launch.log').open('w') as log:
  p=subprocess.Popen([str(R/'squashfs-root/AppRun'),'--background'],env=e,stdout=log,stderr=log,start_new_session=True)
 (R/'launch.pid').write_text(str(p.pid));time.sleep(3)
 print(cmd([str(cli),'ui.show','{"view":"settings"}']).stdout)
elif sys.argv[1]=='frame':
 rows=cmd(['wmctrl','-lx']).stdout.splitlines();print('\n'.join(rows))
 window=next(x.split()[0] for x in rows if 'putgd-ui' in x.lower())
 cmd(['wmctrl','-ir',window,'-b','add,maximized_vert,maximized_horz']);cmd(['wmctrl','-ia',window]);time.sleep(1)
 print(cmd(['xprop','-id',window,'WM_CLASS','_GTK_APPLICATION_ID','_NET_WM_DESKTOP_FILE']).stdout)
 icon=cmd(['xprop','-id',window,'_NET_WM_ICON']).stdout
 print('window-icon-present', 'not found' not in icon, 'length',len(icon))
 # Only our maximized app plus its taskbar crop leaves the host.
 v=RFB(5900);w,h,b=v.frame();v.close()
 from PIL import Image
 im=Image.frombytes('RGBX',(w,h),b,'raw','BGRX').convert('RGB')
 im.crop((0,0,620,h)).save(R/(sys.argv[2]+'.png'))
 (R/(sys.argv[2]+'.json')).write_text(json.dumps({'windows':rows,'size':[w,h],'wmClass':cmd(['xprop','-id',window,'WM_CLASS']).stdout,'version':cmd([str(cli),'--version']).stdout,'session':{k:e.get(k) for k in ('DISPLAY','XDG_SESSION_TYPE','XDG_CURRENT_DESKTOP')},'iconPresent':'not found' not in icon},indent=2))
elif sys.argv[1]=='quit':print(cmd([str(cli),'ui.quit']).stdout)
