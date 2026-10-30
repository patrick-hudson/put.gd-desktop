"""Bounded, target-side observations. No AT-SPI action or D-Bus activation input.

accessible query fields are ANDed: name/text/description/application/window are
case-sensitive globs; role is normalized lowercase with spaces; pid, objectName,
owned, visible/showing/enabled and states constrain ownership/state. Unknown keys
are rejected. rect remains [x,y,width,height] in desktop coordinates; bounds is
its named equivalent. Protected text is never read.
"""
import ctypes
import ctypes.util
import fnmatch
import json
import os
import re
import subprocess
import sys
import time

WINDOW_ROLES = ('frame', 'dialog', 'window', 'file chooser', 'alert')
POPUP_ROLES = ('menu', 'popup menu', 'tool tip')
QUERY_KEYS = {'name', 'text', 'description', 'role', 'pid', 'application', 'window',
              'objectName', 'owned', 'visible', 'showing', 'enabled', 'states', 'within'}


def owned(pid):
    try:
        with open('/proc/' + str(pid) + '/environ', 'rb') as stream:
            return ('XDG_RUNTIME_DIR=' + os.environ['XDG_RUNTIME_DIR']).encode() in stream.read().split(b'\0')
    except (OSError, KeyError):
        return False


# at-spi2-core renamed ATSPI_ROLE_PUSH_BUTTON's name to "button" (Qt 6.10 on
# GNOME 50 reports it); older servers say "push button". One canonical name.
ROLE_SYNONYMS = {'button': 'push button'}


def normalize_role(role):
    role = role.lower().replace('_', ' ').replace('-', ' ')
    return ROLE_SYNONYMS.get(role, role)


def matches(row, query):
    for key, value in query.items():
        if key not in QUERY_KEYS:
            raise ValueError('unknown accessibility query key: ' + key)
        if key in ('name', 'text', 'description', 'application', 'window'):
            if not fnmatch.fnmatchcase(row.get(key, ''), value):
                return False
        elif key == 'role':
            if row['role'] != normalize_role(value):
                return False
        elif key == 'within':
            rect = row.get('rect')
            if not rect or not (value[0] <= rect[0]+rect[2]/2 < value[0]+value[2] and
                                value[1] <= rect[1]+rect[3]/2 < value[1]+value[3]):
                return False
        elif key == 'states':
            if not all(row['states'].get(normalize_role(k), False) == v for k, v in value.items()):
                return False
        elif row.get(key) != value:
            return False
    return True


# Showing popup menus seen through AT-SPI events: {(pid, path): node}.
_POPUPS = None


def watch_popups():
    """Track Qt popup menus, which the application's accessible children omit.

    Qt lists only non-Qt::Popup top-level windows as application children, so a
    context or row menu opened with QMenu::exec() is unreachable by a tree walk.
    Qt does emit object:state-changed:showing for it (ObjectShow) once someone
    listens. Only the persistent probe process lives long enough to hear it,
    and it must register before the menu opens: every input guard probes
    first, which registers it.
    """
    global _POPUPS
    if _POPUPS is not None or __name__ != 'surface_driver_probe':
        return
    import pyatspi
    _POPUPS = {}
    def changed(event):
        try:
            node = event.source
            if normalize_role(node.getRoleName()) not in POPUP_ROLES:
                return
            key = (node.get_process_id(), node.path)
            if event.detail1:
                _POPUPS[key] = node
            else:
                _POPUPS.pop(key, None)
        except Exception:
            pass
    pyatspi.Registry.registerEventListener(changed, 'object:state-changed:showing')


def popup_roots(query):
    """Extra walk roots for showing popups, with the same application scoping."""
    import pyatspi
    roots = []
    for key, node in list((_POPUPS or {}).items()):
        try:
            if not node.getState().contains(pyatspi.STATE_SHOWING):
                _POPUPS.pop(key, None)
                continue
            app = node.get_application()
            name, pid = app.name or '', key[0]
        except Exception:
            _POPUPS.pop(key, None)
            continue
        if query.get('application') and not fnmatch.fnmatchcase(name, query['application']):
            continue
        if (query.get('pid') is not None and pid != query['pid']) or (query.get('owned') and not owned(pid)):
            continue
        roots.append((key, (node, 2, '', name, None, [{'name': name, 'role': 'application', 'rect': None, 'pid': pid}])))
    return roots


