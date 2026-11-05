"""Independent Python X11 input/paint observer; never compiles or saves foreign pixels."""
import ctypes as C, json, os, socket, struct, time, uuid, hashlib, math
from pathlib import Path
from campaign_ui import Drawable, XImage, _x_error
from bench_ui_process import identity, same_identity
from physical_campaign import require

class Map(C.Structure):
 _fields_=[('type',C.c_int),('serial',C.c_ulong),('send',C.c_int),('display',C.c_void_p),('event',C.c_ulong),('window',C.c_ulong),('override',C.c_int)]
class Event(C.Union):
 _fields_=[('type',C.c_int),('mapped',Map),('pad',C.c_long*24)]
class Native:
 def __init__(self,run):
  self.run=run;self.x=C.CDLL('libX11.so.6');self.xt=C.CDLL('libXtst.so.6')
  for name,args,ret in [('XOpenDisplay',[C.c_char_p],C.c_void_p),('XDefaultRootWindow',[C.c_void_p],C.c_ulong),('XInternAtom',[C.c_void_p,C.c_char_p,C.c_int],C.c_ulong),('XStringToKeysym',[C.c_char_p],C.c_ulong),('XKeysymToKeycode',[C.c_void_p,C.c_ulong],C.c_uint),('XPending',[C.c_void_p],C.c_int),('XNextEvent',[C.c_void_p,C.POINTER(Event)],C.c_int),('XSelectInput',[C.c_void_p,C.c_ulong,C.c_long],C.c_int),('XFlush',[C.c_void_p],C.c_int),('XSync',[C.c_void_p,C.c_int],C.c_int),('XGetPixel',[C.POINTER(XImage),C.c_int,C.c_int],C.c_ulong),('XGetImage',[C.c_void_p,C.c_ulong,C.c_int,C.c_int,C.c_uint,C.c_uint,C.c_ulong,C.c_int],C.POINTER(XImage)),('XDestroyImage',[C.POINTER(XImage)],C.c_int),('XFree',[C.c_void_p],C.c_int),('XCloseDisplay',[C.c_void_p],C.c_int)]:
   fn=getattr(self.x,name);fn.argtypes=args;fn.restype=ret
  self.x.XGetGeometry.argtypes=[C.c_void_p,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_int),C.POINTER(C.c_int),C.POINTER(C.c_uint),C.POINTER(C.c_uint),C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
  self.x.XGetWindowProperty.argtypes=[C.c_void_p,C.c_ulong,C.c_ulong,C.c_long,C.c_long,C.c_int,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_int),C.POINTER(C.c_ulong),C.POINTER(C.c_ulong),C.POINTER(C.POINTER(C.c_ubyte))]
  self.x.XSetErrorHandler.argtypes=[type(_x_error)];self.x.XSetErrorHandler(_x_error)
  self.xt.XTestFakeKeyEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong]
  self.xt.XTestFakeMotionEvent.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_ulong]
  self.xt.XTestFakeButtonEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong]
  self.d=self.x.XOpenDisplay(None);require(bool(self.d),'display_unavailable');self.root=self.x.XDefaultRootWindow(self.d)
  self.atom=self.x.XInternAtom(self.d,b'_NET_WM_PID',0);self.x.XSelectInput(self.d,self.root,1<<19);self.x.XSync(self.d,0)
  self.owner=run.resident.pid;self.stamp=identity(self.owner)
  self.coverdraw=Drawable(hex(self.root),self.stamp)
 def rpc(self,method,params=None):
  rid=uuid.uuid4().hex
  with socket.socket(socket.AF_UNIX) as s:
   s.settimeout(180);s.connect(self.run.socket)
   pid,uid,_=struct.unpack('3i',s.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
   require(pid==self.owner and uid==os.getuid() and same_identity(identity(pid),self.stamp),'session_owner_changed')
   s.sendall(json.dumps({'schemaVersion':1,'requestId':rid,'session':self.run.socket,'method':method,'params':params or {}}).encode()+b'\n')
   data=bytearray()
   while not data.endswith(b'\n'):
    chunk=s.recv(65536);require(bool(chunk) and len(data)<16*1024**2,'rpc_reply_unbounded');data.extend(chunk)
  r=json.loads(data);require(r.get('requestId')==rid and r.get('ok'),method+':'+(r.get('error') or {}).get('code','failed'));return r['result']
 def key(self,name,down):
  code=self.x.XKeysymToKeycode(self.d,self.x.XStringToKeysym(name.encode()));require(code>0,'key_missing');self.xt.XTestFakeKeyEvent(self.d,code,int(down),0)
 def chord(self):
  for k,down in [('Control_L',1),('Shift_L',1),('F12',1),('F12',0),('Shift_L',0),('Control_L',0)]:self.key(k,down)
  self.x.XFlush(self.d)
 def escape(self):
  self.key('Escape',1);self.key('Escape',0);self.x.XFlush(self.d)
 def geometry(self,w):
  root=C.c_ulong();x=C.c_int();y=C.c_int();width=C.c_uint();height=C.c_uint();border=C.c_uint();depth=C.c_uint()
  ok=self.x.XGetGeometry(self.d,w,C.byref(root),C.byref(x),C.byref(y),C.byref(width),C.byref(height),C.byref(border),C.byref(depth))
  return [x.value,y.value,width.value,height.value] if ok else None
 def prop_pid(self,w):
  actual=C.c_ulong();fmt=C.c_int();n=C.c_ulong();after=C.c_ulong();data=C.POINTER(C.c_ubyte)()
  rc=self.x.XGetWindowProperty(self.d,w,self.atom,0,1,0,6,C.byref(actual),C.byref(fmt),C.byref(n),C.byref(after),C.byref(data))
  try:return C.cast(data,C.POINTER(C.c_ulong))[0] if rc==0 and fmt.value==32 and n.value==1 else 0
  finally:
   if data:self.x.XFree(data)
 def color(self,w,x=20,y=20):
  p=self.x.XGetImage(self.d,w,x,y,1,1,C.c_ulong(-1),2)
  if not p:return None
  try:
   v=self.x.XGetPixel(p,0,0);return [(v>>16)&255,(v>>8)&255,v&255]
  finally:self.x.XDestroyImage(p)
 def capture(self,submit=False):
  bounds=self.geometry(self.root);require(bounds==[0,0,1920,1080],'unexpected_physical_output_geometry')
  image=self.coverdraw.pixels(bounds)
  require(image.getextrema()==((80,80),(90,90),(100,100)),'whole_synthetic_cover_not_visible_before_input')
  while self.x.XPending(self.d):e=Event();self.x.XNextEvent(self.d,C.byref(e))
  before={o['id'] for o in self.rpc('operation.recent')['operations']}
  started=time.perf_counter();self.chord();window=None;helper=None;paint=None;present=None;observed=[];input_issued=None;pressed=False
  try:
   while time.perf_counter()-started<5:
    while self.x.XPending(self.d):
     e=Event();self.x.XNextEvent(self.d,C.byref(e))
     if e.type!=19:continue
     if self.geometry(e.mapped.window)!=self.geometry(self.root):continue
     pid=self.prop_pid(e.mapped.window)
     if pid<=1:continue
     try:
      proc=Path('/proc')/str(pid);parts=(proc/'stat').read_text().rsplit(')',1)[1].split()
      if int(parts[1])!=self.owner or (proc/'exe').resolve()!=Path(self.run.args.capture_helper).resolve() or proc.stat().st_uid!=os.getuid():continue
      helper=identity(pid);window=e.mapped.window
     except FileNotFoundError:continue
    if window:
     if input_issued is None:
      # Tagged 4d3840c6 paints the live ring only after a real selection.
      # Match the frozen driver's map -> held drag -> paint -> cancel flow.
      time.sleep(.001)
      self.xt.XTestFakeMotionEvent(self.d,-1,100,150,0);self.xt.XTestFakeButtonEvent(self.d,1,1,0);pressed=True
      self.xt.XTestFakeMotionEvent(self.d,-1,700,430,0);self.x.XFlush(self.d)
      input_issued=(time.perf_counter()-started)*1000
     v=self.color(window,102,151)
     if v and v not in observed:observed.append(v)
     if v and all(abs(a-b)<=3 for a,b in zip(v,[255,255,255])) and paint is None:paint=(time.perf_counter()-started)*1000
     v=self.color(self.root,102,151)
     if v and all(abs(a-b)<=3 for a,b in zip(v,[255,255,255])):present=(time.perf_counter()-started)*1000
     if paint is not None and present is not None:break
    time.sleep(.001)
   require(paint is not None and present is not None,'owned_overlay_paint_not_observed:'+repr(observed))
   require(same_identity(identity(helper['pid']),helper),'selector_owner_changed_before_input')
   if not submit:self.escape()
   self.xt.XTestFakeButtonEvent(self.d,1,0,0);pressed=False;self.x.XFlush(self.d)
   deadline=time.monotonic()+10
   while True:
    fresh=[o for o in self.rpc('operation.recent')['operations'] if o['id'] not in before]
    if fresh:break
    require(time.monotonic()<deadline,'new_capture_missing');time.sleep(.002)
   require(len(fresh)==1,'ambiguous_capture');oid=fresh[0]['id']
   if not submit:
    while self.rpc('operation.inspect',{'id':oid})['state'] not in ('cancelled','completed','failed','partial'):
     require(time.monotonic()<deadline,'cancel_did_not_settle');time.sleep(.002)
   while self.prop_pid(window)==helper['pid']:
    require(time.monotonic()<deadline,'selector_surface_did_not_retire');time.sleep(.002)
   while self.color(self.root)!=[80,90,100]:
    require(time.monotonic()<deadline,'cover_did_not_return');time.sleep(.002)
   return {'operationId':oid,'drawablePaintMs':paint,'rootObservedPaintMs':present,'inputIssuedMs':input_issued,'started':started}
  except BaseException:
   if helper:
    try:
     if same_identity(identity(helper['pid']),helper):self.escape()
    except (FileNotFoundError,ProcessLookupError):pass
   if pressed:self.xt.XTestFakeButtonEvent(self.d,1,0,0);self.x.XFlush(self.d)
   raise
 def close(self):self.coverdraw.close();self.x.XCloseDisplay(self.d)

def verify_pixels(detail,run):
 from PIL import Image
 path=Path(detail['local']['path']);require(path.resolve().is_relative_to(run.root),'capture_file_outside_private_root')
 with Image.open(path) as image:
  require(image.size==(600,280) and image.convert('RGB').getextrema()==((80,80),(90,90),(100,100)),'captured_pixels_differ_from_independent_cover')
 run.event('native-capture-pixel-verification',historyId=detail['id'],sha256=hashlib.sha256(path.read_bytes()).hexdigest(),dimensions=[600,280],expectedRGB=[80,90,100],tolerance=0)

def summary(rows,key):
 xs=sorted(r[key] for r in rows);return {'samples':len(xs),'p50Ms':xs[(len(xs)-1)//2],'p95Ms':xs[math.ceil(len(xs)*.95)-1],'maxMs':max(xs)}

def prepare(run,native):
 run.upload_configuration()
 s=native.rpc('settings.get');native.rpc('settings.patch',{'expectedRevision':s['metadata']['observedRevision'],'patch':{'actions':{'region':{'shortcut':'Ctrl+Shift+F12'}},'preferences':{'regionSelectionMode':'live','notifications':False}}})
 deadline=time.monotonic()+10
 while not native.rpc('ui.snapshot')['diagnostics']['shortcuts']['bindings'].get('region',{}).get('bound'):
  require(time.monotonic()<deadline,'shortcut_not_bound');time.sleep(.05)
 time.sleep(.2)

def latency(run,with_links=False):
 n=Native(run)
 try:
  prepare(run,n);samples=[]
  for i in range(35):
   r=n.capture(submit=with_links)
   if with_links:
    deadline=time.monotonic()+60
    while True:
     op=n.rpc('operation.inspect',{'id':r['operationId']})
     if op['state'] in ('completed','failed','partial','cancelled'):break
     require(time.monotonic()<deadline,'capture_upload_timeout');time.sleep(.002)
    require(op['state']=='completed','capture_upload_failed')
    hid=op['items'][0]['artifact']['id'];detail=n.rpc('history.detail',{'id':hid})
    require(detail['linkAvailable'] and detail['uploadStatus']=='uploaded','capture_link_missing')
    verify_pixels(detail,run)
    r['captureToLinkMs']=(time.perf_counter()-r['started'])*1000;r['historyId']=hid
    from campaign_workloads import verify_uploaded
    verify_uploaded(run,op)
   r.pop('started');r['warmup']=i<5;samples.append(r);run.event('native-capture-sample',**r)
  warm=samples[5:];run.result['metrics']['nativeCapture']={'parameters':{'warmups':5,'samples':30,'mode':'live', 'paintOracle':'white ring at (102,151) after map-owned held selection [100,150,600,280]; tagged 4d3840c6 no-scrim overlay','probeSleepMs':1,'autoDragPauseMs':1,'region':[100,150,600,280]},'drawablePaint':summary(warm,'drawablePaintMs'),'rootObservedPaint':summary(warm,'rootObservedPaintMs'),'captureToLink':summary(warm,'captureToLinkMs') if with_links else None,'raw':samples}
  run.result['gates']['overlayPaint']={'status':'pass' if max(summary(warm,'rootObservedPaintMs')['p95Ms'],summary(warm,'drawablePaintMs')['p95Ms'])<=150 else 'fail','limitMs':150}
  run.visual('native-capture-cover-after')
 finally:n.close()

def burst(run):
 from campaign_workloads import tray,verify_uploaded,settle
 n=Native(run)
 try:
  prepare(run,n);baseline=run.sample('pre-native-burst')['totals'];initial=tray(run,'native-burst-before')
  run.fixture('/__rules',{'rules':[{'method':'PUT','delay':10,'remaining':100}]})
  ids=[];queued_peak=active_peak=0
  for i in range(50):
   r=n.capture(submit=True);ids.append(r['operationId']);ops=n.rpc('operation.recent')['operations'];queued=sum(o['state']=='queued' for o in ops);active=sum(o['state']=='uploading' for o in ops);queued_peak=max(queued_peak,queued);active_peak=max(active_peak,active)
   run.event('native-burst-accepted',index=i,**{k:v for k,v in r.items() if k!='started'},queued=queued,uploading=active);run.sample('native-burst-active')
   if i in (4,24,49):tray(run,'native-burst-'+str(i))
  links=set()
  for oid in ids:
   op=run.wait_operation(oid);require(op['state']=='completed','native_burst_failed');verify_pixels(n.rpc('history.detail',{'id':op['items'][0]['artifact']['id']}),run);verify_uploaded(run,op);links.add(op['items'][0]['receipt']['directUrl'])
  require(len(set(ids))==len(links)==50 and queued_peak>=1 and active_peak>=2,'native_burst_queue_or_links_missing')
  run.fixture('/__rules',{'rules':[]});time.sleep(2);final=tray(run,'native-burst-after');require(not final['workingTitleObserved'],'native_burst_tray_not_idle')
  run.result['metrics']['nativeBurst']={'nativeCaptures':50,'verifiedUploads':50,'uniqueLinks':len(links),'queuedPeak':queued_peak,'uploadingPeak':active_peak,'putDelaySeconds':10,'concurrentSlots':4,'trayStatus':final['Status']};run.visual('native-burst-cover-after');settle(run,baseline)
 finally:
  run.fixture('/__rules',{'rules':[]});n.close()

