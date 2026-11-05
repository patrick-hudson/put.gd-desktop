import sys,subprocess,time,json
sys.path.insert(0,'/home/patrick/work/i559/tools/lab')
import native_acceptance as n
from rfb import RFB
facts,env=n.session()
p=subprocess.Popen([sys.executable,'/home/patrick/work/i559/tools/lab/cover_player.py','play'],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
try:
 print(p.stdout.readline().strip(),flush=True);time.sleep(1)
 c=RFB(5900);c.pointer(c.width-30,30);time.sleep(.2);w,h,pix=c.frame();c.close()
 rows=[]
 for y in range(h):
  xs=[x for x in range(w) if any(abs(a-b)>40 for a,b in zip(pix[(y*w+x)*4:(y*w+x)*4+3],(255,0,255)))]
  if len(xs)>20:rows.append((y,len(xs),min(xs),max(xs)))
 print(json.dumps({'badRows':len(rows),'samples':rows[::max(1,len(rows)//12)]}),flush=True)
finally:p.communicate('quit\n',timeout=10)