def accessible(query):
    import pyatspi
    unknown = set(query) - QUERY_KEYS
    if unknown:
        raise ValueError('unknown accessibility query keys: ' + ', '.join(sorted(unknown)))
    watch_popups()
    matches_ = []
    stack = [(pyatspi.Registry.getDesktop(0), 0, '', '', None, [])]
    # LIFO: popup roots go first so the ordinary tree is walked before them;
    # a popup already reached there (a button's own menu) is not walked twice.
    extra = popup_roots(query)
    stack[:0] = [root for _, root in extra]
    extra_keys = {id(root[0]): key for key, root in extra}
    seen = set()
    visited, truncated = 0, False
    while stack and visited < 20000:
        node, depth, window, application, popup, ancestors = stack.pop()
        visited += 1
        try:
            if id(node) in extra_keys:
                if extra_keys[id(node)] in seen:
                    continue
            elif normalize_role(node.getRoleName()) in POPUP_ROLES:
                seen.add((node.get_process_id(), node.path))
            role, name = normalize_role(node.getRoleName()), node.name or ''
            pid = node.get_process_id()
            # Do not walk unrelated applications for owned-control queries.
            if role == 'application':
                application = name
                if query.get('application') and not fnmatch.fnmatchcase(application,query['application']):
                    continue
                if query.get('pid') is not None and pid != query['pid']:
                    continue
                if query.get('owned') and not owned(pid):
                    continue
            if role in WINDOW_ROLES or (ancestors and ancestors[-1]['role']=='application' and role not in POPUP_ROLES):
                window, popup = name, None
            attributes = dict(a.split(':', 1) for a in node.getAttributes() if ':' in a)
            state_set = node.getState()
            states = {}
            for key in ('visible', 'showing', 'enabled', 'sensitive', 'active', 'focused', 'focusable',
                        'selected', 'expanded', 'checked', 'editable', 'modal', 'iconified'):
                constant = getattr(pyatspi, 'STATE_' + key.upper(), None)
                states[key] = bool(constant is not None and state_set.contains(constant))
            if depth>2 and any(query.get(key) is True and not states[key] for key in ('visible','showing')):
                continue
            potential = all(not query.get(key) or fnmatch.fnmatchcase(value, query[key])
                            for key,value in (('name',name),('application',application),('window',window)))
            potential = potential and (not query.get('role') or role==normalize_role(query['role']))
            rect = None
            try:
                rect = list(node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS))
            except (NotImplementedError, RuntimeError):
                pass
            if role in POPUP_ROLES and states['showing']:
                popup = {'name': name, 'role': role, 'rect': rect, 'pid': pid}
            protected = role == 'password text'
            text = ''
            if potential and not protected:
                try:
                    interface = node.queryText()
                    text = interface.getText(0, min(interface.characterCount, 65536))
                except (NotImplementedError, RuntimeError):
                    pass
            accelerator = ''
            try:
                if not potential:
                    raise NotImplementedError
                action = node.queryAction()
                accelerator = ';'.join(action.getKeyBinding(i) for i in range(action.nActions))
            except (NotImplementedError, RuntimeError):
                pass
            object_name = next((attributes[k] for k in ('id', 'objectName', 'object-name', 'accessible-id') if k in attributes), '')
            row = {'window': window, 'application': application, 'name': name, 'role': role,
                   'text': text, 'description': '' if protected else (node.description or ''),
                   'states': states, 'rect': rect, 'attributes': attributes, 'pid': pid,
                   'owned': owned(pid), 'objectName': object_name, 'accelerator': accelerator,
                   'visible': states['visible'], 'showing': states['showing'], 'enabled': states['enabled'],
                   'protected': protected, 'popup': popup, 'ancestors': ancestors}
            row['bounds'] = dict(zip(('x', 'y', 'width', 'height'), rect)) if rect else None
            if matches(row, query):
                matches_.append(row)
            if depth < 50:
                ancestry = ancestors + [{'name':name,'role':role,'rect':rect,'pid':pid}]
                stack.extend((child, depth + 1, window, application, popup, ancestry) for child in node if child is not None)
            elif getattr(node,'childCount',0):
                truncated = True
        except (RuntimeError, AttributeError, NotImplementedError):
            continue
    if stack or truncated:
        raise RuntimeError('accessibility scan exceeded bounded tree size/depth')
    return matches_


def label(value):
    return (value or '').split('\t')[0].replace('&&', '\0').replace('&', '').replace('\0', '&')


CLASS_ROLES = {'QPushButton': ('push button',), 'QToolButton': ('push button', 'toggle button', 'menu'),
               'QLineEdit': ('text', 'entry'), 'QSpinBox': ('spin button',),
               'QComboBox': ('combo box',), 'QMenu': ('menu', 'popup menu'),
               'QCheckBox': ('check box',), 'QAction': ('menu item', 'check menu item', 'radio menu item')}


def candidate_labels(candidate):
    """Every label a catalog control can carry live: aliases, accessibleName, toolTip, text."""
    values = list(candidate.get('accessibleNames') or [])
    values += [candidate.get(key) for key in ('accessibleName', 'toolTip', 'text')]
    return {label(value) for value in values if isinstance(value, str) and label(value)}


