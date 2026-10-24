"""Console login only after matching previously inspected blank GDM screens."""
import sys,time,stat,os,json
from pathlib import Path
from PIL import Image,ImageChops,ImageStat
from full_rfb import RFB
OUT=Path('work/i382n')
def pil(frame):
 w,h,b=frame;return Image.frombytes('RGB',(w,h),b,'raw','BGRX')
def match(current,reference,box):
 diff=ImageChops.difference(current.crop(box),Image.open(OUT/reference).convert('RGB').crop(box))
 return sum(ImageStat.Stat(diff).mean)/3<3
label=sys.argv[1];r=RFB(5992);r.key('shift');r.close();r=None;time.sleep(.5)
end=time.monotonic()+45
while time.monotonic()<end:
 if r is not None:r.close()
 r=RFB(5992);f=r.frame();im=pil(f)
 if match(im,'u382a-masked-login.png',(450,100,820,560)):break
 if match(im,'u382a-greeter-2.png',(460,300,820,430)):
  r.key('ret');time.sleep(.7);continue
 time.sleep(1)
else:raise RuntimeError('GDM did not reach the inspected blank masked field; no secret read')
# Match excludes the clock; entire inspected login widget and blank masked field match.
r.screenshot(str(OUT/('u382a-'+label+'-blank-masked-login.png')))
p=Path('/home/patrick/work/put.gd/work/lab-bootstrap/ubuntu-install/password');st=p.lstat();assert stat.S_ISREG(st.st_mode) and st.st_uid==os.getuid() and stat.S_IMODE(st.st_mode)==0o600
secret=p.read_text().rstrip('\r\n');assert secret and '\n' not in secret;r.type(secret);r.key('ret');del secret;r.close()
# Inspect subsequent frames only in memory until the login widget has gone.
# Never save a frame containing entered protected input, even when masked.
time.sleep(2);end=time.monotonic()+60;desktop_frames=0
while time.monotonic()<end:
 r=RFB(5992);im=pil(r.frame());r.close()
 login=match(im,'u382a-masked-login.png',(450,100,820,560)) or match(im,'u382a-greeter-2.png',(460,300,820,430))
 if login:desktop_frames=0
 else:desktop_frames+=1
 if desktop_frames>=3:break
 time.sleep(1)
else:raise RuntimeError('Guest console did not leave the login screen; no post-input frame saved')

(OUT/('u382a-'+label+'-console-login.json')).write_text(json.dumps({'maskedFieldMatchedBeforeRead':True,'protectedInputOwnedPrivateRegular':True,'valueRecorded':False,'consoleLeftLoginScreenBeforeEvidence':True,'at':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())})+'\n')
print(label,'protected input entered in matched masked guest console field')
