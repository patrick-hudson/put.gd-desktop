"""The X server's view of the desktop (Cinnamon/Muffin on X11), read through python-xlib: the window tree, the EWMH root properties and the pointer.

Same interface as the KWin fence. Windows come from _NET_CLIENT_LIST_STACKING (bottom to top) and from the override-redirect windows
(Qt popup menus, tooltips) that are root children; a client's frame is its geometry grown by _NET_FRAME_EXTENTS. Cinnamon's own panels,
menus and notification bubbles are Clutter actors of its compositor stage and are not X windows, so the fence cannot see them.
"""
import threading
import time

from Xlib import X, display, error

OVERLAY_CLASSES = ('putgd-region',)
POPUP_TYPES = ('MENU', 'POPUP_MENU', 'DROPDOWN_MENU', 'COMBO', 'TOOLTIP', 'NOTIFICATION')
STATIC_TTL = 1.5


class X11Fence:
    def __init__(self):
        self.snapshot = {'windows': [], 'outputs': [], 'cursor': [0, 0]}
        self.stamp = 0.0
        self.serial = 0
        self.errors = 0
        self.last_error = ''
        self.lock = threading.Lock()
        self.stopping = threading.Event()
        self.thread = None
        self.d = None
        self.static = {}
        self.parents = {}
        self.outputs_at = 0.0
        self.outputs_cache = []

    # -- X plumbing ------------------------------------------------------------------------------
    def connect(self):
        self.d = display.Display()
        self.root = self.d.screen().root
        names = ['_NET_CLIENT_LIST_STACKING', '_NET_ACTIVE_WINDOW', '_NET_WM_PID', '_NET_WM_NAME', '_NET_WM_WINDOW_TYPE', '_NET_WM_STATE',
                 '_NET_WM_STATE_HIDDEN', '_NET_WM_STATE_FULLSCREEN', '_NET_FRAME_EXTENTS', 'WM_TRANSIENT_FOR', 'UTF8_STRING', '_NET_WM_STATE_ABOVE']
        self.atom = {n: self.d.intern_atom(n) for n in names}
        self.type_names = {}

    def atom_name(self, atom):
        if atom not in self.type_names:
            self.type_names[atom] = self.d.get_atom_name(atom)
        return self.type_names[atom]

    def prop(self, window, name, kind=X.AnyPropertyType):
        try:
            reply = window.get_full_property(self.atom[name] if isinstance(name, str) and name in self.atom else name, kind)
        except (error.BadWindow, error.XError):
            return None
        return reply.value if reply else None

    def info(self, window, now):
        """The properties that change rarely: pid, class, title, window type. Cached briefly."""
        cached = self.static.get(window.id)
        if cached and now - cached[0] < STATIC_TTL:
            return cached[1]
        pid = self.prop(window, '_NET_WM_PID')
        try:
            wm_class = window.get_wm_class()
        except (error.BadWindow, error.XError):
            wm_class = None
        title = self.prop(window, '_NET_WM_NAME', self.atom['UTF8_STRING'])
        types = self.prop(window, '_NET_WM_WINDOW_TYPE')
        transient = self.prop(window, 'WM_TRANSIENT_FOR')
        info = {'pid': int(pid[0]) if pid is not None and len(pid) else 0, 'cls': (wm_class[0] if wm_class else '').lower(),
                'title': bytes(title).decode('utf-8', 'replace')[:120] if title is not None else '',
                'types': [self.atom_name(a).replace('_NET_WM_WINDOW_TYPE_', '') for a in types] if types is not None else [],
                'tr': str(int(transient[0])) if transient is not None and len(transient) and int(transient[0]) else None}
        self.static[window.id] = (now, info)
        return info

    def frame_of(self, window):
        """The root child that holds this client (its frame), cached."""
        cached = self.parents.get(window.id)
        if cached:
            return cached
        current = window
        for _ in range(4):
            parent = current.query_tree().parent
            if parent.id == self.root.id:
                self.parents[window.id] = current.id
                return current.id
            current = parent
        return window.id

    # -- the snapshot ----------------------------------------------------------------------------
    def monitors(self, now):
        if now - self.outputs_at < 0.8:
            return self.outputs_cache
        self.outputs_at = now
        monitors = []
        try:
            for m in self.root.xrandr_get_monitors().monitors:
                monitors.append({'name': self.atom_name(m.name), 'geom': [m.x, m.y, m.width_in_pixels, m.height_in_pixels], 'scale': 1})
        except (error.XError, AttributeError):
            geo = self.root.get_geometry()
            monitors = [{'name': 'screen', 'geom': [0, 0, geo.width, geo.height], 'scale': 1}]
        self.outputs_cache = sorted(monitors, key=lambda m: (m['geom'][0], m['geom'][1]))
        return self.outputs_cache

    def take(self):
        now = time.time()
        d, root = self.d, self.root
        tree = root.query_tree().children
        order = {w.id: i for i, w in enumerate(tree)}
        active = self.prop(root, '_NET_ACTIVE_WINDOW')
        active_id = int(active[0]) if active is not None and len(active) else 0
        clients = self.prop(root, '_NET_CLIENT_LIST_STACKING')
        windows = []
        seen = set()
        for cid in (clients if clients is not None else []):
            cid = int(cid)
            window = d.create_resource_object('window', cid)
            try:
                attrs = window.get_attributes()
                geo = window.get_geometry()
                origin = root.translate_coords(window, 0, 0)  # the window's origin in root coordinates
            except (error.BadWindow, error.XError):
                continue
            info = self.info(window, now)
            extents = self.prop(window, '_NET_FRAME_EXTENTS')
            left, right, top, bottom = (int(v) for v in extents[:4]) if extents is not None and len(extents) >= 4 else (0, 0, 0, 0)
            x, y = origin.x, origin.y
            state = self.prop(window, '_NET_WM_STATE')
            states = [self.atom_name(a) for a in state] if state is not None else []
            types = info['types']
            frame = [x - left, y - top, geo.width + left + right, geo.height + top + bottom]
            seen.add(self.frame_of(window))
            windows.append((order.get(self.frame_of(window), -1), {
                'id': str(cid), 'pid': info['pid'], 'cls': info['cls'], 'title': info['title'], 'frame': frame, 'content': [x, y, geo.width, geo.height],
                'active': cid == active_id, 'min': attrs.map_state != X.IsViewable or '_NET_WM_STATE_HIDDEN' in states,
                'popup': any(t in POPUP_TYPES for t in types), 'dock': 'DOCK' in types, 'desktop': 'DESKTOP' in types,
                'normal': (not types or 'NORMAL' in types or 'DIALOG' in types), 'notif': 'NOTIFICATION' in types,
                'fullscreen': '_NET_WM_STATE_FULLSCREEN' in states, 'tr': info['tr'], 'kind': 'X11Window', 'types': types, 'states': states}))
        for window in tree:  # override-redirect windows: Qt's menus and tooltips, and the region selector when it is one
            if window.id in seen:
                continue
            try:
                attrs = window.get_attributes()
                if not attrs.override_redirect or attrs.map_state != X.IsViewable or attrs.win_class == X.InputOnly:
                    continue
                geo = window.get_geometry()
                origin = root.translate_coords(window, 0, 0)  # the window's origin in root coordinates
            except (error.BadWindow, error.XError):
                continue
            if geo.width < 3 or geo.height < 3:
                continue  # Cinnamon keeps 1x1 override-redirect helpers
            info = self.info(window, now)
            if not info['pid']:
                continue  # the compositor's own windows have no client
            frame = [origin.x, origin.y, geo.width, geo.height]
            windows.append((order.get(window.id, 0), {
                'id': str(window.id), 'pid': info['pid'], 'cls': info['cls'], 'title': info['title'], 'frame': frame, 'content': frame,
                'active': False, 'min': False, 'popup': True, 'dock': False, 'desktop': False, 'normal': False, 'notif': False, 'fullscreen': False,
                'tr': info['tr'], 'kind': 'X11Window', 'types': info['types'], 'states': ['override-redirect']}))
        monitors = self.monitors(now)
        for _, w in windows:  # put.gd's selector and recording shade are borderless override-redirect windows that cover a whole output
            fx, fy, fw, fh = w['frame']
            if 'override-redirect' in w['states'] and any(fw >= 0.9 * m['geom'][2] and fh >= 0.9 * m['geom'][3] and abs(fx - m['geom'][0]) <= 8 and abs(fy - m['geom'][1]) <= 8
                                                         for m in monitors):
                w['kind'], w['popup'] = 'X11Overlay', False
        windows.sort(key=lambda item: item[0])
        out = []
        for z, (_, w) in enumerate(windows):
            w['z'] = z
            out.append(w)
        pointer = root.query_pointer()
        if len(self.static) > 400:
            self.static = {k: v for k, v in self.static.items() if now - v[0] < 30}
            self.parents = {}
        return {'windows': out, 'outputs': monitors, 'cursor': [pointer.root_x, pointer.root_y]}

    # -- the loop --------------------------------------------------------------------------------
    def update(self, snapshot):
        with self.lock:
            self.snapshot, self.stamp, self.serial = snapshot, time.time(), self.serial + 1

    def get(self):
        with self.lock:
            return self.snapshot, self.stamp

    def run(self):
        while not self.stopping.is_set():
            started = time.time()
            try:
                self.update(self.take())
            except Exception as problem:  # a window vanishing mid-read must not end the fence
                self.errors += 1
                self.last_error = repr(problem)[:200]
                time.sleep(0.2)
            self.stopping.wait(max(0.0, 0.03 - (time.time() - started)))

    def start(self):
        self.connect()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
        deadline = time.time() + 5
        while self.serial == 0 and time.time() < deadline:
            time.sleep(0.05)
        if self.serial == 0:
            raise RuntimeError('the X11 fence never reported: ' + self.last_error)

    def stop(self):
        self.stopping.set()

    def close(self, window_id):
        """Ask the window manager to close a client (_NET_CLOSE_WINDOW), as a title bar button does."""
        from Xlib import protocol
        window = self.d.create_resource_object('window', int(window_id))
        atom = self.d.intern_atom('_NET_CLOSE_WINDOW')
        event = protocol.event.ClientMessage(window=window, client_type=atom, data=(32, [X.CurrentTime, 1, 0, 0, 0]))
        self.root.send_event(event, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
        self.d.flush()
        return 'closed'