def selector_matches(row, selector, candidates):
    """Match rank for one live row: 0 exported objectName, 1 label, None no match.

    The live objectName wins. Labels come from the selector itself (text:/
    text-glob:) and from every catalog candidate's aliases, accessibleName,
    toolTip and text, so "List" also finds "Show history as a list".
    Rectangle/layout equality is never a substitute for runtime state.
    """
    name, live = row.get('objectName'), label(row.get('name'))
    if selector.startswith('class:'):
        return 1 if row['role'] in CLASS_ROLES.get(selector[6:], ()) else None
    if name and (selector == name or (selector.startswith('glob:') and fnmatch.fnmatchcase(name, selector[5:])) or
                 any(c.get('name') == name for c in candidates)):
        return 0
    if not live:
        return None
    if selector.startswith('text:') and live == selector[5:]:
        return 1
    if selector.startswith('text-glob:') and fnmatch.fnmatchcase(live, selector[10:]):
        return 1
    return 1 if any(live in candidate_labels(c) for c in candidates) else None


def control_geometry(query):
    # No name prefilter: catalog text often differs from the live accessible
    # name, and objectName matches must not be filtered out by labels.
    filters = {'owned':True,'visible':True,'showing':True,
               **({'pid':query['pid']} if query.get('pid') else {})}
    rows = accessible(filters)
    selected = []
    for row in rows:
        if not (row['visible'] and row['showing'] and row['enabled'] and row['rect'] and
                row['rect'][2] > 0 and row['rect'][3] > 0):
            continue
        if query.get('title') and not re.search(query['title'], row['window']) and not row['popup']:
            continue
        if query.get('popup') and not row['popup']:
            continue
        if query.get('objectName'):
            rank = 0 if row['objectName'] == query['objectName'] else None
        else:
            rank = selector_matches(row, query['selector'], query.get('candidates', []))
        if rank is not None:
            selected.append((rank, row))
    if selected:
        best = min(rank for rank, _ in selected)
        selected = [row for rank, row in selected if rank == best]
    # An open menu takes precedence over a same-label sidebar/button.
    popups = [row for row in selected if row['popup']]
    if popups:
        selected = popups
    if len(selected) != 1:
        raise RuntimeError('owned live control is missing, hidden, disabled or ambiguous (' + str(len(selected)) + ' matches)')
    return selected[0]


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.DEVNULL, timeout=5).strip()


def xprop(wid, prop):
    return run('xprop', '-id', str(wid), prop).partition('=')[2].strip()


def xwininfo(wid):
    raw = run('xwininfo', '-id', str(wid))
    def field(label):
        found = re.search(r'^\s*' + re.escape(label) + r':\s*(-?\d+|yes|no)\s*$', raw, re.M)
        if not found:
            raise ValueError('xwininfo omitted ' + label)
        return found[1]
    return {'x': int(field('Absolute upper-left X')), 'y': int(field('Absolute upper-left Y')),
            'width': int(field('Width')), 'height': int(field('Height')),
            'override_redirect': field('Override Redirect State') == 'yes'}


def x11_focus_owner():
    """X input focus (keyboard target), which override-redirect windows never take from EWMH."""
    xlib = ctypes.CDLL(ctypes.util.find_library('X11'))
    xlib.XOpenDisplay.argtypes = [ctypes.c_char_p]; xlib.XOpenDisplay.restype = ctypes.c_void_p
    xlib.XGetInputFocus.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int)]
    xlib.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = xlib.XOpenDisplay(None)
    if not display:
        raise RuntimeError('cannot open X11 display for input focus')
    try:
        focus, revert = ctypes.c_ulong(), ctypes.c_int()
        xlib.XGetInputFocus(display, ctypes.byref(focus), ctypes.byref(revert))
        return str(focus.value)
    finally:
        xlib.XCloseDisplay(display)


def x11_windows(query):
    options = ['--onlyvisible'] if not query.get('include_hidden') else []
    search = subprocess.run(['xdotool', 'search', *options, '--name', query.get('title', '.*')],
                            text=True, capture_output=True)
    found = []
    active_ids = re.findall(r'0x[0-9a-fA-F]+', run('xprop','-root','_NET_ACTIVE_WINDOW'))
    active = str(int(active_ids[-1],16)) if active_ids else None
    for wid in search.stdout.split():
        try:
            try:
                pid = int(run('xdotool', 'getwindowpid', wid))
            except (subprocess.CalledProcessError, ValueError):
                pid = None
            if query.get('owned', True) and (pid is None or not owned(pid)):
                continue
            # xdotool's location translates the child's offset inside a WM
            # frame a second time (native decorated dialogs land 1x title bar
            # low). xwininfo reports the server's absolute client origin.
            info = xwininfo(wid)
            values = {'X': info['x'], 'Y': info['y'], 'WIDTH': info['width'], 'HEIGHT': info['height']}
            state = xprop(wid, '_NET_WM_STATE')
            extents = [int(v) for v in re.findall(r'\d+',xprop(wid,'_NET_FRAME_EXTENTS'))]
            frame = None
            if len(extents)==4:
                left,right,top,bottom=extents
                frame = {'x':int(values['X'])-left,'y':int(values['Y'])-top,
                         'width':int(values['WIDTH'])+left+right,'height':int(values['HEIGHT'])+top+bottom}
            found.append({'x': int(values['X']), 'y': int(values['Y']), 'width': int(values['WIDTH']),
                          'height': int(values['HEIGHT']), 'scale': 1, 'pid': pid, 'window_id': wid,
                          'name': run('xdotool', 'getwindowname', wid), 'coordinate_space': 'framebuffer', 'frame':frame,
                          'override_redirect': info['override_redirect'],
                          'states': {'active':wid==active, 'minimized': '_NET_WM_STATE_HIDDEN' in state,
                                     'maximized': all(v in state for v in ('MAXIMIZED_VERT', 'MAXIMIZED_HORZ'))}})
        except (subprocess.CalledProcessError, ValueError, OSError):
            continue  # A window can close during observation.
    return found


