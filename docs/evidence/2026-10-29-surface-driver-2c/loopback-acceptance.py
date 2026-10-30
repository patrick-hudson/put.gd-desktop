"""Boat-only synthetic loopback privacy acceptance; no provider/session inputs."""
import dataclasses,hashlib,inspect,io,json,os,shlex,subprocess,sys,time
from pathlib import Path
from tools.lab import lab
from tools.lab.surface_driver.api import Recipe
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.receipts import write_json
from tools.lab.surface_driver.redact import scan_run
from tools.lab.surface_driver.run import run_recipes
from tools.lab.surface_driver.target import Target,checked_socket
run=Path(sys.argv[1]); out=Path(sys.argv[2]); source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
target=Target(run=run)
checked_socket(target.socket);checked_socket(target.root+'/notify.sock')
out.mkdir(mode=0o700)
env=dict(desktop='mint-cinnamon-x11',host='boat',placement='boat',guest='cinnamon-x11',package='source',destination='local',build=source,
         target=target.identity(),placement_details={'boat_sandbox_id':'bx_qtz97pf4','run_id':target.info['id'],'run_directory':str(run),'input_backend':'qmp','frame_backend':'vnc-in-memory'},
         synthetic_only=True,driver_source=source)
env['socket_preflight']=json.loads(subprocess.check_output([sys.executable,'tools/diagnostics/check_socket_paths.py','--socket',str(run/'qmp.sock'),target.socket,target.root+'/notify.sock'],text=True))
profile=load_profile(env['desktop']);profile['evidence']={'browser_address_bars':[{'rect':[0,0,4096,160],'head':0}]}
target.start()
ctx=DriverContext(target,env,profile,Path('work/empty-catalog'),out)
env['observed_build']=ctx.call('ui.snapshot').get('build',{})
env['build']=env['observed_build']['gitCommit']
ctx.call('ui.quit');target.started=False
env['resident_stopped_for_browser_fixture']=True
ctx.shell('mkdir -p '+shlex.quote(target.root)+'/runtime '+shlex.quote(target.root)+'/firefox')
server=r'''
import http.server,json,secrets
url='http://127.0.0.1:18891/share/'+secrets.token_urlsafe(24)+'#'+secrets.token_urlsafe(24)
seen=False
page=''' + repr(b'''<!doctype html><title>Synthetic share fixture</title><style>body{background:#eef4f7;font:24px sans-serif}input{position:absolute;left:40px;top:160px;width:1000px;height:60px;font-size:22px}h1{position:absolute;top:460px;left:40px;font-size:26px}p{position:absolute;top:540px;left:40px}</style><h1>Synthetic share fixture ready</h1><label for="share">Synthetic share link</label><input id="share" aria-label="Synthetic share link"><p>Only synthetic data. Link field and browser toolbar must be masked.</p><script>document.querySelector('input').value=location.href</script>''') + r'''
class H(http.server.BaseHTTPRequestHandler):
 def do_GET(self):
  global seen
  if self.path.startswith('/share/'):seen=True
  content=json.dumps({'url':url,'seen':seen}).encode() if self.path=='/state' else page
  self.send_response(200);self.send_header('Content-Type','application/json' if self.path=='/state' else 'text/html');self.end_headers();self.wfile.write(content)
 def log_message(self,*args):pass
http.server.HTTPServer(('127.0.0.1',18891),H).serve_forever()
'''
server_pid=int(ctx.shell('nohup python3 -c '+shlex.quote(server)+' >/dev/null 2>&1 </dev/null & echo $!')['stdout'])
prefs='user_pref("dom.security.https_first", false);\nuser_pref("dom.security.https_first_pbm", false);\nuser_pref("dom.security.https_only_mode", false);\nuser_pref("dom.security.https_only_mode_pbm", false);\nuser_pref("browser.privatebrowsing.autostart", true);\nuser_pref("browser.cache.disk.enable", false);\nuser_pref("browser.shell.checkDefaultBrowser", false);\nuser_pref("browser.aboutwelcome.enabled", false);\nuser_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);\nuser_pref("browser.startup.homepage_override.mstone", "ignore");\n'
ctx.shell("python3 -c "+shlex.quote('from pathlib import Path;Path('+repr(target.root+'/firefox/user.js')+').write_text('+repr(prefs)+')'))
browser_pid=int(ctx.shell('nohup firefox --no-remote --profile '+shlex.quote(target.root+'/firefox')+' --private-window about:blank >/dev/null 2>&1 </dev/null & echo $!')['stdout'])
try:
 windows=ctx.wait(lambda:ctx.shell("xdotool search --onlyvisible --class firefox 2>/dev/null || true")['stdout'].split(),'private Firefox X11 window',60)
 ctx.expect(len(windows)==1 and windows[0].isdigit(),'one private Firefox window before input')
 wid=windows[0]
 def bounds():
  values=dict(line.split('=',1) for line in ctx.shell('xdotool getwindowgeometry --shell '+wid)['stdout'].splitlines() if '=' in line)
  return [int(values[k]) for k in ('X','Y','WIDTH','HEIGHT')]
 bx,by,bw,bh=bounds();ctx.click_at(bx+bw//2,by+14);ctx.keys('Alt+F10');time.sleep(1)
 x,y,w,h=bounds()
 ctx.profile['evidence']['browser_address_bars']=[{'rect':[x,y,w,160],'head':0}]
 ctx.sensitive_region([x,y+210,w,200])
 raw=json.loads(ctx.shell("python3 -c \"import urllib.request;print(urllib.request.urlopen('http://127.0.0.1:18891/state').read().decode())\"")['stdout'])['url']
 ctx.sensitive(raw)
 env['fixture_url']=ctx.url_evidence(raw)
 env['equal_url_hash_verified']=ctx.url_evidence(raw)['hash']==env['fixture_url']['hash']
 def share(c):
  for _ in range(5):
   if c.shell('xdotool getactivewindow')['stdout'].strip()==wid:break
   c.keys('Alt+Tab');time.sleep(.2)
  c.expect(c.shell('xdotool getactivewindow')['stdout'].strip()==wid,'private Firefox owns foreground before URL typing')
  c.keys('Ctrl+L');c.type_text(raw);c.keys('Return')
  c.note('foreground verified and synthetic URL entered')
  def requested():
   state=json.loads(c.shell("python3 -c \"import urllib.request;print(urllib.request.urlopen('http://127.0.0.1:18891/state').read().decode())\"")['stdout'])
   return state['seen']
  c.wait(requested,'synthetic loopback share page requested',30)
  c.note('loopback server confirmed page request')
  c.wait(lambda:'Synthetic share fixture' in c.shell('xdotool getwindowname '+wid)['stdout'],'synthetic page title loaded',25)
  c.note('expected synthetic page title verified')
  time.sleep(1)
  region=[x,y+210,w,200];window_geometry=dict(x=x,y=y,width=w,height=h,scale=1)
  c.geometry=lambda role:window_geometry
  copied=''
  for attempt in range(3):
   for _ in range(5):
    if c.shell('xdotool getactivewindow')['stdout'].strip()==wid:break
    c.keys('Alt+Tab');time.sleep(.2)
   c.expect(c.shell('xdotool getactivewindow')['stdout'].strip()==wid,'private Firefox owns foreground before copy')
   c.keys('Ctrl+L','Ctrl+C');time.sleep(.2)
   copied=c.clipboard_text()
   env['clipboard_copy_attempts']=attempt+1
   env['clipboard_url']=c.url_evidence(copied)
   if copied==raw:break
  c.expect(copied==raw,'address bar equals loopback URL in memory')
  c.keys('Escape');c.click_at(1100,500)
  c.frame('share-link-full-desktop');c.frame('share-link-window',window='fixture-browser')
  c.note('synthetic URL equality verified; host and salted hash retained only')
  env['link_field_rect']=region;env['browser_window_rect']=[x,y,w,h];env['rectangle_source']='measured X11 window and explicit synthetic fixture CSS band'
 rows=run_recipes(ctx,[Recipe(('driver.secret-evidence',),share,desktop_drawn=True)],env,out,'secret-2c',out/'coverage.json')
 result=json.loads((out/'receipt.json').read_text())
 cscan=scan_run(out)
 write_json(out/'audit-repeat.json',cscan,redactor=ctx.evidence.redactor)
 # Deliberately leak ONLY in a separate synthetic negative-control directory.
 planted=out.parent/(out.name+'-planted');planted.mkdir(mode=0o700)
 (planted/'leak.log').write_text(raw)
 negative=scan_run(planted)
 write_json(out/'planted-scan.json',negative,redactor=ctx.evidence.redactor)
 write_json(out/'planted-proof.json',{'rejected':not negative['passed'],'planted_url':ctx.url_evidence(raw),'planted_length':len(raw),'raw_artifact_excluded_from_export':True},redactor=ctx.evidence.redactor)
 print(json.dumps({'functional':[r.functional for r in rows],'scan_passed':result['secret_scan']['passed'],'planted_scan_failed':not negative['passed'],'out':str(out)}))
 if not all(r.functional=='passed' for r in rows) or not result['secret_scan']['passed'] or negative['passed']:raise SystemExit(1)
finally:
 target.execute('kill '+str(browser_pid)+' '+str(server_pid)+' 2>/dev/null || true')
 target.execute("printf 'synthetic-cleared' | xclip -selection clipboard >/dev/null 2>&1")
 target.close()
