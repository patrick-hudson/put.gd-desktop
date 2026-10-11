
import datetime,json,os,re,subprocess
out={'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'method':'read-only KWin org.kde.KWin /KWin org.kde.KWin.supportInformation; only rendering lines exported','exportedLines':[]}
cmd=['busctl','--address=unix:path=/run/user/'+str(os.getuid())+'/bus','--timeout=5','--json=short','call','org.kde.KWin','/KWin','org.kde.KWin','supportInformation']
try:
 r=subprocess.run(cmd,capture_output=True,text=True,timeout=8); out['exitCode']=r.returncode
 if r.returncode==0:
  try:
   v=json.loads(r.stdout); data=v.get('data',[]); data=data[0] if data and isinstance(data[0],str) else ''
  except json.JSONDecodeError: data=''
  for line in data.splitlines():
   if re.match(r'^(?:Compositing is|Compositing Type:|OpenGL vendor string:|OpenGL renderer string:|OpenGL version string:|OpenGL shading language version string:|Driver:|GPU class:|OpenGL version:|GLSL version:|Mesa version:|Direct rendering:|Software rendering:)',line.strip()): out['exportedLines'].append(line.strip())
 else: out['error']='KWin_readonly_call_failed'
except (OSError,subprocess.TimeoutExpired) as e: out['error']=type(e).__name__
print(json.dumps(out))