def geometry(query):
    if query.get('backend') == 'x11' or os.environ.get('XDG_SESSION_TYPE') == 'x11':
        found = x11_windows(query)
        if query.get('accessible'):
            nodes = accessible({'owned':True, 'visible':True, **query['accessible']})
            found = [w for w in found if any(n['pid']==w['pid'] and n.get('rect') and
                      n['rect']==[w[k] for k in ('x','y','width','height')] for n in nodes)]
        # Native selector overlays can have one visible window per head. The
        # focused head (or explicitly selected head) disambiguates, never order.
        if query.get('point'):
            px, py = query['point']
            found = [w for w in found if w['x'] <= px < w['x']+w['width'] and w['y'] <= py < w['y']+w['height']]
    else:
        rows = accessible({'owned': True, **query.get('accessible', {})})
        found = []
        for row in rows:
            top = row['role'] in WINDOW_ROLES or (row.get('ancestors') and row['ancestors'][-1]['role']=='application')
            if not top or not re.search(query['title'], row['name']):
                continue
            if not row['visible'] or not row['showing'] or not row['rect'] or min(row['rect'][2:]) <= 0:
                continue
            found.append(dict(zip(('x', 'y', 'width', 'height'), row['rect'])) |
                         {'scale': query.get('scale', 1), 'pid': row['pid'], 'name': row['name'],
                          'coordinate_space': 'logical', 'states': row['states']})
    if len(found) != 1:
        raise RuntimeError('live window match is missing or ambiguous (' + str(len(found)) + ' matches)')
    return found[0]


def intersect(a, b):
    return a['x'] < b['x']+b['width'] and b['x'] < a['x']+a['width'] and a['y'] < b['y']+b['height'] and b['y'] < a['y']+a['height']


