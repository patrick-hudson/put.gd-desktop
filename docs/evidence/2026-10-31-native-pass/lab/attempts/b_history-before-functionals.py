"""Content-specific native/root pixel endpoints beside the frozen observers.

Calibrations must match the expected production row names, and the existing
physical-wall visibility guard, before they can be timing oracles. Exact pixel
matching is used; acknowledgement or a generic repaint never ends a sample.
"""
import hashlib,math,subprocess,time,json
from pathlib import Path
from campaign_ui import Accessible as OriginalAccessible,Drawable
from campaign_workloads import UI as OriginalUI,require,assess,percentile
from observe_idle import owned_windows
from b_native import Native,summary

class Accessible(OriginalAccessible):
 def find(self,name):
  # This source's renamed accessible list. Ownership checks stay unchanged.
  return super().find('History items and upload activity' if name=='Captures and upload activity' else name)
 def search_controls(self):
  return self.find('Search history by file name'),None
 def search(self,text,controls=None):
  field,_=controls or self.search_controls()
  self.check()
  require(self.api.Component.grab_focus(field),'search_field_focus_refused')
  require(self.api.EditableText.set_text_contents(field,text),'search_text_refused')
  # The current product has an auto-filter field and explicit Return, no
  # Search button. Do not pretend the retired button is still available.
  self.key('Return')

class UI(OriginalUI):
 def __init__(self,run):
  import campaign_ui
  original=campaign_ui.Accessible
  campaign_ui.Accessible=Accessible
  try:super().__init__(run)
  finally:campaign_ui.Accessible=original
  self.native=Native(run);self.rootdraw=Drawable(hex(self.native.root),self.owner)
 def close(self):
  self.rootdraw.close();self.native.close();self.draw.close()
  subprocess.run(['wmctrl','-ic',self.window],check=True,capture_output=True,timeout=5)
  deadline=time.monotonic()+10
  while owned_windows(self.owner['pid']) if Path('/proc/'+str(self.owner['pid'])).exists() else False:
   require(time.monotonic()<deadline,'owned_history_did_not_close');time.sleep(.01)
 def focused(self):
  active=int(subprocess.check_output(['xdotool','getactivewindow'],text=True).strip())
  require(active==int(self.window,16),'history_not_foreground')
 def key(self,name):
  self.focused();self.native.key(name,True);self.native.key(name,False);self.native.x.XFlush(self.native.d)
 def pixels(self):
  a=self.draw.pixels(self.list_rectangle).tobytes();r=self.list_rectangle
  b=self.rootdraw.pixels([self.x+r[0],self.y+r[1],r[2],r[3]]).tobytes()
  return hashlib.sha256(a).hexdigest(),hashlib.sha256(b).hexdigest()
 def stable(self,label,expected):
  self.wait_rows(expected);last=None;deadline=time.monotonic()+5
  while True:
   value=self.pixels()
   if value[0]==value[1] and value==last:break
   require(time.monotonic()<deadline,'content_calibration_not_visible_or_stable');last=value;time.sleep(.05)
  self.save('calibration-'+label)
  self.run.event('content-calibration',label=label,sha256=value[0],historyIds=[r['id'] for r in expected],endpoint='expected AT-SPI row names + native/root equality + guarded physical wall crop')
  return value[0]
 def endpoint(self,oracle,action,label):
  self.focused();started=time.perf_counter();action();deadline=time.monotonic()+5;probes=0
  while True:
   native,root=self.pixels();probes+=1
   if native==root==oracle:break
   require(time.monotonic()<deadline,'content_endpoint_not_observed:'+label);time.sleep(.001)
  ms=(time.perf_counter()-started)*1000
  self.run.event('content-response',label=label,milliseconds=ms,probes=probes,oracleSha256=oracle,endpoint='exact calibrated row-content pixels in PID-owned drawable and visible root rectangle')
  return ms

