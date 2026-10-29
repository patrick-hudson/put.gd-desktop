"""Production Context: real desktop input, resident assertions and saved evidence."""
import dataclasses
import json
from pathlib import Path
import re
import shlex
import time
from tools.lab import lab
from tools.lab.surface_map.surface_map import plain_label
from .api import Context, Frame, RecipeBlocked, RecipeFailed
from .catalog import Catalog, compare, control_point, image_modules
from .target import qcodes


class Fixture(dict):
    """Descriptors support both the contract's attributes and mapping recipes."""
    def __getattr__(self, name):
        try:
            return self[name]
        except KeyError as error:
            raise AttributeError(name) from error


class DriverContext(Context):
    def __init__(self, target, environment, profile, catalog, out):
        self.target, self.environment, self.profile = target, environment, profile
        for key in ('desktop','host','placement','package','destination'):
            setattr(self,key,environment[key])
        self.catalog, self.out = Catalog(catalog,profile), Path(out)
        self.profile = dict(profile, guest=environment.get('guest'))
        self._fixtures = {}
        self._captures = {}
        self._current_window = None
        self._ids = {}
        self._screen_dimensions = {}
        self.begin(None)

    def begin(self, recipe):
        self.recipe = recipe
        self.frames, self.notes, self.reviews = [], [], []
        self._noted_ids=set()
        self._notifications_since = time.time()
        self._deadline = time.monotonic() + (recipe.timeout if recipe else 120)

    def _check_time(self):
        if time.monotonic() > self._deadline:
            raise RecipeFailed('recipe deadline exceeded')

    def call(self, method, params=None):
        self._check_time()
        deadline=min(self._deadline,time.monotonic()+15)
        while True:
            response=self.cli(method,json.dumps(params or {}))
            try:
                envelope=json.loads(response['stdout'])
            except ValueError as error:
                raise RecipeBlocked('production CLI returned no JSON envelope') from error
            if envelope.get('ok'):
                result=envelope['result']
                for field in ('operationId','recordingId','shareOperationId'):
                    identity=result.get(field)
                    if isinstance(identity,str) and re.fullmatch('[A-Za-z0-9_-]{1,128}',identity) and identity not in self._noted_ids:
                        self.note(field+'='+identity)
                        self._noted_ids.add(identity)
                return result
            code=envelope.get('error',{}).get('code','unknown')
            # Startup acknowledgement precedes helper construction. Retry only
            # observational commands, never a mutation whose effect is uncertain.
            observational=method.endswith('.inspect') or method in ('ui.viewState','history.list')
            if observational and code in ('editor_starting','initializing') and time.monotonic()<deadline:
                time.sleep(.2)
                continue
            raise RecipeFailed('resident '+method+': '+code)

    def view(self, method, params=None):
        params = dict(params or {})
        role = {'ui.history':'history','ui.diagnostics':'diagnostics','ui.fileDialog':'fileDialog',
                'editor.view':'editor','recording.review':'review'}.get(method)
        if method == 'ui.history':
            self.call('ui.show',{'view':'history'})
            if 'select' in params:
                params = {'action':'select','ids':[params['select']]}
            params.setdefault('action','inspect')
        elif method in ('editor.view','recording.review','ui.diagnostics'):
            params.setdefault('action','inspect')
        elif method == 'ui.show':
            role = params.get('view','main')
        if role:
            self._current_window = role
        result = self.call(method, params)
        if params.get('id'):
            self._ids[role] = params['id']
        return result

    def wait(self, predicate, what, timeout=10.0, interval=.2):
        if timeout < 0 or interval <= 0:
            raise ValueError('wait needs a nonnegative timeout and positive interval')
        deadline = min(self._deadline, time.monotonic()+timeout)
        while True:
            self._check_time()
            value = predicate()
            if value:
                return value
            if time.monotonic() >= deadline:
                raise RecipeFailed('timeout: '+what)
            time.sleep(min(interval,max(0,deadline-time.monotonic())))

    def _probe(self, name, query):
        source = Path(__file__).with_name('probe.py').read_text()
        # stdin contains code only. Query is a separate, shell-quoted JSON argument.
        script = 'python3 - '+shlex.quote(json.dumps({'probe':name,'query':query}))+" <<'SD_PROBE'\n"+source+'\nSD_PROBE\n'
        result = self.shell(script)
        if result['returncode']:
            raise RecipeBlocked(name+' probe failed (missing desktop probe dependencies)')
        result = json.loads(result['stdout'])
        if not result['ok']:
            raise RecipeBlocked(name+': '+result['error'])
        return result['result']

    def geometry(self, window):
        if self.profile.get('session') == 'wayland' and not self.profile.get('at_spi',{}).get('available',False):
            raise RecipeBlocked('window geometry: '+self.profile['at_spi'].get('reason','AT-SPI unavailable'))
        result = self._probe('geometry',{'title':self.profile.get('window_titles',{}).get(window,window),
                                         'scale':self.profile.get('scale',1)})
        # Per-head transforms are explicit profile fragments, never guessed from
        # a full-desktop pixel size. X11 probes already report device pixels.
        screens = self.profile.get('screens',[])
        for screen in screens:
            if screen['x'] <= result['x'] < screen['x']+screen['width'] and screen['y'] <= result['y'] < screen['y']+screen['height']:
                result.update(screen_x=screen['x'],screen_y=screen['y'],head=screen.get('head',0))
                if result.get('coordinate_space') != 'framebuffer':
                    result['scale'] = screen.get('scale',1)
                break
        return result

    def use_catalog(self, window, reference):
        """Optional state hint for controls shared by several catalog captures."""
        self.catalog.reference(reference)
        self._captures[window] = reference

    def click(self, control, window, button='left', offset=None):
        name, inventory, row = self.catalog.resolve(control,window,self._captures.get(window))
        geometry = self.geometry(window)
        # Resizing a Qt layout moves anchors nonlinearly. A ratio alone can
        # hit a neighbouring control, so obtain an independently live rectangle
        # when the window's logical size differs from the capture.
        if (geometry['width'],geometry['height']) != (inventory['width'],inventory['height']):
            if not self.profile.get('at_spi',{}).get('available'):
                raise RecipeBlocked('resized catalog control needs live AT-SPI geometry')
            x,y,w,h=self._probe('control',{'name':row.get('name'),'label':plain_label(row.get('text')),
                                             'title':self.profile.get('window_titles',{}).get(window,window),
                                             'pid':geometry.get('pid'),'bounds':[geometry[k] for k in ('x','y','width','height')]})
            dx,dy=offset if offset is not None else (row['rect'][2]/2,row['rect'][3]/2)
            scale=geometry.get('scale',1)
            x=round((x-geometry.get('screen_x',0)+dx*w/row['rect'][2])*scale)
            y=round((y-geometry.get('screen_y',0)+dy*h/row['rect'][3])*scale)
        else:
            x,y = control_point(inventory,row,geometry,offset)
        self._pointer_click(x,y,button,geometry.get('head',0))

    def _dimensions(self, head=0):
        screens = self.profile.get('screens',[])
        for screen in screens:
            if screen.get('head',0) == head:
                return round(screen['width']*screen.get('scale',1)),round(screen['height']*screen.get('scale',1))
        if head in self._screen_dimensions:
            return self._screen_dimensions[head]
        # A QMP screendump independently measures the actual framebuffer.
        path = self.out / '_screen.png'
        self.target.screenshot(path,head)
        Image,_,_ = image_modules()
        with Image.open(path) as im:
            self._screen_dimensions[head] = im.size
            return im.size

    def _pointer_click(self,x,y,button,head=0):
        self._check_time()
        w,h = self._dimensions(head)
        if not 0 <= x < w or not 0 <= y < h:
            raise RecipeBlocked('input point outside selected head')
        self.target.pointer(x,y,w,h,True,button,head)
        try:
            time.sleep(.08)
        finally:
            self.target.pointer(x,y,w,h,False,button,head)

    def click_at(self,x,y,button='left'):
        self._pointer_click(x,y,button)

    def drag(self,start,end,button='left',steps=12):
        if steps < 1:
            raise ValueError('drag steps must be positive')
        w,h = self._dimensions()
        if any(not 0 <= x < w or not 0 <= y < h for x,y in (start,end)):
            raise RecipeBlocked('drag point outside selected head')
        self.target.pointer(*start,w,h,True,button)
        try:
            for step in range(1,steps+1):
                self._check_time()
                x,y = (round(a+(b-a)*step/steps) for a,b in zip(start,end))
                self.target.pointer(x,y,w,h)
                time.sleep(.025)
        finally:
            self.target.pointer(*end,w,h,False,button)

    def keys(self,*sequence):
        for chord in sequence:
            self._check_time()
            self.target.key(qcodes(chord))

    def type_text(self,text):
        # Validate the whole string before sending any input.
        chords = [(['shift'] if c.isascii() and c.isupper() else []) + qcodes({' ':'Space','\n':'Return','\t':'Tab'}.get(c,c)) for c in text]
        for chord in chords:
            self._check_time(); self.target.key(chord)

    def scroll(self,x,y,clicks):
        w,h = self._dimensions()
        if self.target.physical:
            self.target.scroll_vnc(x,y,clicks)
        else:
            self.target.pointer(x,y,w,h)
            for _ in range(abs(clicks)):
                self._check_time()
                for down in (True,False):
                    self.target.events([{'type':'btn','data':{'button':'wheel-up' if clicks>0 else 'wheel-down','down':down}}])

    def cli(self,*args,timeout=60):
        self._check_time()
        return self.target.cli(*args,timeout=min(timeout,max(.1,self._deadline-time.monotonic())))

    def shell(self,script,timeout=60):
        self._check_time()
        return self.target.execute(script,timeout=min(timeout,max(.1,self._deadline-time.monotonic())))

    def mcp(self,tool,arguments=None):
        # Official client and installed helper, as in mcp_editor_e2e.py. The
        # executable is configurable for a locked tools/mcp/.venv installation.
        python = self.profile.get('mcp_python','python3')
        helper = self.profile.get('mcp_helper','putgd-mcp')
        code = '''import asyncio,json
from mcp import ClientSession,StdioServerParameters
from mcp.client.stdio import stdio_client
async def main():
    p=StdioServerParameters(command=HELPER,args=['--cli','putgd-cli','--socket',SOCKET,'--timeout','60'])
    async with stdio_client(p) as (read,write):
        async with ClientSession(read,write) as session:
            await session.initialize()
            reply=await session.call_tool(TOOL,ARGS)
            print(json.dumps(reply.structuredContent if reply.structuredContent is not None else {'content':[v.model_dump(mode='json') for v in reply.content],'isError':reply.isError}))
asyncio.run(main())
'''
        code = 'HELPER='+repr(helper)+'\nSOCKET='+repr(self.target.socket)+'\nTOOL='+repr(tool)+'\nARGS='+repr(arguments or {})+'\n'+code
        result = self.shell(shlex.quote(python)+" - <<'SD_MCP'\n"+code+'SD_MCP\n')
        if result['returncode']:
            raise RecipeBlocked('official MCP client/helper unavailable or failed; configure mcp_python and mcp_helper in profile')
        return json.loads(result['stdout'])

    def accessible(self,query):
        if not self.profile.get('at_spi',{}).get('available',False):
            raise RecipeBlocked('AT-SPI: '+self.profile.get('at_spi',{}).get('reason','unavailable in this desktop profile'))
        return self._probe('accessible',query)

    def start_notifications(self):
        script = 'nohup dbus-monitor --session '+shlex.quote("type='method_call',interface='org.freedesktop.Notifications',member='Notify'")+' '+shlex.quote("type='method_return',sender='org.freedesktop.Notifications'")+' > '+shlex.quote(self.target.root+'/notifications.log')+' 2>&1 < /dev/null & echo $!'
        result = self.shell(script)
        if result['returncode'] == 0:
            self.target.monitor_pid = int(result['stdout'].strip())

    def notifications(self):
        if not self.target.monitor_pid:
            raise RecipeBlocked('session-bus notification monitor not started')
        result = self.shell('kill -0 '+str(self.target.monitor_pid)+' && cat '+shlex.quote(self.target.root+'/notifications.log'))
        if result['returncode']:
            raise RecipeBlocked('session bus refused notification monitoring or dbus-monitor is unavailable')
        return parse_notifications(result['stdout'],self._notifications_since)

    def clipboard_text(self):
        command = 'wl-paste --no-newline --type text' if self.profile['session']=='wayland' else 'xclip -selection clipboard -out'
        result = self.shell(command)
        if result['returncode']:
            raise RecipeBlocked('clipboard text unavailable (requires wl-clipboard or xclip and a text owner)')
        return result['stdout']

    def fixture(self,name):
        if name in self._fixtures:
            return self._fixtures[name]
        if name == 'image':
            import base64, io
            Image,_,_=image_modules()
            image=Image.new('RGB',(640,320))
            for i,color in enumerate(lab.FIXTURE_RGB):
                x,y=(i%2)*320,(i//2)*160
                image.paste(color,(x,y,x+320,y+160))
            stream=io.BytesIO();image.save(stream,format='PNG')
            path=self.target.root+'/files/synthetic.png'
            result=self.shell("python3 - <<'SD_FIXTURE'\nimport base64\nfrom pathlib import Path\nPath("+repr(path)+").write_bytes(base64.b64decode("+repr(base64.b64encode(stream.getvalue()).decode())+"))\nSD_FIXTURE\n")
            if result['returncode']:
                raise RecipeBlocked('cannot write synthetic image on target')
            value=Fixture(path=path,width=640,height=320)
            self._fixtures[name]=value
            return value
        if name in ('upload','history','history-row'):
            if self.destination != 'local':
                raise RecipeBlocked('remote upload fixture requires the live lane isolated destination/credential bridge; core never provisions credentials')
            settings = self.call('settings.get')
            self.call('settings.patch',{'expectedRevision':settings['metadata']['observedRevision'],
                                       'patch':{'archive':{'root':self.target.root+'/files','keepUploads':True},'actions':{'desktop':{'destination':'local','profileId':None}}}})
            path=self.fixture('image')['path']
            settings=self.call('settings.get')
            self.call('settings.patch',{'expectedRevision':settings['metadata']['observedRevision'],
                                       'patch':{'actions':{'edit':{'destination':'local','profileId':None}}}})
            operation=self.call('editor.open',{'source':'file','path':path})['operationId']
            def editor_ready():
                reply=self.cli('editor.inspect',json.dumps({'id':operation}))
                envelope=json.loads(reply['stdout'])
                return envelope.get('ok')
            self.wait(editor_ready,'synthetic fixture editor ready',30)
            self.call('editor.submit',{'id':operation})
            state = self.wait(lambda:self._completed_operation(operation),'local capture fixture',30)
            rows = self.call('history.list',{'limit':50}).get('records',[])
            row = next((r for r in rows if r.get('operationId')==operation),None)
            if not row:
                raise RecipeBlocked('resident completed fixture but did not expose its history row')
            value = Fixture(id=row['id'],history_id=row['id'],operation_id=operation,
                                    name=row.get('name'),link=row.get('receipt',{}).get('directUrl') or None,path=row.get('local',{}).get('path'),state=state,row=row)
            self._fixtures[name] = value
            return value
        if name == 'recording':
            target=self.profile.get('recording_fixture_target')
            if not target:
                raise RecipeBlocked('recording fixture needs a profile recording_fixture_target covering a visible synthetic pattern')
            prepared = self.call('recording.prepare',{'target':target,'format':'mp4','audio':{'system':False}})
            rid = prepared.get('id') or prepared.get('recordingId')
            if not rid:
                raise RecipeBlocked('recording.prepare returned no recording identity')
            self._ids['review'] = rid
            self.wait(lambda: self.call('recording.inspect',{'id':rid}).get('state')=='ready','recording fixture prepared',30)
            self.call('recording.start',{'id':rid}); time.sleep(1)
            self.call('recording.stop',{'id':rid})
            self.wait(lambda:self.call('recording.inspect',{'id':rid}).get('state')=='reviewing','recording fixture review',45)
            value = Fixture(id=rid,recording_id=rid)
            self._fixtures[name] = value
            return value
        raise RecipeBlocked('unknown fixture: '+name)

    def _completed_operation(self,identity):
        state = self.call('operation.inspect',{'id':identity})
        if state.get('state') in ('failed','cancelled','partial'):
            raise RecipeFailed('fixture operation ended '+state['state'])
        return state if state.get('state')=='completed' else None

    def frame(self,label,window=None,reference=None):
        self._check_time()
        safe = re.sub('[^a-zA-Z0-9_.-]','_',label)[:80] or 'frame'
        recipe = self.recipe.name if self.recipe else 'setup'
        directory = self.out/'frames'/recipe
        directory.mkdir(parents=True,exist_ok=True)
        path = directory/(str(len(self.frames)).zfill(2)+'-'+safe+'.png')
        role = window or (self._current_window if reference else None)
        geometry = self.geometry(role) if role else None
        self.target.screenshot(path,geometry.get('head',0) if geometry else 0)
        if geometry:
            Image,_,_ = image_modules()
            x,y,w,h = (geometry[k] for k in ('x','y','width','height'))
            scale = geometry.get('scale',1)
            x=(x-geometry.get('screen_x',0))*scale;y=(y-geometry.get('screen_y',0))*scale
            box = tuple(round(v) for v in (x,y,x+w*scale,y+h*scale))
            with Image.open(path) as im:
                if box[0]<0 or box[1]<0 or box[2]>im.width or box[3]>im.height:
                    raise RecipeBlocked('window crop extends outside the captured head')
                im.crop(box).save(path)
        frame = Frame(label,str(path.relative_to(self.out)),role,reference)
        self.frames.append(frame)
        if reference and not (self.recipe and self.recipe.desktop_drawn):
            try:
                baseline = self.catalog.reference(reference)
                frame.tolerance = self.profile['catalog_tolerance']
                frame.distance = compare(path,baseline,frame.tolerance)
                if frame.distance > frame.tolerance:
                    self.review(f'catalog mismatch (distance={frame.distance:.6f}, tolerance={frame.tolerance}): actual={path.resolve()} reference={baseline.resolve()}')
            except RecipeBlocked as error:
                self.review(str(error)+'; actual='+str(path.resolve())+' reference='+str((self.catalog.directory/(reference+'.png')).resolve()))
        return frame

    def note(self,text):
        self.notes.append(str(text))

    def review(self,reason):
        self.reviews.append(str(reason))


def parse_notifications(text,since=0):
    blocks = re.split(r'(?m)(?=^(?:method call|method return) )',text)
    rows, pending = [], {}
    for block in blocks:
        timestamp = re.search(r'time=([0-9.]+)',block)
        if not timestamp or float(timestamp[1]) < since:
            continue
        if 'member=Notify' in block:
            strings = re.findall(r'^\s*string "(.*)"\s*$',block,re.M)
            integers = re.findall(r'^\s*uint32 (\d+)',block,re.M)
            serial = re.search(r'\bserial=(\d+)',block)
            sender = re.search(r'\bsender=(\S+)',block)
            if len(strings)>=4 and strings[0]=='put.gd' and serial and sender:
                actions=re.search(r'array \[\s*(.*?)\s*\]',block,re.S)
                action_strings=re.findall(r'string "(.*?)"',actions[1]) if actions else []
                row={'summary':strings[2],'body':strings[3],'actions':action_strings,'id':None,
                     'replaces_id':int(integers[0]) if integers else 0,'at':float(timestamp[1])}
                rows.append(row);pending[(sender[1],serial[1])]=row
        elif 'method return' in block:
            serial = re.search(r'\breply_serial=(\d+)',block)
            identity = re.search(r'uint32 (\d+)',block)
            destination=re.search(r'\bdestination=(\S+)',block)
            if serial and identity and destination and (destination[1],serial[1]) in pending:
                pending[(destination[1],serial[1])]['id']=int(identity[1])
    return rows