def x11_hit(point):
    """Server stacking hit-test, including override-redirect menus/notifications.

    EWMH lists omit those windows. Walk XQueryTree in reverse stacking order
    without moving the pointer, selecting viewable input/output children only.
    """
    class Attributes(ctypes.Structure):
        _fields_ = [(k, ctypes.c_int) for k in ('x','y','width','height','border_width','depth')] + [
            ('visual',ctypes.c_void_p), ('root',ctypes.c_ulong), ('class_',ctypes.c_int),
            ('bit_gravity',ctypes.c_int), ('win_gravity',ctypes.c_int), ('backing_store',ctypes.c_int),
            ('backing_planes',ctypes.c_ulong), ('backing_pixel',ctypes.c_ulong), ('save_under',ctypes.c_int),
            ('colormap',ctypes.c_ulong), ('map_installed',ctypes.c_int), ('map_state',ctypes.c_int),
            ('all_event_masks',ctypes.c_long), ('your_event_mask',ctypes.c_long),
            ('do_not_propagate_mask',ctypes.c_long), ('override_redirect',ctypes.c_int), ('screen',ctypes.c_void_p)]
    xlib = ctypes.CDLL(ctypes.util.find_library('X11'))
    window_t = ctypes.c_ulong
    xlib.XOpenDisplay.argtypes = [ctypes.c_char_p]; xlib.XOpenDisplay.restype = ctypes.c_void_p
    xlib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]; xlib.XDefaultRootWindow.restype = window_t
    xlib.XQueryTree.argtypes = [ctypes.c_void_p,window_t,ctypes.POINTER(window_t),ctypes.POINTER(window_t),ctypes.POINTER(ctypes.POINTER(window_t)),ctypes.POINTER(ctypes.c_uint)]
    xlib.XGetWindowAttributes.argtypes = [ctypes.c_void_p,window_t,ctypes.POINTER(Attributes)]
    xlib.XTranslateCoordinates.argtypes = [ctypes.c_void_p,window_t,window_t,ctypes.c_int,ctypes.c_int,ctypes.POINTER(ctypes.c_int),ctypes.POINTER(ctypes.c_int),ctypes.POINTER(window_t)]
    xlib.XFree.argtypes = [ctypes.c_void_p]; xlib.XCloseDisplay.argtypes = [ctypes.c_void_p]
    error_handler_t = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
    ignore_errors = error_handler_t(lambda display,event:0)
    xlib.XSetErrorHandler.argtypes = [ctypes.c_void_p]
    xlib.XSetErrorHandler.restype = ctypes.c_void_p
    previous_handler = xlib.XSetErrorHandler(ctypes.cast(ignore_errors,ctypes.c_void_p))
    display = xlib.XOpenDisplay(None)
    if not display:
        xlib.XSetErrorHandler(previous_handler)
        raise RuntimeError('cannot open X11 display for stacking hit-test')
    try:
        root = xlib.XDefaultRootWindow(display)
        current, path = root, []
        for _ in range(30):
            root_out, parent, child = window_t(), window_t(), window_t()
            children, count = ctypes.POINTER(window_t)(), ctypes.c_uint()
            if not xlib.XQueryTree(display,current,ctypes.byref(root_out),ctypes.byref(parent),ctypes.byref(children),ctypes.byref(count)):
                raise RuntimeError('X11 stacking tree unavailable')
            try:
                ordered = list(children[:count.value])
            finally:
                if children:
                    xlib.XFree(children)
            found = None
            for wid in reversed(ordered):
                attrs, gx, gy = Attributes(), ctypes.c_int(), ctypes.c_int()
                if not xlib.XGetWindowAttributes(display,wid,ctypes.byref(attrs)) or attrs.map_state != 2 or attrs.class_ != 1:
                    continue
                if not xlib.XTranslateCoordinates(display,wid,root,0,0,ctypes.byref(gx),ctypes.byref(gy),ctypes.byref(child)):
                    continue
                if gx.value <= point[0] < gx.value+attrs.width and gy.value <= point[1] < gy.value+attrs.height:
                    found = wid
                    break
            if found is None:
                break
            path.append(str(found)); current = found
        pid, name = None, ''
        for wid in reversed(path):
            if not name:
                try:
                    name = run('xdotool','getwindowname',wid)
                except subprocess.CalledProcessError:
                    pass
            if pid is None:
                try:
                    pid = int(run('xdotool','getwindowpid',wid))
                except (subprocess.CalledProcessError,ValueError):
                    pass
        return {'path':path,'pid':pid,'name':name or 'unnamed X11 window'}
    finally:
        try:
            xlib.XCloseDisplay(display)
        finally:
            xlib.XSetErrorHandler(previous_handler)


def x11_foreground(query):
    if query.get('desktop_node'):
        hit = x11_hit(query['point'])
        return {'ok': hit['pid'] == query['desktop_node']['pid'], 'backend':'x11-hit-test',
                'obscuring_window':hit['name'], 'obscuring_window_id':hit['path'][-1] if hit['path'] else None}
    target = query['geometry']
    wid = str(target['window_id'])
    if target.get('override_redirect'):
        return x11_unmanaged_foreground(query)
    if query.get('raise'):
        subprocess.run(['xdotool', 'windowactivate', '--sync', wid], check=True, timeout=3,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(['xdotool', 'windowraise', wid], check=True, timeout=3,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    active_raw = run('xprop', '-root', '_NET_ACTIVE_WINDOW')
    active_ids = re.findall(r'0x[0-9a-fA-F]+', active_raw)
    active = str(int(active_ids[-1], 16)) if active_ids else None
    stack = [str(int(v, 16)) for v in re.findall(r'0x[0-9a-fA-F]+', run('xprop', '-root', '_NET_CLIENT_LIST_STACKING'))]
    all_windows = x11_windows({'title': '.', 'owned': False})
    by_id = {w['window_id']: w for w in all_windows}
    popup = query.get('popup')
    point = query.get('point')
    obstruction = None
    # Evaluate stacking at the actual pointer point, including other windows
    # belonging to the same private helper process (first-launch Settings).
    if wid in stack:
        for above in reversed(stack[stack.index(wid)+1:]):
            w = by_id.get(above)
            if w and (intersect(w, target) if point is None else
                      w['x'] <= point[0] < w['x']+w['width'] and w['y'] <= point[1] < w['y']+w['height']):
                obstruction = w
                break
    # Override-redirect menus are not in EWMH stacking. Require an owned live
    # popup containing the input point, with the same active client process.
    active_window = by_id.get(active, {})
    popup_ok = bool(popup and active_window.get('pid') == target.get('pid') and point and
                    popup['rect'][0] <= point[0] < popup['rect'][0]+popup['rect'][2] and
                    popup['rect'][1] <= point[1] < popup['rect'][1]+popup['rect'][3])
    ok = active == wid and wid in stack and not obstruction
    if popup_ok and not obstruction:
        ok = True
    obscurer = obstruction or (active_window if active != wid else {})
    if point:
        hit = x11_hit(point)
        hit_ok = wid in hit['path'] or (popup_ok and hit['pid'] == target.get('pid'))
        if not hit_ok and target.get('frame'):
            # Decorations belong to the WM frame, not the client. Establish the
            # client's actual parent from X11 before allowing header/edge input.
            parent = re.search(r'Parent window id:\s*(0x[0-9a-fA-F]+)',run('xwininfo','-id',wid,'-tree'))
            frame = target['frame']
            hit_ok = bool(parent and str(int(parent[1],16)) in hit['path'] and
                          frame['x'] <= point[0] < frame['x']+frame['width'] and
                          frame['y'] <= point[1] < frame['y']+frame['height'])
        if not hit_ok:
            ok = False
            obscurer = {'name':hit['name'], 'window_id':hit['path'][-1] if hit['path'] else None}
    return {'ok': ok, 'active': active, 'target': wid, 'stacking': stack,
            'obscuring_window': obscurer.get('name', 'unknown foreground window'),
            'obscuring_window_id': obscurer.get('window_id'), 'backend': 'ewmh'}


def x11_pointer():
    """Root pointer position and button mask (XQueryPointer), without moving it."""
    xlib = ctypes.CDLL(ctypes.util.find_library('X11'))
    window_t = ctypes.c_ulong
    xlib.XOpenDisplay.argtypes = [ctypes.c_char_p]; xlib.XOpenDisplay.restype = ctypes.c_void_p
    xlib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]; xlib.XDefaultRootWindow.restype = window_t
    xlib.XQueryPointer.argtypes = [ctypes.c_void_p, window_t, ctypes.POINTER(window_t), ctypes.POINTER(window_t),
                                   ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                   ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint)]
    xlib.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = xlib.XOpenDisplay(None)
    if not display:
        raise RuntimeError('cannot open X11 display for pointer state')
    try:
        root, child = window_t(), window_t()
        rx, ry, wx, wy, mask = ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_uint()
        xlib.XQueryPointer(display, xlib.XDefaultRootWindow(display), ctypes.byref(root), ctypes.byref(child),
                           ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(mask))
        return rx.value, ry.value, mask.value
    finally:
        xlib.XCloseDisplay(display)


