"""Production Context: real desktop input, resident assertions and saved evidence."""
import dataclasses
from contextlib import contextmanager
import json
from pathlib import Path
import re
import shlex
import time
from tools.lab import lab
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
        self._input_head = 0
        self._held_codes = []
        self._input_poisoned = False
        self._popup = None
        self._click_count = 1
        self.begin(None)

    def begin(self, recipe):
        self.recipe = recipe
        self._current_window, self._popup, self._input_head = None, None, 0
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
        if not isinstance(result,dict) or 'ok' not in result:
            raise RecipeBlocked(name+' probe returned an invalid response')
        if not result['ok']:
            raise RecipeBlocked(name+': '+result['error'])
        return result['result']

    def _window_query(self, window):
        spec = self.profile.get('windows', {}).get(window, {})
        return {'title': spec.get('title', self.profile.get('window_titles', {}).get(window, window)),
                'scale': self.profile.get('scale', 1), **spec}

    def _map_geometry(self, result):
        result = dict(result)
        for screen in self.profile.get('screens', []):
            if screen['x'] <= result['x'] < screen['x']+screen['width'] and screen['y'] <= result['y'] < screen['y']+screen['height']:
                result.update(screen_x=screen['x'], screen_y=screen['y'], head=screen.get('head', 0))
                if result.get('coordinate_space') != 'framebuffer':
                    result['scale'] = screen.get('scale', 1)
                break
        result.setdefault('head', 0)
        self._input_head = result['head']
        return result

    def geometry(self, window):
        """Live geometry; absent/ambiguous roles raise RecipeBlocked, never {}.

        editor.canvas is a live accessible child, not a scaled catalog estimate.
        windows() is the read-only presence/lifecycle query (an empty list proves
        no matching visible window at the observation time).
        """
        if window == 'editor.canvas':
            parent = self.geometry('editor')
            row = self._live_control('text:Image being edited', 'editor', parent, [])
            return self._map_geometry(dict(zip(('x','y','width','height'), row['rect'])) |
                                      {k:parent[k] for k in ('scale','pid','coordinate_space') if k in parent})
        if self.profile.get('session')=='wayland' and not self.profile.get('at_spi',{}).get('available'):
            raise RecipeBlocked('window geometry: '+self.profile.get('at_spi',{}).get('reason','AT-SPI unavailable'))
        result = self._probe('geometry', self._window_query(window))
        if window in ('region', 'recsel'):
            self._current_window = window
        return self._map_geometry(result)

    def windows(self, query=None):
        """Read-only shell window inventory, scoped to the private family by default."""
        return self._probe('windows', {'owned': True, **(query or {})})

    def focus(self, window, raise_window=True):
        """Select an input target and verify/optionally raise it before input."""
        owner = self.profile.get('windows',{}).get(window,{}).get('owner',window)
        self._current_window, self._popup = owner, None
        return self._guard(window=owner, raise_window=raise_window)

    def _guard(self, window=None, point=None, geometry=None, popup=None, raise_window=False):
        self._check_time()
        if self._input_poisoned:
            raise RecipeBlocked('held-key release was not confirmed; further input is withheld for this run')
        role = window or self._current_window
        if not role:
            raise RecipeBlocked('real input has no target window; use view(), focus(), or click(window=...)')
        geometry = geometry or self.geometry(role)
        query = {'geometry': geometry, 'point': point, 'popup': popup or self._popup,
                 'raise': raise_window or self.profile.get('focus',{}).get('raise_before_input',False),
                 'compositor_script': self.profile.get('focus', {}).get('compositor_script')}
        witness = self._probe('foreground', query)
        if not witness.get('ok'):
            raise RecipeFailed('input target '+repr(role)+' is obscured by '+repr(witness.get('obscuring_window', 'unknown window'))+
                               ' (window '+str(witness.get('obscuring_window_id') or witness.get('active'))+')')
        return witness

    def use_catalog(self, window, reference):
        self.catalog.reference(reference)
        self._captures[window] = reference

    def _live_control(self, selector, window, geometry, candidates):
        if not self.profile.get('at_spi', {}).get('available'):
            raise RecipeBlocked('live AT-SPI control geometry: '+self.profile.get('at_spi', {}).get('reason', 'unavailable'))
        return self._probe('control', {'selector': selector, 'candidates': candidates,
                                      'title': self._window_query(window)['title'],
                                      'pid': geometry.get('pid'),
                                      'popup': self.profile.get('windows', {}).get(window, {}).get('popup', False)})

    def click(self, control, window, button='left', offset=None):
        # Resolve semantics from every state, then resolve geometry from the
        # current tree. Dynamic menus need no catalog dump or stale rectangle.
        candidates = self.catalog.candidates(control, window, self._captures.get(window))
        popup_role = self.profile.get('windows', {}).get(window, {}).get('popup', False)
        owner = self.profile.get('windows', {}).get(window, {}).get('owner', window)
        geometry = self.geometry(owner)
        self._current_window = owner
        if self.profile.get('at_spi', {}).get('available'):
            row = self._live_control(control, window, geometry, candidates)
            x,y,w,h = row['rect']
            if offset is None:
                dx,dy = w/2,h/2
            else:
                sizes = {tuple(c['rect'][2:]) for c in candidates}
                if len(sizes) != 1:
                    raise RecipeBlocked('offset requires an unambiguous catalog control size')
                cw,ch = next(iter(sizes))
                dx,dy = offset[0]*w/cw,offset[1]*h/ch
            point = (x+dx,y+dy)
            mapped = self._map_geometry(dict(geometry, x=point[0], y=point[1]))
            scale = mapped.get('scale', 1)
            px,py = (round((point[i]-mapped.get(('screen_x','screen_y')[i],0))*scale) for i in (0,1))
            self._popup = row.get('popup')
            try:
                self._guard(window=owner, point=point, geometry=geometry, popup=self._popup)
                self._pointer_click(px,py,button,mapped['head'], guarded=True)
            finally:
                self._popup = None
        else:
            if popup_role:
                raise RecipeBlocked('dynamic menu control needs live AT-SPI geometry')
            _, inventory, row = self.catalog.resolve(control,window,self._captures.get(window))
            if (geometry['width'],geometry['height']) != (inventory['width'],inventory['height']):
                raise RecipeBlocked('resized catalog control needs live AT-SPI geometry')
            x,y = control_point(inventory,row,geometry,offset)
            self._guard(window=owner, point=self._global_point(x,y,geometry['head']), geometry=geometry)
            self._pointer_click(x,y,button,geometry['head'],guarded=True)

    def _global_point(self, x, y, head):
        for screen in self.profile.get('screens', []):
            if screen.get('head', 0) == head:
                scale = screen.get('scale', 1) if self.profile.get('session') != 'x11' else 1
                return (x/scale+screen['x'], y/scale+screen['y'])
        return (x,y)

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

    def _pointer_click(self,x,y,button,head=0,guarded=False):
        self._check_time()
        w,h = self._dimensions(head)
        if not 0 <= x < w or not 0 <= y < h:
            raise RecipeBlocked('input point outside selected head')
        if not guarded:
            self._guard(point=self._global_point(x,y,head))
        for index in range(self._click_count):
            self.target.pointer(x,y,w,h,True,button,head)
            try:
                time.sleep(.04)
            finally:
                self.target.pointer(x,y,w,h,False,button,head)
            if index+1 < self._click_count:
                time.sleep(.06)

    def click_at(self,x,y,button='left',head=None):
        self._pointer_click(x,y,button,self._input_head if head is None else head)

    def double_click(self, control, window, button='left'):
        # One physical gesture: resolving/probing again between clicks can take
        # longer than the desktop's double-click interval over guest SSH.
        self._click_count = 2
        try:
            self.click(control,window,button)
        finally:
            self._click_count = 1

    def hover(self,x,y,head=None):
        head = self._input_head if head is None else head
        w,h = self._dimensions(head)
        if not 0 <= x < w or not 0 <= y < h:
            raise RecipeBlocked('hover point outside selected head')
        self._guard(point=self._global_point(x,y,head))
        self.target.pointer(x,y,w,h,head=head)

    def drag(self,start,end,button='left',steps=12,head=None):
        if steps < 1:
            raise ValueError('drag steps must be positive')
        head = self._input_head if head is None else head
        w,h = self._dimensions(head)
        if any(not 0 <= x < w or not 0 <= y < h for x,y in (start,end)):
            raise RecipeBlocked('drag point outside selected head')
        self._guard(point=self._global_point(*start,head))
        self.target.pointer(*start,w,h,True,button,head)
        try:
            # A held pointer may leave its window during a header/resize drag;
            # recheck keyboard foreground without demanding hit-test at the end.
            for step in range(1,steps+1):
                self._check_time()
                self._guard()
                x,y = (round(a+(b-a)*step/steps) for a,b in zip(start,end))
                self.target.pointer(x,y,w,h,head=head)
                time.sleep(.025)
        finally:
            self.target.pointer(*end,w,h,False,button,head)

    def keys(self,*sequence):
        codes = [qcodes(chord) for chord in sequence]
        if any(set(chord) & set(self._held_codes) for chord in codes):
            raise RecipeBlocked('key chord would release a key held by held_keys()')
        for chord in codes:
            self._guard()
            self.target.key(chord)
        self._popup = None

    @contextmanager
    def held_keys(self,*keys):
        codes = [code for key in keys for code in qcodes(key)]
        if len(set(codes)) != len(codes) or set(codes) & set(self._held_codes):
            raise ValueError('held keys must be distinct and not already held')
        if self.target.physical:
            raise RecipeBlocked('physical held keys require input-agent scoped cleanup across lock loss; guest QMP only')
        self._guard()
        self._held_codes.extend(codes)
        def release():
            try:
                self.target.key_state(list(reversed(codes)),False)
            except BaseException:
                self._input_poisoned = True
                raise
        try:
            # Dispatch can succeed before a reply/deadline failure. Acquisition
            # belongs inside the cleanup scope too.
            self.target.key_state(codes,True)
            yield
        except BaseException:
            try:
                release()
            except Exception:
                self.note('held-key release unavailable after interrupted input')
            raise
        else:
            release()
        finally:
            self._held_codes = [c for c in self._held_codes if c not in codes]

    def type_text(self,text):
        chords = [(['shift'] if c.isascii() and c.isupper() else []) + qcodes({' ':'Space','\n':'Return','\t':'Tab'}.get(c,c)) for c in text]
        if any(set(chord) & set(self._held_codes) for chord in chords):
            raise RecipeBlocked('typed text would release a key held by held_keys()')
        for chord in chords:
            self._guard()
            self.target.key(chord)

    def scroll(self,x,y,clicks,head=None):
        head = self._input_head if head is None else head
        w,h = self._dimensions(head)
        if not 0 <= x < w or not 0 <= y < h:
            raise RecipeBlocked('scroll point outside selected head')
        self._guard(point=self._global_point(x,y,head))
        if self.target.physical:
            if head:
                raise RecipeBlocked('physical VNC scroll supports only the configured head')
            self.target.scroll_vnc(x,y,clicks)
        else:
            self.target.pointer(x,y,w,h,head=head)
            for _ in range(abs(clicks)):
                self._guard(point=self._global_point(x,y,head))
                for down in (True,False):
                    self.target.events([{'type':'btn','data':{'button':'wheel-up' if clicks>0 else 'wheel-down','down':down}}],head)

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
        import inspect
        from tools.lab.mcp_client import helper_session
        code='import asyncio,json\nfrom contextlib import asynccontextmanager\n'+inspect.getsource(helper_session)+'''
async def main():
    async with helper_session(HELPER,['--cli','putgd-cli','--socket',SOCKET,'--timeout','60']) as session:
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
        # Resolve the unique server owner so method-return filtering works on
        # dbus-monitor implementations which do not resolve well-known senders.
        result = self.shell("gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.GetNameOwner org.freedesktop.Notifications")
        owner = re.search(r"':[0-9]+\.[0-9]+'", result['stdout']) if result['returncode'] == 0 else None
        if not owner:
            return
        self._notification_server = owner[0].strip("'")
        filters = ["type='method_call',interface='org.freedesktop.Notifications',member='Notify',arg0='put.gd'",
                   "type='method_return',sender='"+self._notification_server+"'",
                   "type='signal',interface='org.freedesktop.Notifications',sender='"+self._notification_server+"'"]
        script = 'nohup dbus-monitor --session '+shlex.join(filters)+' > '+shlex.quote(self.target.root+'/notifications.log')+' 2>&1 < /dev/null & echo $!'
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

    def click_accessible(self, query, button='left'):
        return self._accessible_pointer(query,button)

    def hover_accessible(self, query):
        return self._accessible_pointer(query,None)

    def _accessible_pointer(self, query, button):
        """Real pointer input to one visible desktop node, with a fresh hit-test.

        This is for desktop-drawn content (notifications/tray/browser), not a
        D-Bus Activate/AT-SPI doAction shortcut. Query must include ownership
        (pid/application/owned); arbitrary first-match selection is forbidden.
        """
        if not any(k in query for k in ('pid','application','owned')):
            raise RecipeBlocked('desktop input query requires pid, application, or owned')
        nodes = self.accessible(dict(query, visible=True, showing=True))
        nodes = [n for n in nodes if n.get('rect') and min(n['rect'][2:]) > 0 and
                 (n.get('enabled') or n.get('role') in ('label','text','notification'))]
        if len(nodes) != 1:
            raise RecipeBlocked('desktop input control is missing or ambiguous')
        node = nodes[0]
        x,y,w,h = node['rect']
        point = (x+w/2,y+h/2)
        witness = self._probe('foreground', {'desktop_node':node,'point':point,
                              'compositor_script':self.profile.get('focus',{}).get('compositor_script'),
                              'desktop_hit_test':self.profile.get('focus',{}).get('desktop_hit_test'),
                              'shell_application':self.profile.get('notifications',{}).get('accessible',{}).get('application','')})
        if not witness.get('ok'):
            raise RecipeFailed('desktop input is obscured by '+repr(witness.get('obscuring_window','unknown window')))
        mapped = self._map_geometry({'x':point[0],'y':point[1], 'coordinate_space':
                                      'framebuffer' if self.profile['session']=='x11' else 'logical'})
        scale = mapped.get('scale',1)
        px,py = (round((point[i]-mapped.get(('screen_x','screen_y')[i],0))*scale) for i in (0,1))
        if button is None:
            w,h = self._dimensions(mapped['head'])
            if not 0 <= px < w or not 0 <= py < h:
                raise RecipeBlocked('desktop hover point outside selected head')
            self.target.pointer(px,py,w,h,head=mapped['head'])
        else:
            self._pointer_click(px,py,button,mapped['head'],guarded=True)
        return node

    def notification_action(self, identity, action='default'):
        """Click a currently drawn notification; correlate ActionInvoked by ID.

        Profiles select shell accessibility ownership. A Notify call alone is
        never treated as proof that the desktop drew the notification.
        """
        if self.profile.get('notifications',{}).get('actions_available') is False:
            raise RecipeBlocked(self.profile['notifications'].get('reason','notification input unavailable'),issue=670)
        rows = [n for n in self.notifications() if n['id'] == identity and not n.get('closed')]
        if len(rows) != 1:
            raise RecipeBlocked('notification identity is missing, replaced, closed or ambiguous')
        notice = rows[0]
        actions = dict(zip(notice['actions'][::2],notice['actions'][1::2]))
        if action not in actions:
            raise RecipeBlocked('notification does not offer the requested action')
        policy = self.profile.get('notifications', {}).get('accessible')
        if not policy:
            raise RecipeBlocked('profile has no notification accessibility ownership selector')
        # Restrict the summary/body witness to this notification server's tree.
        import glob
        name = self.profile.get('notifications',{}).get('title_name_glob','{summary}').replace('{summary}',glob.escape(notice['summary']))
        titles = self.accessible({**policy, 'name':name, 'visible':True, 'showing':True})
        titles = [n for n in titles if n.get('rect') and min(n['rect'][2:])>0]
        exact = [n for n in titles if n['name']==notice['summary']]
        if exact:
            titles = exact
        if len(titles) != 1:
            raise RecipeBlocked('notification is not uniquely drawn in the desktop accessibility tree')
        title = titles[0]
        if action == 'default':
            query = {**policy, 'pid':title['pid'], 'name':glob.escape(title['name']), 'role':title['role']}
        else:
            self.hover_accessible({**policy,'pid':title['pid'],'name':glob.escape(title['name']),'role':title['role']})
            containers = [n for n in title.get('ancestors',[]) if n['role'] in ('notification','panel','alert') and n.get('rect')]
            buttons = self.accessible({**policy,'pid':title['pid'],'name':glob.escape(actions[action]),
                                       'visible':True,'showing':True,'enabled':True})
            query = None
            # A title's nearest panel can contain just its text/icon. Select
            # the smallest common ancestor which also contains one action.
            for ancestor in reversed(containers):
                container = ancestor['rect']
                inside = [n for n in buttons if n.get('rect') and min(n['rect'][2:])>0 and
                          container[0] <= n['rect'][0]+n['rect'][2]/2 < container[0]+container[2] and
                          container[1] <= n['rect'][1]+n['rect'][3]/2 < container[1]+container[3]]
                if len(inside)==1:
                    query = {**policy,'pid':title['pid'],'name':glob.escape(actions[action]),
                             'role':inside[0]['role'],'within':container}
                    break
            if query is None:
                raise RecipeBlocked('notification button is not uniquely inside the drawn notification')
        previous = len(notice.get('action_events',[]))
        self.click_accessible(query)
        def invoked():
            return any(e['action']==action for n in self.notifications()
                       if n['id']==identity and all(n.get(k)==notice.get(k) for k in ('at','sender','server'))
                       for e in n.get('action_events',[])[previous:])
        self.wait(invoked, 'real notification action '+action, timeout=5)

    def tray(self):
        return self._probe('tray', {})

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
    """Correlate Notify replies and action/close signals by unique bus owner + ID."""
    import ast
    def strings(block):
        values = re.findall(r'^\s*string ("(?:[^"\\]|\\.)*")\s*$',block,re.M)
        return [ast.literal_eval(v) for v in values]
    blocks = re.split(r'(?m)(?=^(?:method call|method return|signal) )',text)
    rows, pending, active = [], {}, {}
    for block in blocks:
        timestamp = re.search(r'time=([0-9.]+)',block)
        if not timestamp:
            continue
        at = float(timestamp[1])
        sender = re.search(r'\bsender=(\S+)',block)
        if 'member=Notify' in block:
            values = strings(block)
            integers = re.findall(r'^\s*uint32 (\d+)',block,re.M)
            serial = re.search(r'\bserial=(\d+)',block)
            if len(values)>=4 and values[0]=='put.gd' and serial and sender:
                actions = re.search(r'array \[\s*(.*?)\s*\]',block,re.S)
                action_strings = strings(actions[1]) if actions else []
                timeout = re.findall(r'^\s*int32 (-?\d+)',block,re.M)
                hints = {}
                for entry in re.findall(r'dict entry \((.*?)\)',block,re.S):
                    key = strings(entry)
                    value = re.search(r'variant\s+(boolean|int32|uint32|byte|double) (true|false|[-0-9.]+)',entry)
                    if key and value:
                        hints[key[0]] = (value[2]=='true' if value[1]=='boolean' else float(value[2]) if value[1]=='double' else int(value[2]))
                row = {'summary':values[2],'body':values[3],'actions':action_strings,'id':None,
                       'sender':sender[1],'server':None,'timeout':int(timeout[-1]) if timeout else None,
                       'hints':hints, 'resident':hints.get('resident',False),
                       'replaces_id':int(integers[0]) if integers else 0,'at':at,
                       'action_events':[], 'closed':None}
                rows.append(row);pending[(sender[1],serial[1])]=row
        elif block.startswith('method return'):
            serial = re.search(r'\breply_serial=(\d+)',block)
            identity = re.search(r'uint32 (\d+)',block)
            destination = re.search(r'\bdestination=(\S+)',block)
            key = (destination[1],serial[1]) if serial and destination else None
            if identity and sender and key in pending:
                row = pending.pop(key)
                row.update(id=int(identity[1]),server=sender[1])
                previous = active.get((sender[1],row['id']))
                if previous:
                    previous['closed'] = {'at':at,'reason':'replaced'}
                active[(sender[1],row['id'])] = row
        elif sender and ('member=ActionInvoked' in block or 'member=NotificationClosed' in block):
            integers = re.findall(r'^\s*uint32 (\d+)',block,re.M)
            row = active.get((sender[1],int(integers[0]))) if integers else None
            if row is not None:
                if 'member=ActionInvoked' in block:
                    values = strings(block)
                    if values:
                        row['action_events'].append({'action':values[0],'at':at})
                elif len(integers)>1:
                    row['closed'] = {'reason':int(integers[1]),'at':at}
    return [row for row in rows if row['at']>=since]