def run(run):
 run.result['contentObserver']={'source':str(Path(__file__)),'sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'scope':'content-specific exact native/root viewport matching; no generic-paint substitution; independent calibration before timing','listAccessibleName':'History items and upload activity'}
 ui=UI(run)
 try:
  records=run.call('history.list',{'limit':50})['records'];ui.wait_rows(records)
  field,button=ui.accessible.search_controls()
  ui.accessible.search('',(field,button));ui.wait_rows(records)
  # Use the stable list viewport, with the pointer parked outside it.
  subprocess.run(['xdotool','mousemove','1900','1000'],check=True,capture_output=True)
  canonical=ui.stable('open-10000',records)
  geometry=[ui.x,ui.y,ui.width,ui.height];rectangle=list(ui.list_rectangle)
 finally:ui.close()
 opens=[]
 for index in range(20):
  started=time.perf_counter();ui=UI(run)
  try:
   require([ui.x,ui.y,ui.width,ui.height]==geometry and ui.list_rectangle==rectangle,'history_geometry_changed_since_calibration')
   deadline=time.monotonic()+5
   while True:
    a,b=ui.pixels()
    if a==b==canonical:break
    require(time.monotonic()<deadline,'history_open_content_not_observed');time.sleep(.001)
   elapsed=(time.perf_counter()-started)*1000;opens.append({'milliseconds':elapsed});ui.wait_rows(records);run.event('content-history-open',index=index,milliseconds=elapsed,oracleSha256=canonical)
   if index==19:ui.save('history-content-open')
  finally:ui.close()
 ui=UI(run)
 try:
  ui.wait_rows(records)
  queries=['landscape-00','motion-01','landscape-02','motion-03','landscape-04','motion-05'];cal={}
  controls=ui.accessible.search_controls()
  for term in queries:
   ui.accessible.search(term,controls);expected=run.call('history.list',{'search':term,'limit':50})['records'];require(bool(expected),'search_fixture_missing')
   cal[term]=(ui.stable('search-'+term,expected),expected)
  searches=[]
  for index in range(30):
   term=queries[index%6];oracle,expected=cal[term]
   ms=ui.endpoint(oracle,lambda:ui.accessible.search(term,controls),'search-'+term);ui.wait_rows(expected);searches.append({'milliseconds':ms,'query':term})
  ui.accessible.search('',controls);ui.wait_rows(records)
  node=ui.accessible.prepare_selection();ui.key('Home');time.sleep(.1)
  scroll_oracles=[]
  for index in range(10):
   ui.key('Next');time.sleep(.06)
   value=ui.stable('scroll-'+str(index),records);scroll_oracles.append(value)
   if ui.accessible.last_row_visible():break
  require(len(scroll_oracles)>=2 and ui.accessible.last_row_visible(),'scroll_calibration_did_not_cover_page')
  scrolls=[]
  for cycle in range(5):
   ui.key('Home');time.sleep(.08)
   for index,oracle in enumerate(scroll_oracles):
    ms=ui.endpoint(oracle,lambda:ui.key('Next'),'page-down-'+str(index));scrolls.append({'milliseconds':ms,'cycle':cycle,'step':index})
  # Traverse every page and actually reveal its final row. This is separate
  # from the first-page calibrated response sample; no FPS claim is made.
  ui.key('Home');seen=0;pages=[];started=time.monotonic()
  for offset in range(0,10000,50):
   page=run.call('history.list',{'offset':offset,'limit':50})['records'];ui.wait_rows(page)
   ui.key('Home')
   for step in range(30):
    if ui.accessible.last_row_visible():break
    ui.key('Next')
   require(ui.accessible.last_row_visible(),'page_tail_not_visible')
   seen+=len(page);pages.append({'offset':offset,'count':len(page),'ids':[r['id'] for r in page]})
   if offset in (0,4950,9950):ui.save('history-traverse-'+str(offset))
   if offset+50<10000:
    # Same native accessible button used by the frozen observer; verify rows.
    ui.accessible.click('Next')
  require(seen==10000,'full_history_traversal_incomplete')
  run.result['metrics']['historyContent']={'open':summary(opens,'milliseconds'),'search':summary(searches,'milliseconds'),'scroll':summary(scrolls,'milliseconds'),'openRaw':opens,'searchRaw':searches,'scrollRaw':scrolls,'scrollScope':'PageDown responses across a full 50-row page, five repetitions; root/native exact content endpoint, not frames per second','traversedRows':seen,'pages':200,'traversalSeconds':time.monotonic()-started,'traversal':pages}
  for key,value in [('historyOpenUpperBoundMs',max(r['milliseconds'] for r in opens)),('historySearchP95UpperBoundMs',summary(searches,'milliseconds')['p95Ms']),('historyScrollP95UpperBoundMs',summary(scrolls,'milliseconds')['p95Ms'])]:assess(run,key,value)
  ui.save('history-content-complete')
 finally:ui.close()