def x11_ancestors(wid):
    chain = [str(wid)]
    for _ in range(12):
        parent = re.search(r'Parent window id:\s*(0x[0-9a-fA-F]+)', run('xwininfo', '-id', chain[-1], '-tree'))
        if not parent or int(parent[1], 16) in (0, int(chain[-1])):
            break
        chain.append(str(int(parent[1], 16)))
    return chain


def x11_unmanaged_foreground(query):
    """Owned override-redirect windows (recording bar, selector covers).

    They are outside EWMH stacking and never become _NET_ACTIVE_WINDOW, so the
    witness is the server hit-test: pointer input needs the target in the
    path at the input point (focus() selects at its centre); during a held
    drag, at the pointer's position. Keyboard input needs the X input focus
    inside the target; a show-without-activating window
    normally refuses it, naming where keys would really go.
    """
    target = query['geometry']
    wid = str(target['window_id'])
    if not owned(target.get('pid')):
        return {'ok': False, 'backend': 'x11-unmanaged', 'obscuring_window': 'unowned override-redirect target',
                'obscuring_window_id': wid}
    point = query.get('point')
    if point is None:
        # A held button (drag in progress) means the implicit X grab routes
        # motion to the pressed window, not the keyboard focus: hit-test the
        # pointer's current position instead.
        px, py, mask = x11_pointer()
        if mask & (0x1f << 8):  # Button1Mask..Button5Mask
            point = (px, py)
    if point is not None:
        hit = x11_hit(point)
        ok = wid in hit['path']
        return {'ok': ok, 'backend': 'x11-unmanaged-hit-test', 'target': wid,
                'obscuring_window': None if ok else hit['name'],
                'obscuring_window_id': None if ok else (hit['path'][-1] if hit['path'] else None)}
    focus = x11_focus_owner()
    ok = focus not in ('0', '1') and wid in x11_ancestors(focus)
    name = ''
    if not ok and focus not in ('0', '1'):
        try:
            name = run('xdotool', 'getwindowname', focus)
        except subprocess.CalledProcessError:
            pass
    return {'ok': ok, 'backend': 'x11-input-focus', 'target': wid,
            'obscuring_window': None if ok else 'keyboard focus ' + repr(name or 'unnamed window') +
                ' (override-redirect target does not take keyboard focus)',
            'obscuring_window_id': None if ok else focus}


SHELL_PICKS = ('shell-at-spi', 'gnome-shell-at-spi')


