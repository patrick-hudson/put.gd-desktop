import sys,stat,os,time
from pathlib import Path
from PIL import Image
sys.path.insert(0,'/home/patrick/work/put.gd/work/i33/tools/lab');import lab
r=Path(sys.argv[1]).resolve();a=sys.argv[2]
if a=='shot':
 p=Path(lab.screenshot(r,sys.argv[3])['path']);Image.open(p).save(p.with_suffix('.png'));print(p.with_suffix('.png'))
elif a=='key':lab.key(r,sys.argv[3:])
elif a=='click':
 x,y=map(int,sys.argv[3:]);lab.pointer(r,x,y,1280,800,True);time.sleep(.1);lab.pointer(r,x,y,1280,800,False)
elif a=='unlock':
 profile=lab.state(r)['profile'];folder={'cinnamon-x11':'install','gnome-wayland':'ubuntu-install','kde-wayland':'kubuntu-install'}[profile];p=Path('/home/patrick/work/put.gd/work/lab-bootstrap')/folder/'password';s=p.lstat();assert stat.S_ISREG(s.st_mode) and s.st_uid==os.getuid() and s.st_mode&0o077==0
 value=p.read_text().rstrip('\n');shifted={'+':'equal','_':'minus',':':'semicolon','?':'slash','!':'1','@':'2','#':'3','$':'4','%':'5','^':'6','&':'7','*':'8','(':'9',')':'0'};plain={' ':'spc','-':'minus','/':'slash','.':'dot','=':'equal',',':'comma',';':'semicolon'}
 for c in value:lab.key(r,['shift',shifted.get(c,plain.get(c,c.lower()))] if c.isupper() or c in shifted else [plain.get(c,c.lower())])
 value=None;lab.key(r,['ret']);print('Protected login input entered in the visible masked field.')