def shell_hit(query):
    """The compositor shell's own AT-SPI stage pick (GNOME Shell, Cinnamon), not an app-local pick."""
    import pyatspi
    expected = query['desktop_node']
    if not fnmatch.fnmatchcase(expected.get('application',''), query['shell_application']):
        raise RuntimeError('desktop hit-test requires the profile compositor application')
    point = query['point']
    desktop = pyatspi.Registry.getDesktop(0)
    roots = [app for app in desktop if app and app.get_process_id()==expected['pid']]
    for app in roots:
        for window in app:
            try:
                node = window.queryComponent().getAccessibleAtPoint(int(point[0]),int(point[1]),pyatspi.DESKTOP_COORDS)
                for _ in range(50):
                    if node is None:
                        break
                    if node.get_process_id()==expected['pid'] and (node.name or '')==expected['name'] and normalize_role(node.getRoleName())==expected['role']:
                        return {'ok':True,'backend':'shell-at-spi-pick','obscuring_window':None}
                    child = node.queryComponent().getAccessibleAtPoint(int(point[0]),int(point[1]),pyatspi.DESKTOP_COORDS)
                    if child == node:
                        break
                    node = child
            except (RuntimeError,NotImplementedError,AttributeError):
                continue
    return {'ok':False,'backend':'shell-at-spi-pick','obscuring_window':'compositor shell stage did not pick the expected notification control'}


def foreground(query):
    try:
        watch_popups()
    except ImportError:
        pass
    # Compositor-drawn content (notification banners) is not an X window: an
    # X11 hit-test at a Cinnamon banner reaches the client window beneath it.
    # Muffin/Mutter shells pick their own stage through AT-SPI on X11 too.
    if query.get('desktop_node') and query.get('desktop_hit_test') in SHELL_PICKS:
        return shell_hit(query)
    if os.environ.get('XDG_SESSION_TYPE') == 'x11':
        return x11_foreground(query)
    script = query.get('compositor_script')
    if script:
        result = subprocess.run(script, shell=True, input=json.dumps(query), text=True,
                                capture_output=True, timeout=5)
        if result.returncode:
            raise RuntimeError('profile compositor focus witness failed')
        witness = json.loads(result.stdout)
        if not all(k in witness for k in ('ok', 'obscuring_window')):
            raise RuntimeError('profile compositor focus witness has invalid shape')
        return witness
    # AT-SPI focus alone cannot prove compositor stacking on Wayland. The
    # resident's public view-state currently has no focus/occlusion witness.
    # Do not invent those fields or accept an arbitrary frame as pixel proof.
    raise RuntimeError('Wayland compositor focus/stacking witness unavailable; configure focus.compositor_script')


def windows(query):
    if os.environ.get('XDG_SESSION_TYPE') == 'x11':
        return x11_windows(query)
    return [row for row in accessible(query) if row['role'] in WINDOW_ROLES or
            (row.get('ancestors') and row['ancestors'][-1]['role']=='application')]


def tray(query):
    """Read-only SNI properties belonging to the private resident family."""
    import dbus
    import hashlib
    bus = dbus.SessionBus()
    watcher = bus.get_object('org.kde.StatusNotifierWatcher','/StatusNotifierWatcher')
    items = dbus.Interface(watcher,'org.freedesktop.DBus.Properties').Get('org.kde.StatusNotifierWatcher','RegisteredStatusNotifierItems')
    daemon = dbus.Interface(bus.get_object('org.freedesktop.DBus','/org/freedesktop/DBus'),'org.freedesktop.DBus')
    rows = []
    for item in items:
        service, separator, suffix = str(item).partition('/')
        path = '/'+suffix if separator else '/StatusNotifierItem'
        pid = int(daemon.GetConnectionUnixProcessID(service))
        if not owned(pid) or query.get('pid') not in (None,pid):
            continue
        properties = dbus.Interface(bus.get_object(service,path),'org.freedesktop.DBus.Properties').GetAll('org.kde.StatusNotifierItem')
        pixmaps = []
        for width,height,data in properties.get('IconPixmap',[]):
            payload = bytes(data)
            pixmaps.append({'width':int(width),'height':int(height),'bytes':len(payload),'sha256':hashlib.sha256(payload).hexdigest()})
        tooltip = properties.get('ToolTip',('',[],'',''))
        rows.append({'pid':pid,'service':service,'path':path,'id':str(properties.get('Id','')),
                     'title':str(properties.get('Title','')),'status':str(properties.get('Status','')),
                     'icon_name':str(properties.get('IconName','')),'icon_pixmaps':pixmaps,
                     'tooltip':{'title':str(tooltip[2]),'text':str(tooltip[3])},'menu':str(properties.get('Menu',''))})
    return rows


NOTIFY_INTERFACE = 'org.freedesktop.Notifications'


class NotificationFilter:
    """Keep only the private family's own notification traffic (finding 8).

    dbus-monitor sees every application's Notify replies and every
    ActionInvoked/NotificationClosed signal the server broadcasts. A block is
    kept only when it is a put.gd Notify from a private-family sender, the
    server's reply to that exact call, or a server signal for an ID that reply
    returned. Everything else is dropped before it reaches the log.
    """
    def __init__(self, server, sender_owned):
        self.server, self.sender_owned = server, sender_owned
        self.pending, self.ids = {}, set()

    def keep(self, block):
        header = block.split('\n', 1)[0]
        sender = re.search(r'\bsender=(\S+)', header)
        sender = sender[1] if sender else None
        if header.startswith('method call') and 'member=Notify' in header:
            serial = re.search(r'\bserial=(\d+)', header)
            app = re.search(r'^\s*string "((?:[^"\\]|\\.)*)"', block, re.M)
            if serial and sender and app and app[1] == 'put.gd' and self.sender_owned(sender):
                if len(self.pending) >= 256:
                    self.pending.pop(next(iter(self.pending)))
                self.pending[(sender, serial[1])] = True
                return True
            return False
        if header.startswith('method return'):
            serial = re.search(r'\breply_serial=(\d+)', header)
            destination = re.search(r'\bdestination=(\S+)', header)
            identity = re.search(r'^\s*uint32 (\d+)', block, re.M)
            key = (destination[1], serial[1]) if serial and destination else None
            if sender == self.server and key in self.pending and identity:
                del self.pending[key]
                self.ids.add(int(identity[1]))
                return True
            return False
        if header.startswith('signal') and sender == self.server and 'interface=' + NOTIFY_INTERFACE in header:
            identity = re.search(r'^\s*uint32 (\d+)', block, re.M)
            if identity and int(identity[1]) in self.ids:
                if 'member=NotificationClosed' in header:
                    self.ids.discard(int(identity[1]))
                return True
        return False


def sender_pid(sender):
    reply = run('gdbus', 'call', '--session', '--dest', 'org.freedesktop.DBus', '--object-path',
                '/org/freedesktop/DBus', '--method', 'org.freedesktop.DBus.GetConnectionUnixProcessID', sender)
    found = re.search(r'uint32 (\d+)', reply)
    return int(found[1]) if found else None


def notification_monitor(query):
    """Long-running: filter dbus-monitor into query['log'] until terminated."""
    import select
    import signal
    owners = {}
    def sender_owned(sender):
        if sender not in owners:
            try:
                owners[sender] = owned(sender_pid(sender))
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError):
                owners[sender] = False
        return owners[sender]
    keeper = NotificationFilter(query['server'], sender_owned)
    rules = ["type='method_call',interface='" + NOTIFY_INTERFACE + "',member='Notify',arg0='put.gd'",
             "type='method_return',sender='" + query['server'] + "'",
             "type='signal',interface='" + NOTIFY_INTERFACE + "',sender='" + query['server'] + "'"]
    def die_with_parent():
        ctypes.CDLL(None).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    child = subprocess.Popen(['dbus-monitor', '--session', *rules], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, preexec_fn=die_with_parent)
    signal.signal(signal.SIGTERM, lambda *_: (child.terminate(), sys.exit(0)))
    descriptor = os.open(query['log'], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    buffer, block = b'', []
    def flush():
        if block and keeper.keep(''.join(block)):
            os.write(descriptor, ''.join(block).encode())
        block.clear()
    while True:
        # dbus-monitor ends a message only by starting the next one; an idle
        # gap completes the current block so the last signal is not delayed.
        ready, _, _ = select.select([child.stdout], [], [], .05)
        if not ready:
            flush()
            continue
        chunk = os.read(child.stdout.fileno(), 65536)
        if not chunk:
            flush()
            break
        buffer += chunk
        while b'\n' in buffer:
            raw, buffer = buffer.split(b'\n', 1)
            line = raw.decode('utf-8', 'replace') + '\n'
            if line[:1] not in (' ', '\t', '\n') and block:
                flush()
            block.append(line)
    return {'exit': child.wait()}


def notification_log(query):
    """Read the filtered log written by this run's own monitor (bounded)."""
    try:
        os.kill(int(query['pid']), 0)
        alive = True
    except (OSError, ValueError, TypeError):
        alive = False
    try:
        with open(query['log'], 'rb') as stream:
            data = stream.read(8 * 1024 * 1024 + 1)
    except FileNotFoundError:
        data = b''
    if len(data) > 8 * 1024 * 1024:
        raise RuntimeError('notification log exceeded 8 MiB')
    return {'alive': alive, 'text': data.decode('utf-8', 'replace')}


def main():
    request = json.loads(sys.argv[1])
    try:
        function = {'accessible': accessible, 'geometry': geometry, 'control': control_geometry,
                    'foreground': foreground, 'windows': windows, 'tray':tray,
                    'notification_monitor': notification_monitor, 'notification_log': notification_log}[request['probe']]
        print(json.dumps({'ok': True, 'result': function(request['query'])}))
    except Exception as error:
        print(json.dumps({'ok': False, 'error': type(error).__name__ + ': ' + str(error)}))


if __name__ == '__main__':
    main()
