"""Bounded, target-side observations. No AT-SPI action or D-Bus activation input.

accessible query fields are ANDed: name/text/description/application/window are
case-sensitive globs; role is normalized lowercase with spaces; pid, objectName,
owned, visible/showing/enabled and states constrain ownership/state. Unknown keys
are rejected. rect remains [x,y,width,height] in desktop coordinates; bounds is
its named equivalent. On Wayland, client rows are placed through the query's
'compositor' witness (WaylandPlacement); unplaceable rows carry a null rect and
coordinate_error instead of window-relative numbers. Protected text is never read.
"""
import ctypes
import ctypes.util
import fnmatch
import json
import os
import re
import contextlib
import signal
import subprocess
import sys
import time

WINDOW_ROLES = ('frame', 'dialog', 'window', 'file chooser', 'alert')
POPUP_ROLES = ('menu', 'popup menu', 'tool tip')
QUERY_KEYS = {'name', 'text', 'description', 'role', 'pid', 'application', 'window',
              'objectName', 'owned', 'visible', 'showing', 'enabled', 'states', 'within', 'compositor', 'coordinates'}
# Every AT-SPI call answers or fails within CALL_TIMEOUT_MS (raw D-Bus walk,
# RawNode; libatspi, where still used, gets the same bound instead of its
# longer wait for recently registered applications). A walk that cannot
# finish within WALK_SECONDS of the fresh child's start stops with the
# slowest application named instead of a bare timeout (#805); budget plus
# one slow call stays under the 11 s stall dump and the 12 s kill.
CALL_TIMEOUT_MS = 3000
WALK_SECONDS = 7.5
PROCESS_STARTED = None  # set at a fresh child's start (main() or the server's child): the walk budget counts from there
VERIFIED_HELPER_SHA256 = {}  # Supplied over stdin from the controller-side installation receipt.


def bound_calls():
    # pyatspi has loaded libatspi's binding; the timeout is process-wide.
    atspi = sys.modules.get('gi.repository.Atspi')
    if atspi is not None:
        atspi.set_timeout(CALL_TIMEOUT_MS, 0)


def unanswered(error):
    """A D-Bus call that timed out (libatspi GLib.Error or dbus-python), not a vanished object."""
    text = str(error)
    return any(marker in text for marker in ('Timeout was reached', 'NoReply', 'Did not receive a reply', 'timed out'))


def executable(pid):
    """Local identity of a process: executable and argv basenames (no AT-SPI call)."""
    names = []
    try:
        names.append(os.path.basename(os.readlink('/proc/%d/exe' % pid)))
    except (OSError, TypeError):
        pass
    try:
        with open('/proc/%d/cmdline' % pid, 'rb') as stream:
            names += [os.path.basename(a.decode('utf-8', 'replace')) for a in stream.read().split(b'\0')[:2] if a]
    except (OSError, TypeError):
        pass
    return list(dict.fromkeys(n for n in names if n))


def application_names():
    """{a11y bus name: accessible name, None (no answer) or False (gone)} for every application.

    Asked concurrently, so applications that never answer (busy or wedged
    GTK session services, #805) cost one bounded wait in total, not
    CALL_TIMEOUT_MS each in turn. None when the registry cannot be read.
    """
    try:
        import dbus
        import dbus.bus
        import dbus.mainloop.glib
        from gi.repository import GLib
    except ImportError:
        return None
    seconds = CALL_TIMEOUT_MS / 1000
    try:
        address = dbus.Interface(dbus.SessionBus().get_object('org.a11y.Bus', '/org/a11y/bus'),
                                 'org.a11y.Bus').GetAddress(timeout=seconds)
        connection = dbus.bus.BusConnection(str(address), mainloop=dbus.mainloop.glib.DBusGMainLoop())
        registry = connection.get_object('org.a11y.atspi.Registry', '/org/a11y/atspi/accessible/root', introspect=False)
        children = registry.GetChildren(dbus_interface='org.a11y.atspi.Accessible', timeout=seconds)
    except dbus.DBusException:
        return None
    loop, names = GLib.MainLoop(), {}
    pending = {str(bus) for bus, _ in children}
    def answer(bus, value):
        names[bus] = value
        pending.discard(bus)
        if not pending:
            loop.quit()
    try:
        for bus, path in children:
            bus = str(bus)
            connection.get_object(bus, str(path), introspect=False).Get(
                'org.a11y.atspi.Accessible', 'Name', dbus_interface='org.freedesktop.DBus.Properties', timeout=seconds,
                reply_handler=lambda value, bus=bus: answer(bus, str(value)),
                error_handler=lambda error, bus=bus: answer(bus, None if unanswered(error) else False))
        if pending:
            GLib.timeout_add(CALL_TIMEOUT_MS + 250, loop.quit)
            loop.run()
    finally:
        connection.close()
    names.update((bus, None) for bus in pending)
    return names


def accessible_id(node):
    try:
        return str(node.get_accessible_id() or '')
    except (AttributeError, NotImplementedError, RuntimeError) as error:
        if unanswered(error):
            raise  # a silent application makes the walk incomplete, not objectName-less (#887 N2)
        return ''


# Qt item interfaces (QAccessibleTableCell, header cells, QAccessibleTabButton)
# are not QObjects: their AccessibleId is the view's or tab bar's own path.
ITEM_ROLES = ('list item', 'table cell', 'tree item', 'page tab', 'column header', 'row header', 'table row')


def id_object_name(identifier, role):
    """objectName from Qt's AT-SPI AccessibleId (Qt 6.10 exports no objectName attribute, #819).

    Qt joins the object ancestry as 'QApplication.window.….objectName' and
    uses the class name for objects without one; view items carry their
    view's path. Neither names the row itself.
    """
    if not identifier or role in ITEM_ROLES:
        return ''
    last = identifier.rsplit('.', 1)[-1]
    return '' if '::' in last or re.match(r'Q[A-Z]', last) else last


def owned(pid):
    try:
        with open('/proc/' + str(pid) + '/environ', 'rb') as stream:
            return ('XDG_RUNTIME_DIR=' + os.environ['XDG_RUNTIME_DIR']).encode() in stream.read().split(b'\0')
    except (OSError, KeyError):
        return False


# at-spi2-core renamed ATSPI_ROLE_PUSH_BUTTON's name to "button" (Qt 6.10 on
# GNOME 50 reports it); older servers say "push button". Normalize for matching
# only; observations retain the server's spelling.
ROLE_SYNONYMS = {'button': 'push button'}


def normalize_role(role):
    role = role.lower().replace('_', ' ').replace('-', ' ')
    return ROLE_SYNONYMS.get(role, role)


def matches(row, query):
    for key, value in query.items():
        if key not in QUERY_KEYS:
            raise ValueError('unknown accessibility query key: ' + key)
        if key in ('compositor', 'coordinates'):
            continue  # placement inputs, not row filters
        if key in ('name', 'text', 'description', 'application', 'window'):
            if not fnmatch.fnmatchcase(row.get(key, ''), value):
                return False
        elif key == 'role':
            if normalize_role(row['role']) != normalize_role(value):
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


ROOT_PATH = '/org/a11y/atspi/accessible/root'
NULL_PATH = '/org/a11y/atspi/null'
REGISTRY = 'org.a11y.atspi.Registry'

# Reads go over raw D-Bus, never through libatspi (#814). libatspi sends
# org.a11y.atspi.Cache.GetItems to every application it first meets, and a
# probe meets the whole desktop: getDesktop(0) adds every registered app, an
# event from an app adds it too. Each fresh probe process therefore asked the
# shell to serialise its whole bridge cache. On Cinnamon (at-spi2-core 2.52)
# that cache can hold a freed AtkObject; GetItems refs every entry first and
# crashes the shell ("Cinnamon just crashed", impl_GetItems ->
# g_hash_table_foreach -> g_object_ref). Plain Accessible/Component/Text/
# Action calls never walk that cache.
_A11Y = {}


def a11y_bus():
    """This process's private connection to the accessibility bus."""
    import dbus
    connection = _A11Y.get('connection')
    if connection is None or not connection.get_is_connected():
        session = dbus.SessionBus()
        address = dbus.Interface(session.get_object('org.a11y.Bus', '/org/a11y/bus'), 'org.a11y.Bus').GetAddress(timeout=CALL_TIMEOUT_MS / 1000)
        connection = dbus.bus.BusConnection(str(address))
        _A11Y.clear()
        _A11Y.update(connection=connection, pids={})
    return connection


def bus_pid(connection, bus):
    """Owner PID of an a11y-bus name, from the bus daemon (never the app itself)."""
    import dbus
    pids = _A11Y.setdefault('pids', {})
    if bus not in pids:
        daemon = dbus.Interface(connection.get_object('org.freedesktop.DBus', '/org/freedesktop/DBus'), 'org.freedesktop.DBus')
        try:
            pids[bus] = int(daemon.GetConnectionUnixProcessID(bus, timeout=3))
        except dbus.DBusException as error:
            raise NotImplementedError(str(error)) from error
    return pids[bus]


def role_name(role, fallback):
    """libatspi's own name for a role number (as pyatspi reported it), else the server's."""
    try:
        import gi
        gi.require_version('Atspi', '2.0')
        from gi.repository import Atspi
        if 0 <= role < int(Atspi.Role.LAST_DEFINED) and role != int(Atspi.Role.EXTENDED):
            return Atspi.role_get_name(Atspi.Role(role))
    except (ImportError, ValueError, AttributeError):
        pass
    return fallback()


def popup_file():
    runtime = os.environ.get('XDG_RUNTIME_DIR')
    return runtime + '/putgd-lab-popups.json' if runtime else None


def popup_watch(query):
    """Long-running: record the private family's showing popup menus.

    Qt lists only non-Qt::Popup top-level windows as application children, so a
    context or row menu opened with QMenu::exec() is unreachable by a tree walk.
    Qt does emit object:state-changed:showing for it (ObjectShow) once a
    listener is registered. This process registers that listener with the
    registry itself and reads the signals off the a11y bus (no libatspi, so no
    GetItems on the shell, #814). It keeps {bus, path, pid} of owned showing
    popups in a private runtime file; every (fresh) accessible() walks those
    popups live over the a11y bus.
    """
    import dbus
    import dbus.mainloop.glib
    import signal
    from gi.repository import GLib
    path, current, focus = popup_file(), {}, {}
    def write():
        temporary = path + '.tmp'
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        try:
            os.write(descriptor, json.dumps(list(current.values()) + list(focus.values())).encode())
        finally:
            os.close(descriptor)
        os.replace(temporary, path)
    session = dbus.SessionBus()
    address = dbus.Interface(session.get_object('org.a11y.Bus', '/org/a11y/bus'), 'org.a11y.Bus').GetAddress(timeout=CALL_TIMEOUT_MS / 1000)
    connection = dbus.bus.BusConnection(str(address), mainloop=dbus.mainloop.glib.DBusGMainLoop())
    _A11Y.update(connection=connection, pids={})
    def changed(minor, detail1, detail2, value, properties, sender=None, path=None):
        try:
            if str(minor) not in ('showing', 'focused') or not sender or not path:
                return
            pid = bus_pid(connection, str(sender))
            shell = plasma_popup_sender(pid, str(minor), query)
            if not owned(pid) and not shell:
                return
            bus, node_path = str(sender), str(path)
            key = bus + node_path
            if str(minor) == 'focused':
                # Widgets parented to a view's viewport (History's row "More
                # actions" button) are outside Qt's accessible tree too; the
                # focused one is found here.
                if not detail1:
                    if key not in focus:
                        return
                    focus.clear()
                else:
                    focus.clear()
                    focus[key] = {'bus': bus, 'path': node_path, 'pid': pid, 'focus': True}
            elif normalize_role(RawNode(connection, bus, node_path, pid).getRoleName()) not in POPUP_ROLES:
                return
            elif detail1:
                current[key] = {'bus': bus, 'path': node_path, 'pid': pid, 'plasma': shell}
            elif current.pop(key, None) is None:
                return
            write()
        except Exception:
            pass
    write()
    connection.add_signal_receiver(changed, signal_name='StateChanged', dbus_interface='org.a11y.atspi.Event.Object',
                                   sender_keyword='sender', path_keyword='path')
    registry = dbus.Interface(connection.get_object(REGISTRY, '/org/a11y/atspi/registry'), 'org.a11y.atspi.Registry')
    for event in ('object:state-changed:showing', 'object:state-changed:focused'):
        registry.RegisterEvent(event, dbus.Array([], signature='s'), '', timeout=5)
    loop = GLib.MainLoop()
    signal.signal(signal.SIGTERM, lambda *_: loop.quit())
    loop.run()
    return {}


def plasma_popup_sender(pid, event, query):
    """Only showing popups from the live compositor shell; never foreign focus."""
    return (event == 'showing' and (query.get('compositor') or {}).get('kind') == 'kwin-script'
            and type(pid) is int and pid > 0 and pid == plasma_pid())


class RawNode:
    """A live accessible addressed by (bus name, path) on the a11y bus.

    Implements just the pyatspi node surface the probes use, so the desktop,
    applications and popups known only from the watcher's file are walked with
    the same code. Every property is a direct D-Bus call: nothing is cached
    across probes, and the Cache interface is never called (#814).
    """
    ACCESSIBLE = 'org.a11y.atspi.Accessible'

    def __init__(self, connection, bus, path, pid=None):
        self.connection, self.bus, self.path, self.pid = connection, str(bus), str(path), pid
        self._object = connection.get_object(self.bus, self.path, introspect=False)
        self._interfaces = None

    def __eq__(self, other):
        return isinstance(other, RawNode) and (self.bus, self.path) == (other.bus, other.path)

    def __hash__(self):
        return hash((self.bus, self.path))

    def _call(self, interface, method, *args):
        import dbus
        try:
            return self._object.get_dbus_method(method, interface)(*args, timeout=CALL_TIMEOUT_MS / 1000)
        except dbus.DBusException as error:
            raise NotImplementedError(str(error)) from error

    def _property(self, name, interface=ACCESSIBLE):
        return self._call('org.freedesktop.DBus.Properties', 'Get', interface, name)

    def _node(self, reference):
        bus, path = str(reference[0]), str(reference[1])
        if path == NULL_PATH:
            return None
        return RawNode(self.connection, bus, path, self.pid if bus == self.bus else None)

    @property
    def name(self):
        return str(self._property('Name'))

    def get_accessible_id(self):
        """Qt 6.10's objectName path (#819); missing on older servers."""
        return self._property('AccessibleId')

    @property
    def description(self):
        return str(self._property('Description'))

    @property
    def childCount(self):
        return int(self._property('ChildCount'))

    def getRoleName(self):
        return role_name(int(self._call(self.ACCESSIBLE, 'GetRole')), lambda: str(self._call(self.ACCESSIBLE, 'GetRoleName')))

    def get_process_id(self):
        if self.pid is None:
            self.pid = bus_pid(self.connection, self.bus)
        return self.pid

    def getAttributes(self):
        return [str(k) + ':' + str(v) for k, v in self._call(self.ACCESSIBLE, 'GetAttributes').items()]

    def getState(self):
        words = [int(word) for word in self._call(self.ACCESSIBLE, 'GetState')]
        class States:
            def contains(self, state):
                index = int(state)
                return bool(index // 32 < len(words) and words[index // 32] >> (index % 32) & 1)
        return States()

    def _require(self, interface):
        if self._interfaces is None:
            self._interfaces = {str(i) for i in self._call(self.ACCESSIBLE, 'GetInterfaces')}
        if interface not in self._interfaces:
            raise NotImplementedError(interface)

    def queryComponent(self):
        return self

    def getExtents(self, coordinates):
        import dbus
        return tuple(int(v) for v in self._call('org.a11y.atspi.Component', 'GetExtents', dbus.UInt32(int(coordinates))))

    def getAccessibleAtPoint(self, x, y, coordinates):
        import dbus
        return self._node(self._call('org.a11y.atspi.Component', 'GetAccessibleAtPoint',
                                     dbus.Int32(int(x)), dbus.Int32(int(y)), dbus.UInt32(int(coordinates))))

    def queryText(self):
        self._require('org.a11y.atspi.Text')
        node = self
        class Text:
            characterCount = int(node._property('CharacterCount', 'org.a11y.atspi.Text'))
            def getText(self, start, end):
                import dbus
                return str(node._call('org.a11y.atspi.Text', 'GetText', dbus.Int32(start), dbus.Int32(end)))
        return Text()

    def queryAction(self):
        self._require('org.a11y.atspi.Action')
        node = self
        class Action:
            nActions = int(node._property('NActions', 'org.a11y.atspi.Action'))
            def getKeyBinding(self, index):
                import dbus
                return str(node._call('org.a11y.atspi.Action', 'GetKeyBinding', dbus.Int32(index)))
        return Action()

    def __iter__(self):
        return iter([self._node(child) for child in self._call(self.ACCESSIBLE, 'GetChildren')])

    def parent(self):
        return self._node(self._property('Parent'))

    def window(self):
        """Name of the top-level accessible (child of the application root) above this node."""
        node = self
        for _ in range(40):
            parent = node.parent()
            if parent is None:
                return ''
            if parent.path.endswith('/accessible/root'):
                return node.name
            node = parent
        return ''


def desktop_root():
    """The registry's desktop; its children are the application roots."""
    return RawNode(a11y_bus(), REGISTRY, ROOT_PATH)


def popup_roots(query):
    """Extra walk roots for the watcher's showing popups, with the same app scoping."""
    path = popup_file()
    try:
        with open(path) as stream:
            entries = json.load(stream)
    except (OSError, TypeError, ValueError):
        return []
    if not entries:
        return []
    import pyatspi
    connection = a11y_bus()
    roots = []
    for entry in entries:
        pid = entry['pid']
        if (query.get('pid') is not None and pid != query['pid']) or (query.get('owned') and not owned(pid)):
            continue
        try:
            if entry.get('plasma') and (not plasma_popup_sender(pid, 'showing', query)
                                       or bus_pid(connection, entry['bus']) != pid):
                continue
            node = RawNode(connection, entry['bus'], entry['path'], pid)
            states = node.getState()
            if not states.contains(pyatspi.STATE_SHOWING) or (entry.get('focus') and not states.contains(pyatspi.STATE_FOCUSED)):
                continue
            name = RawNode(connection, entry['bus'], ROOT_PATH, pid).name
            window = node.window() if entry.get('focus') else ''
        except NotImplementedError:
            continue
        if query.get('application') and not fnmatch.fnmatchcase(name, query['application']):
            continue
        app = {'name': name, 'role': 'application', 'rect': None, 'pid': pid}
        # Depth 3 keeps the walk from treating a focused widget as a top-level window.
        roots.append(((pid, entry['path']), (node, 3 if entry.get('focus') else 2, window, name, None,
                      [app, {'name': window, 'role': 'frame', 'rect': None, 'pid': pid}] if entry.get('focus') else [app])))
    return roots


UNREAD = object()

# Mutter window types for popup surfaces (MetaWindowType: DROPDOWN_MENU,
# POPUP_MENU, TOOLTIP, COMBO); KWin reports popup/menu flags instead.
POPUP_WINDOW_TYPES = {8, 9, 10, 13}


def gnome_witness(method, *args):
    """One bounded call to the lab witness inside gnome-shell; a silent shell is named, not a raw D-Bus error."""
    import dbus
    try:
        witness = dbus.SessionBus().get_object('org.gnome.Shell', '/org/putgd/LabWitness')
        return getattr(witness, method)(*args, dbus_interface='org.putgd.LabWitness', timeout=3)
    except dbus.DBusException as error:
        if unanswered(error):
            raise RuntimeError('compositor witness (gnome-shell) did not answer %s within 3 s' % method) from None
        raise RuntimeError('compositor witness (gnome-shell) %s failed: %s' % (method, error.get_dbus_name())) from None


def gnome_selector(point=None):
    selector = json.loads(str(gnome_witness('Selector', json.dumps({'point': point}))))
    if (selector.get('name') != 'Capture region' or not selector.get('request_id') or
            not owned(selector.get('client_pid')) or not selector.get('rect') or min(selector['rect'][2:]) <= 0):
        return None
    return selector


def compositor_windows(compositor):
    """The compositor's own window list: frame/buffer rects, pid, focus, stacking."""
    kind = (compositor or {}).get('kind')
    if kind == 'gnome-lab-witness':
        return json.loads(str(gnome_witness('Windows')))
    if kind == 'kwin-script':
        return kwin_windows()
    if kind == 'wayland-toplevels':
        return wayland_toplevel_windows(compositor)
    if kind == 'hyprctl':
        return hyprland_windows()
    if kind == 'toplevel-pixels':
        return toplevel_pixel_windows(compositor)
    raise RuntimeError('no compositor window witness configured for this Wayland profile')


def wayland_toplevel_windows(compositor=None):
    """labwc and Mir have no scripting: the toplevel protocols are the witness.

    They give identifiers, app IDs, titles and the activated window, but no PID
    and no geometry, so frame and content are None and hit-tests are refused.
    The probe runs from shipped source with no file of its own, so the context
    sends the witness's source with the compositor settings (witness_source).
    """
    source = (compositor or {}).get('witness_source')
    if source:
        import types
        wayland_toplevels = types.ModuleType('wayland_toplevels')
        exec(compile(source, 'wayland_toplevels.py', 'exec'), wayland_toplevels.__dict__)
    else:
        import sys
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
        import wayland_toplevels
    seen = wayland_toplevels.snapshot(wayland_toplevels.socket_path())
    active = (seen.get('active') or {}).get('identifier')
    windows = [{'id': w.get('identifier'), 'title': w.get('title'), 'pid': None, 'wm_class': w.get('app_id'),
                'frame': None, 'content': None, 'focus': w.get('identifier') == active, 'stack': i}
               for i, w in enumerate(seen.get('ext', []))]
    return {'windows': windows, 'protocols': seen.get('protocols', {})}


def compositor_activate(compositor, window_id):
    """Explicit raise for focus(raise_window=True) on Wayland: the compositor's own activation."""
    kind = (compositor or {}).get('kind')
    if kind == 'gnome-lab-witness':
        import dbus
        return bool(gnome_witness('Activate', dbus.UInt64(window_id)))
    if kind == 'hyprctl':
        return hyprland_activate(window_id)
    if kind == 'toplevel-pixels':
        return toplevel_pixels_activate(compositor, window_id)
    raise RuntimeError('this compositor witness cannot raise windows')


def resize_witness(query):
    """Read Mutter grab signals; geometry changes alone never prove resize start."""
    compositor = query.get('compositor') or {}
    if compositor.get('kind') != 'gnome-lab-witness':
        raise RuntimeError('resize-start witness unavailable for this compositor')
    import dbus
    witness = dbus.SessionBus().get_object('org.gnome.Shell', '/org/putgd/LabWitness')
    return json.loads(str(witness.Grabs(dbus_interface='org.putgd.LabWitness', timeout=3)))


def resize_started(before, after, target):
    """Bind a new begin event to this extension instance and this owned client."""
    if not before.get('epoch') or after.get('epoch') != before['epoch']:
        raise RuntimeError('resize-start witness restarted during the gesture')
    if after['serial'] < before['serial'] or (after['events'] and after['events'][0]['serial'] > before['serial'] + 1):
        raise RuntimeError('resize-start witness lost events during the gesture')
    return next((event for event in after['events']
                 if before['serial'] < event['serial'] <= after['serial'] and event['phase'] == 'begin'
                 and event['window_id'] == target['window_id'] and event['pid'] == target['pid']
                 and event['kind'] == 'resize' and event['operation'].startswith('RESIZING_')), None)


def compositor_pick(compositor, point):
    """The compositor's hit-test at a desktop point: {window, chain}."""
    kind = (compositor or {}).get('kind')
    if kind == 'gnome-lab-witness':
        return json.loads(str(gnome_witness('Pick', int(point[0]), int(point[1]))))
    if kind == 'kwin-script':
        return kwin_pick(point)
    if kind == 'hyprctl':
        return hyprland_pick(point)
    raise RuntimeError('no compositor hit-test witness configured for this Wayland profile')


_KWIN = None
KWIN_SCRIPT = r"""
const rect = g => [Math.round(g.x), Math.round(g.y), Math.round(g.width), Math.round(g.height)];
const list = workspace.stackingOrder;
const windows = [];
for (let i = 0; i < list.length; i++) {
    const w = list[i];
    windows.push({id: w.internalId.toString(), title: w.caption, pid: w.pid, wm_class: w.resourceClass,
                  frame: rect(w.frameGeometry), content: rect(w.clientGeometry), focus: w.active,
                  minimized: w.minimized, hidden: !!w.hidden, popup: !!w.popupWindow, dock: !!w.dock,
                  desktop: !!w.desktopWindow, normal: !!w.normalWindow, notification: !!(w.notification || w.criticalNotification),
                  surface_class: w.toString().split("(")[0],
                  stack: i, transient_for: w.transientFor ? w.transientFor.internalId.toString() : null});
}
callDBus(RECEIVER, RECEIVER_PATH, "org.putgd.LabWitness", "Report", JSON.stringify({windows: windows}));
"""


def kwin_windows():
    """KWin's own stacking order through a one-shot read-only KWin script.

    The script reports back over D-Bus to a private receiver name owned by
    this probe process; it is unloaded afterwards. clientGeometry is KWin's
    exact content rect, so no decoration split is guessed.
    """
    import tempfile
    import dbus
    import dbus.bus
    import dbus.service
    import dbus.mainloop.glib
    from gi.repository import GLib
    global _KWIN
    if _KWIN is None:
        # One private connection per process, owning a private receiver name.
        bus = dbus.bus.BusConnection(dbus.bus.BUS_SESSION, mainloop=dbus.mainloop.glib.DBusGMainLoop())
        receiver = 'org.putgd.LabWitness.p%d' % os.getpid()
        _KWIN = {'bus': bus, 'receiver': receiver, 'name': dbus.service.BusName(receiver, bus), 'calls': 0}
    bus, receiver = _KWIN['bus'], _KWIN['receiver']
    _KWIN['calls'] += 1
    object_path = '/r%d' % _KWIN['calls']
    loop, reply = GLib.MainLoop(), {}
    class Receiver(dbus.service.Object):
        @dbus.service.method('org.putgd.LabWitness', in_signature='s')
        def Report(self, text):
            reply['text'] = str(text)
            loop.quit()
    handler = Receiver(bus, object_path)
    plugin = 'putgd-lab-witness-%d' % os.getpid()
    with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False) as stream:
        stream.write('const RECEIVER = ' + json.dumps(receiver) + ';\nconst RECEIVER_PATH = ' + json.dumps(object_path) + ';\n' + KWIN_SCRIPT)
        path = stream.name
    scripting = dbus.Interface(bus.get_object('org.kde.KWin', '/Scripting'), 'org.kde.kwin.Scripting')
    try:
        try:
            identifier = int(scripting.loadScript(path, plugin, signature='ss'))
        except dbus.DBusException:
            identifier = int(scripting.loadScript(path, signature='s'))  # older one-argument form
        script = bus.get_object('org.kde.KWin', '/Scripting/Script%d' % identifier)
        dbus.Interface(script, 'org.kde.kwin.Script').run()
        GLib.timeout_add(3000, loop.quit)
        loop.run()
    finally:
        try:
            scripting.unloadScript(plugin)
        except dbus.DBusException:
            pass
        os.unlink(path)
        handler.remove_from_connection()
    if 'text' not in reply:
        raise RuntimeError('KWin witness script did not report within 3 s')
    document = json.loads(reply['text'])
    for window in document['windows']:
        window['minimized'] = window['minimized'] or window['hidden']
        window['type'] = 9 if window['popup'] else 0
    return document


def kwin_pick(point):
    """Topmost visible KWin window containing the point (panels and notifications included)."""
    x, y = point
    for window in reversed(kwin_windows()['windows']):
        if window['minimized'] or window['desktop']:
            continue
        fx, fy, fw, fh = window['frame']
        if fx <= x < fx + fw and fy <= y < fy + fh:
            return {'window': window, 'chain': []}
    return {'window': None, 'chain': [{'name': 'desktop'}]}


# Hyprland: the compositor's own IPC. j/clients gives every window's address,
# pid, position, size, workspace and focus recency; j/layers the layer-shell
# surfaces (panels, notifications, put.gd's selector and recording overlays)
# per output and level; j/monitors the shown workspaces. Spoken on the request
# socket directly, as put.gd does, so no hyprctl binary or library path is needed.
HYPR_LEVELS = {'0': 'background', '1': 'bottom', '2': 'top', '3': 'overlay'}


def hyprland_socket():
    """The session's request socket under the real runtime directory, not the run's private one."""
    import glob
    runtime = os.environ.get('PIPEWIRE_RUNTIME_DIR') or '/run/user/%d' % os.getuid()
    signature = os.environ.get('HYPRLAND_INSTANCE_SIGNATURE')
    paths = ([os.path.join(runtime, 'hypr', signature, '.socket.sock')] if signature
             else sorted(glob.glob(os.path.join(runtime, 'hypr', '*', '.socket.sock'))))
    paths = [path for path in paths if os.path.exists(path)]
    if len(paths) != 1:
        raise RuntimeError('Hyprland request socket is missing or ambiguous (%d found)' % len(paths))
    return paths[0]


def hyprland_request(command, timeout=3.0):
    """One request, one reply; Hyprland refuses connections briefly after a reload."""
    import socket as socket_
    deadline = time.monotonic() + timeout
    path = hyprland_socket()
    while True:
        connection = socket_.socket(socket_.AF_UNIX, socket_.SOCK_STREAM)
        connection.settimeout(max(.1, deadline - time.monotonic()))
        try:
            connection.connect(path)
            connection.sendall(command.encode())
            chunks = []
            while True:
                chunk = connection.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
            return b''.join(chunks).decode('utf-8', 'replace')
        except (ConnectionRefusedError, FileNotFoundError):
            if time.monotonic() >= deadline:
                raise RuntimeError('Hyprland did not accept a request within %g s' % timeout) from None
            time.sleep(.1)
        except OSError as error:
            raise RuntimeError('Hyprland request %s failed: %s' % (command.split()[0], type(error).__name__)) from None
        finally:
            connection.close()


def hyprland_json(command):
    reply = hyprland_request('j/' + command)
    try:
        return json.loads(reply)
    except ValueError:
        raise RuntimeError('Hyprland answered %s without JSON' % command) from None


def hyprland_parse(clients, monitors, layers, active):
    """Windows and layer surfaces, in the witness's common shape, bottom to top.

    Hyprland does not report a z-order. Its drawing order is fixed by kind:
    background and bottom layers, tiled windows, floating windows (the one
    focused last on top, as Hyprland raises a floating window it focuses),
    fullscreen windows, the top layer, the overlay layer. A window on a
    workspace no output shows is reported minimized, so it never answers a pick.
    """
    shown = set()
    for monitor in monitors:
        for key in ('activeWorkspace', 'specialWorkspace'):
            workspace = (monitor.get(key) or {}).get('id')
            if workspace:
                shown.add(workspace)
    focused = (active or {}).get('address')
    rows = []
    for client in clients:
        if not client.get('mapped', True):
            continue
        rect = [int(v) for v in list(client['at']) + list(client['size'])]
        fullscreen = int(client.get('fullscreen') or 0) >= 2
        band = 3 if fullscreen else 2 if client.get('floating') else 1
        recency = client.get('focusHistoryID')
        rows.append(((band, -(recency if isinstance(recency, int) and recency >= 0 else 1 << 20)), {
            'id': str(client['address']), 'title': client.get('title') or '', 'pid': _real_pid(client.get('pid')),
            'wm_class': client.get('class') or '', 'frame': rect, 'content': rect,
            'focus': client['address'] == focused,
            'minimized': bool(client.get('hidden')) or (client.get('workspace') or {}).get('id') not in shown,
            'floating': bool(client.get('floating')), 'fullscreen': fullscreen, 'layer': None,
            'workspace': (client.get('workspace') or {}).get('id'), 'monitor': client.get('monitor'),
            'stable_id': client.get('stableId'), 'type': 0, 'popup': False,
            'transient_for': None, 'desktop': False, 'notification': False}))
    for monitor, value in (layers or {}).items():
        for level, surfaces in (value.get('levels') or {}).items():
            name = HYPR_LEVELS.get(str(level))
            if name is None:
                continue
            band = {'background': -2, 'bottom': -1, 'top': 4, 'overlay': 5}[name]
            for index, surface in enumerate(surfaces):
                rect = [int(surface[k]) for k in ('x', 'y', 'w', 'h')]
                rows.append(((band, index), {
                    'id': str(surface['address']), 'title': '', 'pid': _real_pid(surface.get('pid')),
                    'wm_class': surface.get('namespace') or '', 'frame': rect, 'content': rect,
                    'focus': False, 'minimized': False, 'floating': False, 'fullscreen': False,
                    'layer': name, 'monitor': monitor, 'type': 0, 'popup': False, 'transient_for': None,
                    'desktop': name in ('background', 'bottom'), 'notification': False}))
    rows.sort(key=lambda row: row[0])
    windows = []
    for stack, (_, row) in enumerate(rows):
        row['stack'] = stack
        windows.append(row)
    return {'windows': windows}


def _real_pid(pid):
    """Hyprland reports -1 for a client whose process it could not learn; that is no process."""
    return pid if isinstance(pid, int) and not isinstance(pid, bool) and pid > 1 else None


def hyprland_windows():
    return hyprland_parse(hyprland_json('clients'), hyprland_json('monitors'),
                          hyprland_json('layers'), hyprland_json('activewindow'))


def hyprland_pick(point, document=None):
    """Topmost window or layer surface at the point, by Hyprland's drawing order."""
    x, y = point
    document = document or hyprland_windows()
    for window in reversed(document['windows']):
        if window['minimized']:
            continue
        fx, fy, fw, fh = window['frame']
        if fx <= x < fx + fw and fy <= y < fy + fh:
            if window['desktop']:
                break
            return {'window': window, 'chain': []}
    return {'window': None, 'chain': [{'name': 'desktop'}]}


def hyprland_foreground(target, point, attempts=3):
    """Pointer input on Hyprland: the topmost surface at the point must be the target.

    Omarchy's bar and notification layer surfaces span the output and are mostly
    transparent, so one of the shell's layer surfaces over the target is decided by
    pixels: the target's own capture (ext identifier = Hyprland's stableId) must be
    what the outputs show at the point.
    """
    wid = target.get('window_id')
    document = hyprland_windows()
    pick = hyprland_pick(point, document)
    window = pick['window']
    result = {'backend': 'hyprctl-pick', 'target': wid, 'obscuring_window_id': window['id'] if window else None}
    if window and window['id'] == wid:
        return dict(result, ok=True, obscuring_window=None)
    mine = next((w for w in document['windows'] if w['id'] == wid), None)
    shell = window and window.get('layer') in ('top', 'overlay') and not owned(window.get('pid'))
    if not (shell and mine and not mine.get('layer') and mine.get('stable_id')):
        name = (window['title'] or window['wm_class'] or 'untitled window') if window else 'the desktop'
        return dict(result, ok=False, obscuring_window=name)
    for attempt in range(attempts):
        if attempt:
            time.sleep(.2)
        own, screen = grim(str(mine['stable_id'])), grim()
        if pixels_at(own, screen, mine['content'], point, cursor=True):
            return dict(result, ok=True, obscuring_window=None, backend='hyprctl-pick-pixels')
    # A see-through window (Omarchy draws Nautilus over the wallpaper) never shows its own
    # pixels anywhere; a cover changes them only where it lies. For such a window the shell
    # layer covers the point only inside the output's reserved strip (Omarchy's bar) or
    # while quickshell holds notification cards.
    if window_share(own, screen, mine['content']) < .2:
        reserved = hyprland_reserved(point)
        cards = quickshell_cards()
        if not reserved and cards == 0:
            return dict(result, ok=True, obscuring_window=None, backend='hyprctl-pick-translucent')
        return dict(result, ok=False, obscuring_window=window['wm_class'] + (
            ' (the bar\'s reserved strip)' if reserved else ' (%s notification cards)' % cards))
    return dict(result, ok=False, obscuring_window=window['wm_class'] + ' (shell layer, drawn over the window)')


def window_share(window, screen, rect):
    """Share of a window's sampled cells the outputs show exactly at its known place."""
    ww, wh, wb = window
    sw, sh, sb = screen
    total = equal = 0
    for r in range(0, wh, max(1, wh // 32)):
        for c in range(0, ww, 64):
            n, sx, sy = min(64, ww - c), rect[0] + c, rect[1] + r
            if not (0 <= sy < sh and 0 <= sx and sx + n <= sw):
                continue
            total += 1
            equal += sb[(sy * sw + sx) * 3:(sy * sw + sx + n) * 3] == wb[(r * ww + c) * 3:(r * ww + c + n) * 3]
    return equal / total if total else 0.0


def hyprland_reserved(point):
    """True when the point lies in an output's reserved strip (panels' exclusive zones)."""
    x, y = point
    for monitor in hyprland_json('monitors'):
        scale = float(monitor.get('scale') or 1)
        mx, my = monitor['x'], monitor['y']
        mw, mh = monitor['width'] / scale, monitor['height'] / scale
        if not (mx <= x < mx + mw and my <= y < my + mh):
            continue
        left, top, right, bottom = (list(monitor.get('reserved') or []) + [0, 0, 0, 0])[:4]
        return x < mx + left or y < my + top or x >= mx + mw - right or y >= my + mh - bottom
    return True


def quickshell_cards():
    """quickshell's own count of notification cards, or None when it cannot be asked."""
    try:
        owner = run('gdbus', 'call', '--session', '--dest', 'org.freedesktop.DBus', '--object-path', '/org/freedesktop/DBus',
                    '--method', 'org.freedesktop.DBus.GetConnectionUnixProcessID', 'org.freedesktop.Notifications')
        pid = int(re.search(r'uint32 (\d+)', owner)[1])
        if process_name(pid) != 'quickshell':
            return None
        runtime = os.environ.get('PIPEWIRE_RUNTIME_DIR') or os.environ.get('XDG_RUNTIME_DIR', '')
        reply = subprocess.run(['qs', 'ipc', '--pid', str(pid), 'call', 'notifications', 'count'], capture_output=True,
                               text=True, timeout=3, env=dict(os.environ, XDG_RUNTIME_DIR=runtime, LANG='C.UTF-8'))
        return int(reply.stdout.strip()) if reply.returncode == 0 else None
    except (OSError, subprocess.SubprocessError, TypeError, ValueError):
        return None


def hyprland_activate(window_id):
    """Hyprland's own focus dispatcher for one window address."""
    if not re.fullmatch(r'0x[0-9a-fA-F]+', str(window_id)):
        raise RuntimeError('Hyprland window address is malformed')
    # Hyprland 0.56 with a Lua configuration takes the dispatcher as Lua; older ones the legacy form.
    reply = hyprland_request('dispatch hl.dsp.focus({ window = "address:%s" })' % window_id).strip()
    if reply != 'ok':
        reply = hyprland_request('dispatch focuswindow address:' + str(window_id)).strip()
    return reply == 'ok'


# labwc has no window geometry anywhere: no IPC, and neither toplevel list
# carries a position. A window is placed only where its own pixels are: grim -T
# reads the window's content through ext-foreign-toplevel-image-capture (what
# put.gd's window capture uses), grim reads the composed outputs, and the
# window must be found there exactly. An occluded, moving or repetitive window
# is refused rather than guessed. A hit-test asks the same question at the
# point: are the window's own pixels what the outputs show there?
PIXEL_CHUNK = 32
PIXEL_HITS = 32


def parse_ppm(data):
    match = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', data)
    if not match:
        raise RuntimeError('capture is not a binary PPM')
    width, height = int(match[1]), int(match[2])
    body = data[match.end():match.end() + width * height * 3]
    if len(body) != width * height * 3:
        raise RuntimeError('capture is truncated')
    return width, height, body


def grim(identifier=None):
    """The composed outputs (no cursor), or one toplevel's own content by its ext identifier."""
    args = ['grim', '-t', 'ppm'] + (['-T', identifier] if identifier else []) + ['-']
    try:
        reply = subprocess.run(args, capture_output=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError('grim capture unavailable: ' + type(error).__name__) from None
    if reply.returncode:
        raise RuntimeError('grim could not capture ' + ('the toplevel' if identifier else 'the outputs'))
    return parse_ppm(reply.stdout)


def _detail(data):
    import zlib
    return len(zlib.compress(data, 1))


def locate_pixels(window, screen):
    """Where the window's pixels are in the outputs: {'rect', 'visible'} or RuntimeError.

    Detailed runs of the window's pixels vote for an offset; a run that occurs
    more than PIXEL_HITS times on the outputs is no evidence and is skipped. The
    winning offset must reproduce at least half of the window's pixel cells
    exactly, and no other offset may.
    """
    ww, wh, wb = window
    sw, sh, sb = screen
    if not (0 < ww <= sw and 0 < wh <= sh):
        raise RuntimeError('window capture %dx%d does not fit the %dx%d outputs' % (ww, wh, sw, sh))
    wstride, sstride, chunk = ww * 3, sw * 3, min(PIXEL_CHUNK, max(8, ww // 8), ww)
    step = max(1, wh // 40)
    lines = sorted(range(0, wh, step), key=lambda r: -_detail(wb[r * wstride:(r + 1) * wstride]))[:16]
    votes = {}
    starts = list(range(0, ww - chunk + 1, chunk))
    # The most detailed run in each quarter of a row, so a cover on one side still leaves evidence.
    quarters = [starts[i * len(starts) // 4:(i + 1) * len(starts) // 4] or starts for i in range(4)]
    needles = []
    for r in lines:
        line = wb[r * wstride:(r + 1) * wstride]
        for quarter in quarters:
            best = max(quarter, key=lambda c: _detail(line[c * 3:(c + chunk) * 3]))
            if (r, best) not in [(n[0], n[1]) for n in needles]:
                needles.append((r, best, line[best * 3:(best + chunk) * 3]))
    for r, best, needle in needles:
        if _detail(needle) < len(needle) // 4:
            continue  # flat: would match anywhere
        found = []
        for y in range(sh):
            start = y * sstride
            at = sb.find(needle, start, start + sstride)
            while at >= 0 and len(found) <= PIXEL_HITS:
                if (at - start) % 3 == 0:
                    found.append(((at - start) // 3 - best, y - r))
                at = sb.find(needle, at + 1, start + sstride)
            if len(found) > PIXEL_HITS:
                break
        if len(found) > PIXEL_HITS:
            continue
        for key in set(found):
            votes[key] = votes.get(key, 0) + 1
    if not votes:
        raise RuntimeError('the window has no distinctive pixels on the outputs (occluded, moving or flat)')

    # Only cells with detail count: a flat cell agrees at any nearby offset.
    cells = [(r, c, min(64, ww - c)) for r in range(0, wh, max(1, wh // 64)) for c in range(0, ww, 64)]
    cells = [(r, c, n) for r, c, n in cells
             if _detail(wb[(r * ww + c) * 3:(r * ww + c + n) * 3]) >= n * 3 // 4] or cells

    def coverage(x, y):
        total = equal = 0
        for r, c, n in cells:
            sy, sx = y + r, x + c
            if not 0 <= sy < sh or sx < 0 or sx + n > sw:
                continue
            total += 1
            at = (r * ww + c) * 3
            equal += sb[(sy * sw + sx) * 3:(sy * sw + sx + n) * 3] == wb[at:at + n * 3]
        return equal / total if total else 0.0
    ranked = sorted(votes.items(), key=lambda item: -item[1])[:6]
    verified = [(offset, coverage(*offset)) for offset, count in ranked if count >= min(2, len(needles))]
    # Flat areas also agree at a nearby wrong offset; only a second offset nearly as good is ambiguity.
    best = max((share for _, share in verified), default=0.0)
    accepted = [(offset, share) for offset, share in verified if share >= max(.5, best - .2)]
    if len(accepted) != 1:
        raise RuntimeError('the window\'s pixels are %s on the outputs (%s)' % (
            'not found' if not accepted else 'ambiguous',
            ', '.join('%s:%.2f' % (offset, share) for offset, share in verified) or 'no offset verified'))
    (x, y), share = accepted[0]
    return {'rect': [x, y, ww, wh], 'visible': round(share, 3)}


def pixels_at(window, screen, rect, point, radius=6, cursor=False):
    """True when the outputs show the window's own pixels at and around the point.

    Without a cursor in the capture (grim's default), the point itself and the four
    boxes that meet at it must all be the window's own pixels. With CURSOR (Hyprland
    draws its software cursor into every output copy), the arrow whose hotspot is at
    the point covers the point and the box below and right of it, so the other three
    boxes must match; a cover reaching only that corner is then not seen.
    """
    ww, wh, wb = window
    sw, sh, sb = screen
    px, py = int(point[0]) - rect[0], int(point[1]) - rect[1]

    def same(left, right, top, bottom):
        left, right, top, bottom = max(0, left), min(ww, right), max(0, top), min(wh, bottom)
        if left >= right or top >= bottom:
            return False
        for r in range(top, bottom):
            sx, sy = rect[0] + left, rect[1] + r
            if not (0 <= sy < sh and 0 <= sx and sx + right - left <= sw):
                return False
            if sb[(sy * sw + sx) * 3:(sy * sw + sx + right - left) * 3] != wb[(r * ww + left) * 3:(r * ww + right) * 3]:
                return False
        return True
    if not (0 <= px < ww and 0 <= py < wh):
        return False
    gap = 2  # the arrow's outline reaches a pixel or so past its hotspot
    boxes = ((px - gap - radius, px - gap, py - gap - radius, py - gap), (px + gap, px + gap + radius, py - gap - radius, py - gap),
             (px - gap - radius, px - gap, py + gap, py + gap + radius), (px + gap, px + gap + radius, py + gap, py + gap + radius))
    if cursor:
        return all(same(*box) for box in boxes[:3])
    return same(px, px + 1, py, py + 1) and all(same(*box) for box in boxes)


def content_box(image, width, height):
    """The width x height box of a capture outside of which every pixel is black, or None."""
    w, h, data = image
    stride = w * 3
    rows = [r for r in range(h) if data[r * stride:(r + 1) * stride].strip(b'\0')]
    if not rows:
        return None
    left = min((len(data[r * stride:(r + 1) * stride]) - len(data[r * stride:(r + 1) * stride].lstrip(b'\0'))) // 3
               for r in rows)
    right = max(w - (len(data[r * stride:(r + 1) * stride]) - len(data[r * stride:(r + 1) * stride].rstrip(b'\0'))) // 3
                for r in rows)
    top, bottom = rows[0], rows[-1] + 1
    if (right - left, bottom - top) != (width, height):
        return None  # a black content edge or a coloured shadow: no exact box, no guess
    box = b''.join(data[(top + r) * stride + left * 3:(top + r) * stride + (left + width) * 3] for r in range(height))
    return width, height, box


def crop(image, x, y, width, height):
    """The width x height box of a capture at (x, y)."""
    iw, _, data = image
    return width, height, b''.join(data[((y + r) * iw + x) * 3:((y + r) * iw + x + width) * 3] for r in range(height))


# A window shown where the desktop is known to put it must match there almost everywhere.
KNOWN_SHARE = .9


def pixel_share(window, screen, x, y):
    """The share of the window's cells the outputs show exactly, with the window at (x, y).

    Every cell counts, flat ones too: at a position known in advance there is no
    other offset for a flat cell to agree with. Cells off the outputs don't count.
    """
    ww, wh, wb = window
    sw, sh, sb = screen
    total = equal = 0
    for r in range(0, wh, max(1, wh // 64)):
        for c in range(0, ww, 64):
            n = min(64, ww - c)
            sy, sx = y + r, x + c
            if not 0 <= sy < sh or sx < 0 or sx + n > sw:
                continue
            total += 1
            at = (r * ww + c) * 3
            equal += sb[(sy * sw + sx) * 3:(sy * sw + sx + n) * 3] == wb[at:at + n * 3]
    return equal / total if total else 0.0


def rule_rect(compositor, title, app_id, size):
    """The rectangle the lab guest's window rule puts this window at (labwc), or None.

    The nightly guest writes these rules into labwc's rc.xml from the same profile entries
    (nightly_desktop_guest.prepare_labwc_rules): no server-side decoration, so the rule's
    position is the content's.
    """
    rules = [r for r in (compositor or {}).get('window_rules', ())
             if r.get('title') == title and r.get('app_id') == app_id and r.get('position')]
    if len(rules) > 1:
        raise RuntimeError('more than one window rule places %r' % title)
    return [int(rules[0]['position'][0]), int(rules[0]['position'][1])] + list(size) if rules else None


def place_pixels(compositor, title, app_id, window, screen, rule_only=False):
    """Where a toplevel's pixels are on the outputs: {'rect', 'visible', 'by'} or RuntimeError.

    A window a lab window rule places must show at the rule's position. Any other is found by its
    own pixels; when it has none to find (a blank fullscreen kiosk), a window exactly the size of
    one output must show on that output.
    """
    size = list(window[:2])
    known = rule_rect(compositor, title, app_id, size)
    if known:
        share = pixel_share(window, screen, known[0], known[1])
        if share < KNOWN_SHARE:
            raise RuntimeError('%r is not at its window-rule position %s (%.2f of its pixels there)'
                               % (title, known[:2], share))
        return {'rect': known, 'visible': round(share, 3), 'by': 'window rule'}
    if rule_only:
        raise RuntimeError('no lab window rule places %r (%s)' % (title, app_id))
    try:
        return dict(locate_pixels(window, screen), by='pixels')
    except RuntimeError as error:
        if 'no distinctive pixels' not in str(error):
            raise
        fits = [o for o in wlr_outputs() if o[2:] == size]
        shown = [o for o in fits if pixel_share(window, screen, o[0], o[1]) >= KNOWN_SHARE]
        if len(shown) != 1:
            raise RuntimeError(str(error) + ('; nor does it fill one output (%d of %d outputs its size show it)'
                                             % (len(shown), len(fits)))) from None
        return {'rect': shown[0], 'visible': round(pixel_share(window, screen, *shown[0][:2]), 3), 'by': 'output'}


def dialog_place(compositor, pid, title, window, size, screen):
    """(parent id, the dialog's own pixels, its rect) for a dialog labwc captures as 1x1.

    labwc draws a toplevel's dialogs into that toplevel's own capture at its origin, so the
    dialog's pixels are the top-left corner of one other toplevel of the same process. Only a
    dialog a lab window rule places is accepted: its pixels must show at the rule's position,
    from exactly one such corner.
    """
    placed, refused = [], []
    for other in toplevel_pixel_windows(compositor)['windows']:
        if other['id'] == window['id'] or other.get('minimized') or not app_id_names(other['wm_class'], pid):
            continue
        image = grim(other['id'])
        if image[:2] == (1, 1) or image[0] < size[0] or image[1] < size[1]:
            continue
        corner = crop(image, 0, 0, *size)
        try:
            placed.append((other['id'], corner, place_pixels(compositor, title, window['wm_class'], corner, screen,
                                                             rule_only=True)['rect']))
        except RuntimeError as error:
            refused.append(str(error))
    if len(placed) != 1:
        raise RuntimeError('labwc captured dialog %r as 1x1 and %d of its process\'s toplevels show it at its '
                           'window-rule position%s' % (title, len(placed), (': ' + '; '.join(refused[:2])) if refused else ''))
    return placed[0]


def app_id_names(app_id, pid):
    """Wayland gives a toplevel no PID: its app ID must name the accessible's process."""
    if not app_id:
        return False
    tail = app_id.rsplit('.', 1)[-1].lower()
    if any(name == app_id or name.lower() == tail for name in executable(pid)):
        return True
    # Firefox takes its Wayland app ID from MOZ_APP_REMOTINGNAME in its own environment
    # (the lab's private kiosk sets one per profile).
    try:
        with open('/proc/%d/environ' % pid, 'rb') as stream:
            return ('MOZ_APP_REMOTINGNAME=' + app_id).encode() in stream.read().split(b'\0')
    except (OSError, TypeError):
        return False


def toplevels_module(compositor):
    source = (compositor or {}).get('witness_source')
    if source:
        import types
        module = types.ModuleType('wayland_toplevels')
        exec(compile(source, 'wayland_toplevels.py', 'exec'), module.__dict__)
        return module
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    import wayland_toplevels
    return wayland_toplevels


def toplevel_pixel_windows(compositor):
    """The toplevel lists' identities, titles, app IDs and active window; no geometry and no PID."""
    module = toplevels_module(compositor)
    seen = module.snapshot(module.socket_path())
    active = (seen.get('active') or {}).get('identifier')
    minimized = {(w.get('app_id'), w.get('title')) for w in seen.get('wlr', []) if w.get('minimized')}
    windows = [{'id': w.get('identifier'), 'title': w.get('title') or '', 'pid': None, 'wm_class': w.get('app_id') or '',
                'frame': None, 'content': None, 'focus': w.get('identifier') == active, 'stack': None,
                'minimized': (w.get('app_id'), w.get('title')) in minimized, 'type': 0, 'popup': False}
               for w in seen.get('ext', [])]
    return {'windows': windows, 'protocols': seen.get('protocols', {})}


def toplevel_pixels_activate(compositor, window_id):
    module = toplevels_module(compositor)
    return module.activate(module.socket_path(), window_id)


def wlr_outputs():
    """Each enabled output's logical rectangle from wlr-output-management (wlr-randr --json)."""
    try:
        reply = subprocess.run(['wlr-randr', '--json'], capture_output=True, text=True, timeout=5)
        outputs = json.loads(reply.stdout) if reply.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired, ValueError):
        outputs = None
    if not isinstance(outputs, list):
        raise RuntimeError('wlr-randr could not list the outputs')
    return wlr_output_rects(outputs)


def wlr_output_rects(outputs):
    rects = []
    for output in outputs:
        if not output.get('enabled'):
            continue
        mode = next((m for m in output.get('modes', []) if m.get('current')), None)
        if not mode:
            continue
        scale = float(output.get('scale') or 1)
        width, height = mode['width'], mode['height']
        if str(output.get('transform', 'normal')) in ('90', '270', 'flipped-90', 'flipped-270'):
            width, height = height, width
        rects.append([int(output['position']['x']), int(output['position']['y']),
                      round(width / scale), round(height / scale)])
    return rects


def shell_overlays_showing(applications):
    """Showing top-level windows of the shell's own layer-shell clients (notification popups)."""
    found = []
    for application in applications or ():
        for row in accessible({'application': application, 'showing': True, 'visible': True}, toplevel=True, detail=False):
            if row['role'] in WINDOW_ROLES and row['states'].get('showing'):
                found.append(application + (' ' + repr(row['name']) if row['name'] else ''))
    return found


def display_screen(display):
    """The outputs as the driver read them from the guest's display over the target window (else black)."""
    import base64
    import zlib
    width, height = display['size']
    x, y, w, h = display['rect']
    crop = zlib.decompress(base64.b64decode(display['rgb']))
    if len(crop) != w * h * 3 or not (0 <= x and 0 <= y and x + w <= width and y + h <= height):
        raise RuntimeError('display snapshot does not match its rectangle')
    data = bytearray(width * height * 3)
    for r in range(h):
        data[((y + r) * width + x) * 3:((y + r) * width + x + w) * 3] = crop[r * w * 3:(r + 1) * w * 3]
    return width, height, bytes(data)


def toplevel_pixels_foreground(compositor, target, point, attempts=3, display=None):
    """Pointer input on labwc: the target's own pixels must be what the outputs show at the point.

    With DISPLAY (the driver's read of the guest display over the target), that is the outputs;
    otherwise grim's capture of them.
    """
    wid = target['window_id']
    if str(wid).startswith('layer:'):
        # put.gd's own full-output overlay selector: nothing but another overlay can cover it.
        rect = [target.get(k) for k in ('x', 'y', 'width', 'height')]
        if point is not None and not (rect[0] <= point[0] < rect[0] + rect[2] and rect[1] <= point[1] < rect[1] + rect[3]):
            return {'ok': False, 'backend': 'toplevel-pixels-layer', 'target': wid,
                    'obscuring_window': 'point outside the selector', 'obscuring_window_id': None}
        covers = shell_overlays_showing(compositor.get('overlay_shells'))
        return {'ok': not covers, 'backend': 'toplevel-pixels-layer', 'target': wid,
                'obscuring_window': '; '.join(covers) or None, 'obscuring_window_id': None}
    reason = 'the outputs do not show the window\'s pixels at the point'
    for attempt in range(attempts):
        if attempt:
            time.sleep(.2)
        window, screen = grim(wid), display_screen(display) if display else grim()
        size = (target.get('width'), target.get('height'))
        try:
            me = next(w for w in toplevel_pixel_windows(compositor)['windows'] if w['id'] == wid)
        except StopIteration:
            reason = 'the compositor no longer lists the window'
            continue
        try:
            if window[:2] == (1, 1):
                # A dialog labwc captures as 1x1: its own pixels come from its parent's capture.
                _, window, where = dialog_place(compositor, target.get('pid'), target.get('name', ''), me, size, screen)
            else:
                if window[:2] != size and window[0] >= size[0] and window[1] >= size[1]:
                    window = content_box(window, *size) or window
                where = place_pixels(compositor, target.get('name', ''), me['wm_class'], window, screen)['rect']
        except RuntimeError as error:
            reason = str(error)
            continue
        if where[:2] != [target.get('x'), target.get('y')]:
            reason = 'the window moved since it was placed'
            continue
        if not (where[0] <= point[0] < where[0] + where[2] and where[1] <= point[1] < where[1] + where[3]):
            return {'ok': False, 'backend': 'toplevel-pixels-pick', 'target': wid,
                    'obscuring_window': 'point outside the window', 'obscuring_window_id': None}
        if pixels_at(window, screen, where, point):
            return {'ok': True, 'backend': 'toplevel-pixels-pick', 'target': wid, 'obscuring_window': None}
    # No window was found covering the point: the witness could not place or match the target's
    # pixels. That is not evidence against the app, so the guard reports it as unverified.
    return {'ok': False, 'unverified': True, 'backend': 'toplevel-pixels-pick', 'target': wid,
            'obscuring_window': reason, 'obscuring_window_id': None}


class WaylandPlacement:
    """Wayland clients report AT-SPI extents relative to their own window.

    Qt places a top-level at (0,0) and its children relative to the content
    origin; it cannot know where the compositor put it. Map each client
    window to the compositor's frame (same pid, title, plausible size), split
    client-side decorations symmetrically (left=right=bottom, title bar on
    top), and refuse anything implausible instead of guessing.
    """
    MAX_SIDE, MAX_TOP = 24, 120
    MAP_SETTLE = 2.5

    def __init__(self, compositor):
        self.compositor = compositor
        self.shell = compositor.get('shell_application', '')
        self._windows, self._cache, self._failed = None, {}, None
        self.pixels, self._screen = compositor.get('kind') == 'toplevel-pixels', None
        # Neither labwc nor Hyprland lists xdg popups as windows.
        self.relative_popups = compositor.get('kind') in ('toplevel-pixels', 'hyprctl')

    def screen(self):
        if self._screen is None:
            self._screen = grim()
        return self._screen

    def windows(self):
        if self._failed:
            raise RuntimeError(self._failed)  # one silent witness per walk, not one wait per row
        if self._windows is None:
            try:
                self._windows = [w for w in compositor_windows(self.compositor)['windows'] if not w.get('minimized')]
            except RuntimeError as error:
                self._failed = str(error)
                raise
        return self._windows

    def absolute(self, application):
        return bool(self.shell) and fnmatch.fnmatchcase(application or '', self.shell)

    def toplevel(self, pid, title, rect):
        key = ('top', pid, title, tuple(rect))
        if key not in self._cache:
            self._cache[key] = self._toplevel(pid, title, rect)
        return self._cache[key]

    def _toplevel(self, pid, title, rect):
        if self.pixels:
            return self._pixel_toplevel(pid, title, rect)
        x, y, width, height = rect
        deadline = time.monotonic() + self.MAP_SETTLE
        while True:
            # Qt's frameless selector sets an accessible name, but no caption.
            # This exception is only for the installed put.gd helper's exact,
            # undecorated cover size; ordinary anonymous windows stay refused.
            anonymous_selector = title in ('Capture region', 'Recording region') and owned(pid) and selector_process(pid)
            found = [w for w in self.windows() if w['pid'] == pid and
                     (w['title'] == title or (anonymous_selector and not w['title'] and w['frame'][2:] == [width, height])
                      # Plasma's banner has no accessible title; KWin names it a notification.
                      or (not title and w.get('notification') and w['frame'][2:] == [width, height])) and
                     w.get('type') not in POPUP_WINDOW_TYPES and
                     0 <= w['frame'][2] - width <= 2 * self.MAX_SIDE and 0 <= w['frame'][3] - height <= self.MAX_TOP + self.MAX_SIDE]
            if not found:
                found = self.recording_bar(pid, title, rect)
            # Qt reports a new (modal) window as showing before Mutter maps it:
            # the compositor lists it with an empty frame. Re-observe, bounded.
            unmapped = not found and any(w['pid'] == pid and w['title'] == title and not (w['frame'][2] and w['frame'][3])
                                         for w in self.windows())
            if not unmapped or time.monotonic() >= deadline:
                break
            time.sleep(.1)
            self._windows = None
        if unmapped:
            raise RuntimeError('compositor window for %r (pid %s) is not mapped after %g s (empty frame)%s'
                               % (title, pid, self.MAP_SETTLE, self.same_process(pid)))
        if len(found) != 1:
            raise RuntimeError('compositor window for %r (pid %s, %dx%d) is missing or ambiguous (%d matches)%s'
                               % (title, pid, width, height, len(found), self.same_process(pid)))
        frame = found[0]['frame']
        content = found[0].get('content')
        if content:
            # The compositor knows the client area exactly (KWin clientGeometry).
            if abs(content[2] - width) > 2 or abs(content[3] - height) > 2:
                raise RuntimeError('compositor content %s does not match %r content %dx%d' % (content, title, width, height))
            return {'dx': content[0] - x, 'dy': content[1] - y, 'window_id': found[0]['id'], 'frame': frame}
        horizontal, vertical = frame[2] - width, frame[3] - height
        side = horizontal // 2
        top = vertical - side
        if horizontal % 2 or side > self.MAX_SIDE or not 0 <= top <= self.MAX_TOP:
            raise RuntimeError('implausible decoration split for %r: frame %s vs content %dx%d' % (title, frame, width, height))
        return {'dx': frame[0] + side - x, 'dy': frame[1] + top - y, 'window_id': found[0]['id'], 'frame': frame}

    def _pixel_toplevel(self, pid, title, rect):
        """labwc: the one toplevel with this title whose app ID names the process, found by its own pixels."""
        x, y, width, height = rect
        if title in ('Capture region', 'Recording region') and owned(pid) and selector_process(pid):
            # put.gd's selector is a layer surface anchored to every edge of one output: it
            # is that output's rectangle. A size no single output has is refused.
            outputs = [o for o in wlr_outputs() if o[2:] == [width, height]]
            if len(outputs) != 1:
                raise RuntimeError('selector %dx%d matches %d outputs' % (width, height, len(outputs)))
            return {'dx': outputs[0][0] - x, 'dy': outputs[0][1] - y, 'window_id': 'layer:%d:%s' % (pid, title),
                    'frame': outputs[0]}
        if (title == 'put.gd recording controls' and owned(pid) and 'putgd-record' in executable(pid)
                and rect[:2] != [0, 0] and any(o[0] <= x and o[1] <= y and x + width <= o[0] + o[2]
                                                and y + height <= o[1] + o[3] for o in wlr_outputs())):
            # putgd-record anchors its bar to the output's top left with margins equal to the
            # position Qt keeps for the window and no exclusive zone, so layer-shell puts it
            # exactly there. A bar Qt reports at the origin is not trusted.
            return {'dx': 0, 'dy': 0, 'window_id': 'layer:%d:%s' % (pid, title), 'frame': list(rect)}
        found = [w for w in self.windows() if w['title'] == title and app_id_names(w['wm_class'], pid)]
        if len(found) != 1:
            shown = ', '.join('%r (%s)' % (w['title'], w['wm_class']) for w in self.windows()[:16]) or 'none'
            raise RuntimeError('toplevel for %r (pid %s) is missing or ambiguous (%d matches); toplevels: %s'
                               % (title, pid, len(found), shown))
        # A dialog shows in AT-SPI before its first buffer: its capture is a 1x1 placeholder.
        deadline = time.monotonic() + self.MAP_SETTLE
        image = grim(found[0]['id'])
        while image[:2] != (width, height) and time.monotonic() < deadline:
            time.sleep(.2)
            image = grim(found[0]['id'])
            self._screen = None
        if image[:2] == (1, 1):
            # labwc 0.20 captures a transient (modal) dialog as a 1x1 placeholder.
            return self._dialog(pid, title, rect, found[0])
        if image[:2] != (width, height) and image[0] >= width and image[1] >= height:
            # A client-side shadow (Firefox) is captured with the window: transparent black around
            # the window geometry. Keep the content box when it is exactly the accessible size.
            image = content_box(image, width, height) or image
        if image[:2] != (width, height):
            raise RuntimeError('toplevel %r captures %dx%d but its accessible window is %dx%d'
                               % (title, image[0], image[1], width, height))
        deadline = time.monotonic() + self.MAP_SETTLE
        while True:
            # A window just shown may still be under one that is closing: look again, bounded.
            try:
                content = place_pixels(self.compositor, title, found[0]['wm_class'], image, self.screen())['rect']
                break
            except RuntimeError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.2)
                self._screen = None
        return {'dx': content[0] - x, 'dy': content[1] - y, 'window_id': found[0]['id'], 'frame': content}

    def _dialog(self, pid, title, rect, window):
        """A dialog labwc captures as 1x1, at its window-rule position (see dialog_place)."""
        x, y, width, height = rect
        deadline = time.monotonic() + self.MAP_SETTLE
        while True:
            try:
                parent, _, box = dialog_place(self.compositor, pid, title, window, (width, height), self.screen())
                break
            except RuntimeError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.2)
                self._screen = None
        return {'dx': box[0] - x, 'dy': box[1] - y, 'window_id': window['id'], 'frame': box, 'parent': parent}

    def recording_bar(self, pid, title, rect):
        """Narrow captionless KWin layer-shell association; never a title wildcard.

        Layer-shell carries no xdg_toplevel title. Require the actual KWin
        surface class, exact undecorated client size, owned recording helper
        PID and a compositor identity new since recording.prepare. Ambiguous
        surfaces still fail in _toplevel; foreground still picks the exact ID.
        """
        if (self.compositor.get('kind') == 'hyprctl' and title == 'put.gd recording controls'
                and owned(pid) and 'putgd-record' in executable(pid)):
            # Hyprland lists the bar as one of putgd-record's overlay layer surfaces; its size is the bar's.
            return [w for w in self.windows() if w['pid'] == pid and w.get('layer') == 'overlay'
                    and w.get('wm_class') == 'putgd-record' and w['frame'][2:] == list(rect[2:])]
        before = self.compositor.get('recording_bar_before')
        if (self.compositor.get('kind') != 'kwin-script' or before is None
                or title != 'put.gd recording controls' or not owned(pid)
                or 'putgd-record' not in executable(pid)):
            return []
        return [w for w in self.windows() if w['pid'] == pid and w['id'] not in before
                and not w['title'] and w.get('surface_class') == 'KWin::LayerShellV1Window'
                and w.get('wm_class') == 'putgd-record'
                and w.get('normal') and not w.get('popup')
                and w.get('content') == w['frame'] and w['frame'][2:] == list(rect[2:])]

    def popup(self, pid, rect):
        key = ('popup', pid, tuple(rect))
        if key not in self._cache:
            found = [w for w in self.windows() if w['pid'] == pid and w.get('type') in POPUP_WINDOW_TYPES and
                     abs(w['frame'][2] - rect[2]) <= 2 and abs(w['frame'][3] - rect[3]) <= 2]
            if len(found) != 1:
                raise RuntimeError('compositor popup window for pid %s (%dx%d) is missing or ambiguous (%d matches)%s'
                                   % (pid, rect[2], rect[3], len(found), self.same_process(pid)))
            frame = found[0]['frame']
            self._cache[key] = {'dx': frame[0] - rect[0], 'dy': frame[1] - rect[1], 'window_id': found[0]['id'], 'frame': frame}
        return self._cache[key]

    def same_process(self, pid):
        """What the compositor does show for this (owned) process, so a refusal is diagnosable."""
        shown = ['%r %s type %s' % (w['title'], w['frame'], w.get('type'))
                 + (' (%s)' % w['surface_class'] if w.get('surface_class') else '')
                 for w in self.windows() if w['pid'] == pid][:16]
        return '; compositor has for pid %s: %s' % (pid, ', '.join(shown) if shown else 'no windows')

    def inside(self, rect, frame):
        return (frame[0] - 1 <= rect[0] and frame[1] - 1 <= rect[1] and
                rect[0] + rect[2] <= frame[0] + frame[2] + 1 and rect[1] + rect[3] <= frame[1] + frame[3] + 1)


def shift(rect, placement):
    return [rect[0] + placement['dx'], rect[1] + placement['dy'], rect[2], rect[3]] if rect else rect


def extents(node, kind=None):
    import pyatspi
    try:
        value = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS if kind is None else kind)
    except (NotImplementedError, RuntimeError) as error:
        if unanswered(error):
            raise
        return None
    return list(value) if value is not None else None


def accessible(query, toplevel=False, detail=True):
    """Bounded walk of the live accessibility tree.

    toplevel: stop at window level (application children); geometry and
    windows() only ever return those rows, so a hidden Settings page or an
    editor's controls are not read for them. detail=False skips text,
    description and key bindings, which control_geometry() never uses.
    """
    import pyatspi
    bound_calls()
    owners = {}
    def is_owned(pid):
        if pid not in owners:
            owners[pid] = owned(pid)
        return owners[pid]
    unknown = set(query) - QUERY_KEYS
    if unknown:
        raise ValueError('unknown accessibility query keys: ' + ', '.join(sorted(unknown)))
    wayland = os.environ.get('XDG_SESSION_TYPE') == 'wayland'
    placement = WaylandPlacement(query['compositor']) if wayland and query.get('compositor') else None
    # GTK4 on Wayland answers screen extents as (0,0) for every node; its
    # window-relative extents are real. Only a query that asks reads them.
    kind = None
    if query.get('coordinates') == 'window':
        if not wayland or placement is None:
            raise RuntimeError('window-relative extents need a Wayland compositor witness')
        kind = pyatspi.WINDOW_COORDS
    elif query.get('coordinates') not in (None, 'screen'):
        raise ValueError('coordinates must be screen or window')
    tops = {}
    def place(row):
        """Desktop coordinates for a Wayland client row, or None with a reason."""
        if not wayland or not row['rect']:
            return 'desktop', None
        if placement is None:
            return 'window-relative', 'no compositor window witness for this Wayland profile'
        if placement.absolute(row['application']):
            return 'desktop', None
        try:
            if row['popup'] and placement.relative_popups:
                # xdg_popup.configure tells the client where its popup is, relative to the
                # parent; Qt keeps that in the parent's own coordinates. Place it by the
                # one parent toplevel of that process.
                parents = [(name, rect) for (pid, name), rect in tops.items() if pid == row['pid'] and rect]
                if len(parents) != 1:
                    return 'window-relative', 'popup parent of pid %s is missing or ambiguous (%d)' % (row['pid'], len(parents))
                parent = placement.toplevel(row['pid'], parents[0][0], parents[0][1])
                where = dict(parent, frame=shift(row['popup']['rect'], parent))
            elif row['popup']:
                where = placement.popup(row['pid'], row['popup']['rect'])
            else:
                top = tops.get((row['pid'], row['window']))
                if top is None:
                    return 'window-relative', 'top-level window %r was not observed' % row['window']
                where = placement.toplevel(row['pid'], row['window'], top)
        except RuntimeError as error:
            return 'window-relative', str(error)
        placed = shift(row['rect'], where)
        if not placement.inside(placed, where['frame']):
            # Scrolled past the window edge: not an input target, but evidence
            # masking still needs where it would be drawn (clipped by the crop).
            row['unclipped_rect'] = placed
            return 'window-relative', 'placed rectangle %s falls outside compositor frame %s' % (placed, where['frame'])
        row['rect'] = placed
        row['window_id'] = where['window_id']
        if row['popup'] and placement.relative_popups:
            # The popup's own rectangle goes to the input guard with the row's point, so it
            # must be in the same desktop coordinates: on labwc and Hyprland the accessible
            # one is in the parent's coordinates. A copy: the walk shares it between rows.
            row['popup'] = dict(row['popup'], rect=list(where['frame']))
        for ancestor in row['ancestors']:
            if ancestor.get('rect') not in (None, UNREAD) and ancestor.get('role') != 'application':
                ancestor['rect'] = shift(ancestor['rect'], where)
        return 'desktop', None
    matches_ = []
    stack = [(desktop_root(), 0, '', '', None, [])]
    # LIFO: popup roots go first so the ordinary tree is walked before them;
    # a popup already reached there (a button's own menu) is not walked twice.
    extra = popup_roots(query)
    stack[:0] = [root for _, root in extra]
    extra_keys = {id(root[0]): key for key, root in extra}
    seen = set()
    visited, truncated = 0, False
    deadline = (PROCESS_STARTED or time.monotonic()) + WALK_SECONDS
    spent, silent = {}, {}
    # Unscoped by pid: learn every application's name at once before any walk.
    names = UNREAD if query.get('pid') is None and not query.get('owned') else None
    while stack and visited < 20000:
        if time.monotonic() > deadline:
            raise RuntimeError(walk_budget_error(spent, visited))
        node, depth, window, application, popup, ancestors = stack.pop()
        visited += 1
        started, pid = time.monotonic(), None
        try:
            if id(node) in extra_keys:
                if extra_keys[id(node)] in seen:
                    continue
            elif extra_keys:
                seen.add((node.get_process_id(), node.path))
            if depth == 1 and (query.get('pid') is not None or query.get('owned')):
                # Applications are scoped by the bus's pid before any call into
                # them: a busy or hung foreign application is never contacted.
                pid = node.get_process_id()
                if (query.get('pid') is not None and pid != query['pid']) or (query.get('owned') and not is_owned(pid)):
                    continue
            bus = (getattr(node, 'bus', None) or getattr(getattr(node, 'app', None), 'bus_name', None)
                   if depth == 1 and names is not None else None)
            if bus and names is UNREAD:
                names = application_names()
            if bus and names:
                known = names.get(bus, '')
                if known is False:
                    continue  # gone since the registry listed it
                if known is None:
                    pid = node.get_process_id()
                    pattern = query.get('application')
                    if pattern and not any(fnmatch.fnmatchcase(n, pattern) for n in executable(pid)):
                        continue  # cannot be the queried application; never contacted
                    entry = silent.setdefault((' / '.join(executable(pid)), pid), [0, 'no answer to its name'])
                    entry[0] += 1
                    continue
                if known and query.get('application') and not fnmatch.fnmatchcase(known, query['application']):
                    continue
            pid = node.get_process_id()
            server_role = node.getRoleName()
            role, name = normalize_role(server_role), node.name or ''
            # Do not walk unrelated applications for owned-control queries.
            if role == 'application':
                application = name
                if query.get('application') and not fnmatch.fnmatchcase(application,query['application']):
                    continue
                if query.get('pid') is not None and pid != query['pid']:
                    continue
                if query.get('owned') and not is_owned(pid):
                    continue
            if role in WINDOW_ROLES or (ancestors and ancestors[-1]['role']=='application' and role not in POPUP_ROLES):
                window, popup = name, None
                if placement is not None and not placement.absolute(application):
                    tops[(pid, name)] = extents(node, kind)
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
            # Extents/attributes/text are one D-Bus round trip each: fetch them
            # only for rows that can match and for popups. Ancestor rectangles
            # are filled in for matched rows only (a shell tree has ~2000 nodes).
            rect = extents(node, kind) if potential or role in POPUP_ROLES else UNREAD
            if role in POPUP_ROLES and states['showing']:
                popup = {'name': name, 'role': role, 'rect': rect, 'pid': pid,
                         'a11y_bus': getattr(node, 'bus', None), 'a11y_path': getattr(node, 'path', None)}
            if potential:
                attributes = dict(a.split(':', 1) for a in node.getAttributes() if ':' in a)
                protected = role == 'password text'
                text = ''
                if detail and not protected:
                    try:
                        interface = node.queryText()
                        text = interface.getText(0, min(interface.characterCount, 65536))
                    except (NotImplementedError, RuntimeError) as error:
                        if unanswered(error):
                            raise
                accelerator = ''
                if detail:
                    try:
                        action = node.queryAction()
                        accelerator = ';'.join(action.getKeyBinding(i) for i in range(action.nActions))
                    except (NotImplementedError, RuntimeError) as error:
                        if unanswered(error):
                            raise
                object_name = next((attributes[k] for k in ('id', 'objectName', 'object-name', 'accessible-id') if k in attributes), '')
                identifier = ''
                if not object_name:
                    identifier = accessible_id(node)
                    object_name = id_object_name(identifier, role)
                row = {'window': window, 'application': application, 'name': name, 'role': server_role,
                       'a11y_bus': getattr(node, 'bus', None), 'a11y_path': getattr(node, 'path', None),
                       'text': text, 'description': '' if protected or not detail else (node.description or ''),
                       'states': states, 'rect': rect, 'attributes': attributes, 'pid': pid,
                       'owned': is_owned(pid), 'objectName': object_name, 'accessibleId': identifier,
                       'accelerator': accelerator,
                       'visible': states['visible'], 'showing': states['showing'], 'enabled': states['enabled'],
                       'protected': protected, 'popup': popup, 'ancestors': ancestors}
                row['bounds'] = dict(zip(('x', 'y', 'width', 'height'), rect)) if rect else None
                row['coordinate_space'], reason = 'desktop', None
                if wayland:
                    # Translate before matching: 'within' is in desktop coordinates.
                    for ancestor in ancestors:
                        if ancestor['rect'] is UNREAD:
                            ancestor['rect'] = extents(ancestor['_node'], kind)
                    row['ancestors'] = [dict(a) for a in ancestors]  # shifted copies; children keep originals
                    row['coordinate_space'], reason = place(row)
                    if reason:
                        row['coordinate_error'] = reason
                        row['rect'] = row['bounds'] = None
                    elif row['rect']:
                        row['bounds'] = dict(zip(('x', 'y', 'width', 'height'), row['rect']))
                if matches(row, query):
                    for ancestor in row['ancestors']:
                        if ancestor['rect'] is UNREAD:
                            ancestor['rect'] = extents(ancestor['_node'], kind)
                    matches_.append(row)
            if toplevel and depth >= 2:
                pass
            elif depth < 50:
                ancestry = ancestors + [{'name':name,'role':server_role,'rect':rect,'pid':pid,'_node':node}]
                stack.extend((child, depth + 1, window, application, popup, ancestry) for child in node if child is not None)
            elif getattr(node,'childCount',0):
                truncated = True
        except (RuntimeError, AttributeError, NotImplementedError) as error:
            # A vanished object is skipped as before; an unanswered call makes
            # the walk incomplete, so absence and ambiguity are not provable.
            if unanswered(error):
                entry = silent.setdefault((application, pid), [0, str(error)[:160]])
                entry[0] += 1
            continue
        finally:
            used = spent.setdefault((application, pid), [0, 0.0])
            used[0] += 1
            used[1] += time.monotonic() - started
    if stack or truncated:
        raise RuntimeError('accessibility scan exceeded bounded tree size/depth')
    if silent:
        (application, pid), (count, error) = max(silent.items(), key=lambda item: item[1][0])
        raise RuntimeError('accessibility walk incomplete: %s (pid %s) did not answer %d call(s) within %d ms (%s)'
                           % (application or ' / '.join(executable(pid)) or 'an application', pid, count, CALL_TIMEOUT_MS, error))
    for row in matches_:
        row['ancestors'] = [{k: (None if v is UNREAD else v) for k, v in a.items() if not k.startswith('_')} for a in row['ancestors']]
    return matches_


def walk_budget_error(spent, visited):
    if not spent:
        return ('accessibility walk exceeded its %g s budget after 0 nodes: probe setup (compositor witness, '
                'observer popups) used the whole budget' % WALK_SECONDS)
    (application, pid), (nodes, seconds) = max(spent.items(), key=lambda item: item[1][1])
    return ('accessibility walk exceeded its %g s budget after %d nodes: %s (pid %s) took %.1f s for %d nodes '
            '(%.1f ms per node); its accessibility replies are too slow to observe'
            % (WALK_SECONDS, visited, application or ' / '.join(executable(pid)) or 'an application', pid,
               seconds, nodes, 1000 * seconds / max(nodes, 1)))


def label(value):
    return (value or '').split('\t')[0].replace('&&', '\0').replace('&', '').replace('\0', '&')


# Qt 6.4 QAccessibleToolButton::role returns CheckBox for a checkable
# toolbutton without a menu (including the editor's recent-color swatches).
CLASS_ROLES = {'QPushButton': ('push button',), 'QToolButton': ('push button', 'toggle button', 'check box', 'menu'),
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
    toolTip and text, so "Gallery" also finds "Gallery view".
    Rectangle/layout equality is never a substitute for runtime state.
    """
    name, live = row.get('objectName'), label(row.get('name'))
    if selector.startswith('class:'):
        return 1 if normalize_role(row['role']) in CLASS_ROLES.get(selector[6:], ()) else None
    if name and (selector == name or (selector.startswith('glob:') and fnmatch.fnmatchcase(name, selector[5:])) or
                 any(c.get('name') == name for c in candidates)):
        return 0
    if not live:
        return None
    if selector.startswith('text:') and live == selector[5:]:
        return 1
    if selector.startswith('text-glob:') and fnmatch.fnmatchcase(live, selector[10:]):
        return 1
    # A label's text can equal a named combo's accessibleName on Qt 6.4.
    # Apply each matching candidate's class semantics; coordinates never break
    # ties. Unknown catalog classes retain the existing label-only contract.
    return 1 if any(live in candidate_labels(c) and
                    (c.get('class') not in CLASS_ROLES or row['role'] in CLASS_ROLES[c['class']])
                    for c in candidates) else None


def control_geometry(query):
    # No name prefilter: catalog text often differs from the live accessible
    # name, and objectName matches must not be filtered out by labels.
    filters = {'owned':True,'visible':True,'showing':True,
               **({'pid':query['pid']} if query.get('pid') else {}),
               **({'role':query['role']} if query.get('role') else {}),
               **({'compositor':query['compositor']} if query.get('compositor') else {})}
    rows = accessible(filters, detail=False)
    unplaced = [row for row in rows if row.get('coordinate_error') and
                (selector_matches(row, query.get('selector',''), query.get('candidates', [])) is not None)]
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
    if not selected and unplaced:
        raise RuntimeError('owned live control has no desktop placement: ' + unplaced[0]['coordinate_error'])
    if len(selected) != 1:
        raise RuntimeError('owned live control is missing, hidden, disabled or ambiguous (' + str(len(selected)) + ' matches)')
    row = selected[0]
    # Qt can mark a partly clipped child SHOWING and export its full bounds.
    # Check the final resolved node, after any recipe scrolling/layout changes;
    # a safe centre alone does not prove the whole control is visible (#915).
    x, y, width, height = row['rect']
    ancestors = row.get('ancestors', [])
    # A native popup is a separate surface: its logical owner (or parent
    # menu) does not clip it. Its own descendants still must fit inside it.
    if row.get('popup'):
        for index in range(len(ancestors) - 1, -1, -1):
            if normalize_role(ancestors[index].get('role', '')) in POPUP_ROLES:
                ancestors = ancestors[index:]
                break
    for ancestor in ancestors:
        rect = ancestor.get('rect')
        if normalize_role(ancestor.get('role', '')) in ('desktop frame', 'application') or not rect or min(rect[2:]) <= 0:
            continue
        left, top, w, h = rect
        if not (left <= x and top <= y and x + width <= left + w and y + height <= top + h):
            raise RuntimeError('owned live control is clipped by ancestor ' + repr(ancestor.get('role')) + ' at ' + str(rect))
    return row


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
            'viewable': bool(re.search(r'^\s*Map State:\s*IsViewable\s*$', raw, re.M)),
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
    ids = search.stdout.split()
    if query.get('selector_screens'):
        # Native XCB covers have a PID but no WM_NAME, so xdotool's name
        # search omits them. Enumerate root children only for selector queries.
        children = run('xwininfo', '-root', '-children')
        ids += [str(int(w, 16)) for w in re.findall(r'^\s+(0x[0-9a-fA-F]+)\s', children, re.M)]
    found = []
    active_ids = re.findall(r'0x[0-9a-fA-F]+', run('xprop','-root','_NET_ACTIVE_WINDOW'))
    active = str(int(active_ids[-1],16)) if active_ids else None
    for wid in dict.fromkeys(ids):
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
            if query.get('selector_screens') and not info['viewable']:
                continue  # Root enumeration also includes not-yet-mapped covers.
            values = {'X': info['x'], 'Y': info['y'], 'WIDTH': info['width'], 'HEIGHT': info['height']}
            state = xprop(wid, '_NET_WM_STATE')
            extents = [int(v) for v in re.findall(r'\d+',xprop(wid,'_NET_FRAME_EXTENTS'))]
            frame = None
            if len(extents)==4:
                left,right,top,bottom=extents
                frame = {'x':int(values['X'])-left,'y':int(values['Y'])-top,
                         'width':int(values['WIDTH'])+left+right,'height':int(values['HEIGHT'])+top+bottom}
            # Native XCB covers deliberately have no WM_NAME. Do not discard
            # an otherwise verified cover when xdotool cannot read its name.
            name = ('' if query.get('selector_screens') and info['override_redirect'] and native_capture_process(pid)
                    else run('xdotool', 'getwindowname', wid))
            found.append({'x': int(values['X']), 'y': int(values['Y']), 'width': int(values['WIDTH']),
                          'height': int(values['HEIGHT']), 'scale': 1, 'pid': pid, 'window_id': wid,
                          'name': name, 'coordinate_space': 'framebuffer', 'frame':frame,
                          'override_redirect': info['override_redirect'],
                          'states': {'active':wid==active, 'minimized': '_NET_WM_STATE_HIDDEN' in state,
                                     'maximized': all(v in state for v in ('MAXIMIZED_VERT', 'MAXIMIZED_HORZ'))}})
        except (subprocess.CalledProcessError, ValueError, OSError):
            continue  # A window can close during observation.
    return found


def geometry(query):
    if (query.get('selector') and query.get('accessible', {}).get('name') == 'Capture region' and
            (query.get('compositor') or {}).get('kind') == 'gnome-lab-witness'):
        selector = gnome_selector()
        if selector:
            return dict(zip(('x','y','width','height'), selector['rect'])) | {
                'pid': selector['client_pid'], 'window_id': selector['request_id'], 'gnome_selector': True,
                'name': selector['name'], 'scale': 1, 'coordinate_space': 'logical'}
        raise RuntimeError('live window match is missing or ambiguous (0 matches)')
    if query.get('backend') == 'x11' or os.environ.get('XDG_SESSION_TYPE') == 'x11':
        if query.get('selector'):
            # Independently measured output covers, not catalog rectangles or
            # the accessible selection's changing extents.
            monitors = re.findall(r'\s(\d+)/\d+x(\d+)/\d+([+-]\d+)([+-]\d+)', run('xrandr', '--listactivemonitors'))
            query = dict(query, selector_screens=[[int(x),int(y),int(w),int(h)] for w,h,x,y in monitors])
            if not query['selector_screens']:
                raise RuntimeError('native selector output geometry is unavailable')
        found = x11_windows(query)
        if query.get('accessible'):
            nodes = accessible({'owned':True, 'visible':True, **query['accessible']})
            def bound(w, n):
                if n['pid'] != w['pid']:
                    return False
                box = [w[k] for k in ('x','y','width','height')]
                if (query.get('accessible', {}).get('name') == 'Capture region' and
                        query.get('selector_screens') and w.get('override_redirect') and
                        native_capture_process(w['pid'])):
                    # NativeRegionAccessible.rect() is the selection (initially
                    # the output union), not any individual cover window.
                    return box in query['selector_screens'] and n.get('name') == 'Capture region'
                return n.get('rect') == box
            if (not nodes and query.get('selector') and
                    query.get('accessible', {}).get('name') == 'Capture region' and
                    not (set(query['accessible']) - {'name', 'application'}) and
                    fnmatch.fnmatchcase('putgd-capture', query['accessible'].get('application', '*'))):
                # The native XCB cover maps before Qt's accessibility bridge.
                # Pointer input can bind it independently: exact owned helper,
                # anonymous override-redirect window and measured output bounds.
                # Its fixed application name follows from that exact helper;
                # any other accessibility constraint still needs a real node.
                # The existing hit-test still guards every pointer gesture;
                # keyboard input keeps its separate focus/grab witness.
                found = [w for w in found if owned(w.get('pid')) and native_capture_process(w.get('pid'))
                         and w.get('override_redirect') and not w.get('name')
                         and [w[k] for k in ('x', 'y', 'width', 'height')] in query['selector_screens']]
            else:
                found = [w for w in found if any(bound(w, n) for n in nodes)]
        # Native selector overlays can have one visible window per head. The
        # focused head (or explicitly selected head) disambiguates, never order.
        if query.get('point'):
            px, py = query['point']
            found = [w for w in found if w['x'] <= px < w['x']+w['width'] and w['y'] <= py < w['y']+w['height']]
    else:
        compositor = {'compositor': query['compositor']} if query.get('compositor') else {}
        # Only visible, showing windows are kept below, and only application
        # children can be windows: hidden pages and controls are never read
        # (Settings on GNOME: 1469 nodes, 38 s, versus a few window rows).
        rows = accessible({'owned': True, 'visible': True, 'showing': True, **query.get('accessible', {}), **compositor},
                          toplevel=True)
        found, unplaced = [], []
        for row in rows:
            top = row['role'] in WINDOW_ROLES or (row.get('ancestors') and row['ancestors'][-1]['role']=='application')
            if not top or not re.search(query['title'], row['name']):
                continue
            if row.get('coordinate_error') and row['visible'] and row['showing']:
                unplaced.append(row['name'] + ': ' + row['coordinate_error'])
            if not row['visible'] or not row['showing'] or not row['rect'] or min(row['rect'][2:]) <= 0:
                continue
            found.append(dict(zip(('x', 'y', 'width', 'height'), row['rect'])) |
                         {'scale': query.get('scale', 1), 'pid': row['pid'], 'name': row['name'],
                          'coordinate_space': 'logical', 'states': row['states'],
                          **({'window_id': row['window_id']} if row.get('window_id') is not None else {})})
        if not found and unplaced:
            raise RuntimeError('Wayland window cannot be placed on the desktop: ' + '; '.join(unplaced))
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
        return x11_hit_identity(path)

    finally:
        try:
            xlib.XCloseDisplay(display)
        finally:
            xlib.XSetErrorHandler(previous_handler)


def x11_hit_identity(path):
    """Resolve a WM decoration hit to its unique EWMH client, not the WM frame."""
    pid, name, client = None, '', None
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
    if path and (not name or pid is None):
        clients = [str(int(value,16)) for value in re.findall(
            r'0x[0-9a-fA-F]+', run('xprop','-root','_NET_CLIENT_LIST'))]
        candidates = []
        for wid in clients:
            try:
                if set(x11_ancestors(wid)) & set(path):
                    candidates.append(wid)
            except subprocess.CalledProcessError:
                continue  # A client closed during observation.
        if len(candidates) == 1:
            client = candidates[0]
            try:
                name = run('xdotool','getwindowname',client) or name
                pid = int(run('xdotool','getwindowpid',client))
            except (subprocess.CalledProcessError,ValueError):
                pass
    return {'path':path,'pid':pid,'name':name or 'unnamed X11 window','client':client}


def x11_active():
    ids = re.findall(r'0x[0-9a-fA-F]+', run('xprop', '-root', '_NET_ACTIVE_WINDOW'))
    value = str(int(ids[-1], 16)) if ids else '0'
    return value


def x11_settle(target, bound=2.5):
    """Let a just-mapped target become active before it is judged (#750).

    Right after a window maps, _NET_ACTIVE_WINDOW can still be 0 or another
    window of the same private process. Only then: activate the target once
    and wait, bounded, for the window manager to settle. A foreign active
    window is never displaced: the caller reports it as the obstruction. The
    caller then re-observes stacking and the hit-test from scratch.
    """
    wid = str(target['window_id'])
    deadline, activated = time.monotonic() + bound, False
    while True:
        active = x11_active()
        if active == wid:
            return True
        if active != '0':
            try:
                owner = int(run('xdotool', 'getwindowpid', active))
            except (subprocess.CalledProcessError, ValueError):
                owner = None
            if owner is None or owner != target.get('pid'):
                return False
        if time.monotonic() >= deadline:
            return False
        if not activated:
            # PID lookup and the deadline check can let a new foreground land.
            # Repeat the complete eligibility check immediately before sending
            # activation; a changed foreground is evaluated on the next pass.
            if x11_active() != active:
                continue
            subprocess.run(['xdotool', 'windowactivate', '--sync', wid], timeout=2,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            activated = True
        time.sleep(.1)


def x11_foreground(query):
    if query.get('desktop_node'):
        hit = x11_hit(query['point'])
        return {'ok': hit['pid'] == query['desktop_node']['pid'], 'backend':'x11-hit-test',
                'obscuring_window':hit['name'], 'obscuring_window_id':hit['path'][-1] if hit['path'] else None}
    target = query['geometry']
    wid = str(target['window_id'])
    if target.get('override_redirect'):
        return x11_unmanaged_foreground(query)
    point = query.get('point')
    if point and query.get('shell_cover'):
        # Shell-drawn chrome over the point refuses input before any stacking read.
        cover = shell_cover(point, query['shell_cover'])
        if cover:
            return {'ok': False, 'backend': 'shell-at-spi-cover', 'target': wid, 'obscuring_window': cover,
                    'obscuring_window_id': None}
    if query.get('settle'):
        x11_settle(target)
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
        cover = shell_cover(point,query['shell_cover']) if query.get('shell_cover') else None
        if cover:
            return {'ok':False,'backend':'shell-at-spi-cover','target':wid,'obscuring_window':cover}
        hit = x11_hit(point)
        ok = wid in hit['path']
        return {'ok': ok, 'backend': 'x11-unmanaged-hit-test', 'target': wid,
                'obscuring_window': None if ok else hit['name'],
                'obscuring_window_id': None if ok else (hit['path'][-1] if hit['path'] else None)}
    focus = x11_focus_owner()
    ok = focus not in ('0', '1') and wid in x11_ancestors(focus)
    if not ok and query.get('keyboard_grab') and native_capture_process(target['pid']):
        # The native helper reports focused iff xcb_grab_keyboard succeeded
        # and selection has not finished. Arbitrary Qt focus and another
        # process's override-redirect window cannot establish this contract.
        rows = accessible({'owned':True,'pid':target['pid'],'name':'Capture region','role':'panel',
                           'visible':True,'showing':True,'states':{'focused':True}})
        rows = [r for r in rows if r.get('pid') == target['pid'] and r.get('states',{}).get('focused')
                and r.get('rect') and intersect(dict(zip(('x','y','width','height'),r['rect'])),target)]
        centre = (target['x']+target['width']/2,target['y']+target['height']/2)
        cover = shell_cover(centre,query['shell_cover']) if query.get('shell_cover') else None
        hit = x11_hit(centre)
        if len(rows) == 1 and wid in hit['path'] and not cover:
            return {'ok':True,'backend':'native-selector-keyboard-grab','target':wid,'obscuring_window':None}
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


def verified_helper_process(pid, names):
    """Match an owned process against installation digests supplied by the controller."""
    import hashlib
    if not owned(pid):
        return False
    expected = {value for name in names if isinstance(value := VERIFIED_HELPER_SHA256.get(name), str)
                and re.fullmatch(r'[0-9a-f]{64}', value)}
    if not expected:
        return False
    try:
        with open('/proc/'+str(pid)+'/exe', 'rb') as actual:
            return hashlib.file_digest(actual, 'sha256').hexdigest() in expected
    except OSError:
        return False


def native_capture_process(pid):
    """Bind a native selector only to the installed capture helper's trusted digest."""
    return verified_helper_process(pid, ('putgd-capture',))


def selector_process(pid):
    """Bind anonymous selector covers to the controller-verified helper inventory."""
    return verified_helper_process(pid, ('putgd-capture', 'putgd-region', 'putgd-kde-region', 'putgd-ui'))


SHELL_PICKS = ('shell-at-spi', 'gnome-shell-at-spi', 'kwin-shell-at-spi')


def shell_applications(application):
    """Select the compositor by its bus-owner executable before reading AT-SPI.

    The registry can retain foreign clients with unsupported or stale Name
    properties. They cannot establish anything about the shell's stage (#898).
    A failed daemon ownership query or a failed matching shell still refuses.
    """
    for app in desktop_root():
        if app is None:
            continue
        try:
            pid = app.get_process_id()
        except NotImplementedError as error:
            # The bus daemon definitively reports a disconnected client. All
            # other failures, including timeouts, leave ownership unverified.
            cause = error.__cause__
            if getattr(cause, 'get_dbus_name', lambda: '')() == 'org.freedesktop.DBus.Error.NameHasNoOwner':
                continue
            raise
        if any(fnmatch.fnmatchcase(name, application) for name in executable(pid)):
            yield app


def shell_cover(point, application):
    """Shell-drawn chrome over an X11 point (banners, panels, shell menus).

    Muffin/Mutter X11 shells draw these in their stage, not as X windows, so
    the server hit-test reaches the client beneath them. Ask the shell's own
    AT-SPI pick: over client windows and bare desktop its deepest showing node
    is a full-stage container; anything smaller is chrome on top.
    """
    import pyatspi
    bound_calls()
    x, y = int(point[0]), int(point[1])
    observed = False
    for app in shell_applications(application):
        try:
            if not app or not fnmatch.fnmatchcase(app.name or '', application):
                continue
            for window in app:
                stage = list(window.queryComponent().getExtents(pyatspi.DESKTOP_COORDS))
                observed = True
                node, chain = window.queryComponent().getAccessibleAtPoint(x, y, pyatspi.DESKTOP_COORDS), []
                for _ in range(40):
                    if node is None or not node.getState().contains(pyatspi.STATE_SHOWING):
                        break
                    chain.append((normalize_role(node.getRoleName()), node.name or '',
                                  list(node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS))))
                    child = node.queryComponent().getAccessibleAtPoint(x, y, pyatspi.DESKTOP_COORDS)
                    if child is None or child.path == node.path:
                        break
                    node = child
                if chain and chain[-1][2] != stage:
                    kind = next((role for role, _, _ in chain if role in ('notification', 'alert') + POPUP_ROLES), chain[-1][0])
                    named = next((name for _, name, _ in reversed(chain) if name), '')
                    return (app.name or 'shell') + ' ' + kind + (' ' + repr(named) if named else '') + ' at ' + str(chain[-1][2])
                cover = shell_tree_cover(window, (x,y), app.name or 'shell', pyatspi)
                if cover:
                    return cover
        except (NotImplementedError, AttributeError) as error:
            raise RuntimeError('shell cover observation unavailable: '+type(error).__name__) from error
    if not observed:
        raise RuntimeError('shell cover observation unavailable: compositor stage was not observed')
    return None


def shell_tree_cover(window, point, application, api):
    """Fallback when Cinnamon's stage pick stops at a full-stage container.

    A showing notification can be a non-reactive actor: AT-SPI's point pick
    misses it, though the exported tree still witnesses its drawn rectangle.
    Only semantic shell covers containing the point qualify, never a label or
    a full-stage container. Read names inside that cover to identify it.
    """
    stack, visited = [window], set()
    while stack:
        node = stack.pop()
        key = getattr(node,'path',id(node))
        if key in visited:
            continue
        visited.add(key)
        if len(visited) > 5000:
            raise RuntimeError('shell cover tree exceeded 5000 nodes; occlusion unverified')
        if not node.getState().contains(api.STATE_SHOWING):
            continue
        role = normalize_role(node.getRoleName())
        if role in ('notification','alert') + POPUP_ROLES:
            rect = list(node.queryComponent().getExtents(api.DESKTOP_COORDS))
            if rect[0] <= point[0] < rect[0]+rect[2] and rect[1] <= point[1] < rect[1]+rect[3]:
                names, pending = [], [node]
                for _ in range(80):
                    if not pending:
                        break
                    child = pending.pop()
                    if not child.getState().contains(api.STATE_SHOWING):
                        continue
                    if normalize_role(child.getRoleName()) != 'password text' and child.name:
                        names.append(child.name)
                    pending.extend(c for c in child if c is not None)
                return application+' '+role+(' '+repr(' / '.join(names)) if names else '')+' at '+str(rect)
        stack.extend(child for child in node if child is not None)
    return None


def shell_hit(query):
    """The compositor shell's own AT-SPI stage pick (GNOME Shell, Cinnamon), not an app-local pick."""
    import pyatspi
    bound_calls()
    expected = query['desktop_node']
    if not fnmatch.fnmatchcase(expected.get('application',''), query['shell_application']):
        raise RuntimeError('desktop hit-test requires the profile compositor application')
    point = query['point']
    rect = expected.get('rect')
    if (not rect or len(rect) != 4 or min(rect[2:]) <= 0 or
            not (rect[0] <= point[0] < rect[0] + rect[2] and rect[1] <= point[1] < rect[1] + rect[3])):
        return {'ok': False, 'backend': 'shell-at-spi-pick', 'obscuring_window': 'expected shell control has no containing bounds'}
    roots = [app for app in desktop_root() if app and app.get_process_id()==expected['pid']]
    for app in roots:
        for window in app:
            try:
                node = window.queryComponent().getAccessibleAtPoint(int(point[0]),int(point[1]),pyatspi.DESKTOP_COORDS)
                for _ in range(50):
                    if node is None:
                        break
                    if (node.get_process_id()==expected['pid'] and (node.name or '')==expected['name']
                            and normalize_role(node.getRoleName())==normalize_role(expected['role'])
                            and list(node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)) == list(rect)):
                        return {'ok':True,'backend':'shell-at-spi-pick','obscuring_window':None}
                    child = node.queryComponent().getAccessibleAtPoint(int(point[0]),int(point[1]),pyatspi.DESKTOP_COORDS)
                    if child == node:
                        break
                    node = child
            except (RuntimeError,NotImplementedError,AttributeError):
                continue
    return {'ok':False,'backend':'shell-at-spi-pick','obscuring_window':'compositor shell stage did not pick the expected notification control'}


def native_shell_role(value):
    """Shell actors use ATK roles; AT-SPI role enum numbers are different.

    Resolve the native ABI's canonical name rather than guessing an offset.
    atk_role_get_name is non-localized; button aliases use the query matcher.
    """
    library = ctypes.util.find_library('atk-1.0')
    if not library:
        raise RuntimeError('native shell role names require the distro ATK library')
    atk = ctypes.CDLL(library)
    atk.atk_role_get_name.argtypes = [ctypes.c_int]
    atk.atk_role_get_name.restype = ctypes.c_char_p
    name = atk.atk_role_get_name(value)
    if not name:
        raise RuntimeError('native shell reported an unknown ATK role')
    return normalize_role(name.decode('utf-8'))


def shell_witness_hit(query):
    """Bind the live AT-SPI node to the shell's actual picked actor and PID."""
    if (query.get('compositor') or {}).get('kind') == 'kwin-script':
        return plasma_witness_hit(query)
    expected = query['desktop_node']
    pick = compositor_pick(query['compositor'],query['point'])
    ok = (bool(query['shell_application']) and type(expected.get('pid')) is int and expected['pid'] > 0
          and fnmatch.fnmatchcase(expected.get('application',''),query['shell_application'])
          and expected.get('pid') == pick.get('pid') and pick.get('window') is None)
    role = normalize_role(expected['role'])
    rect = expected.get('rect')
    ok = ok and bool(expected.get('name')) and bool(rect) and len(rect) == 4 and any(
        expected['name'] in (actor.get('accessible'),actor.get('text'))
        and type(actor.get('role')) is int and native_shell_role(actor['role']) == role
        and actor.get('rect') and len(actor['rect']) == 4 and all(abs(a-b)<=1 for a,b in zip(actor['rect'],rect))
        for actor in pick.get('shell_nodes',[]))
    window = pick.get('window')
    return {'ok':bool(ok),'backend':'gnome-native-shell-pick',
            'obscuring_window':None if ok else (window.get('title') if window else
                'native shell actor identity, role or bounds did not match the live desktop node')}


def plasma_witness_hit(query):
    """KWin proves the surface; Plasma's live component pick proves the actor.

    Plasma exports Qt window-relative coordinates. Never treat its tree as a
    GNOME stage or authorize a panel solely by its process/name/rectangle.
    """
    import pyatspi
    bound_calls()
    expected, point = query['desktop_node'], query['point']
    window = compositor_pick(query['compositor'], point).get('window')
    def refused(reason):
        return {'ok': False, 'backend': 'kwin-plasma-pick', 'obscuring_window': reason}
    if not window:
        return refused('KWin picked no Plasma surface')
    pid = expected.get('pid')
    if (type(pid) is not int or pid <= 0 or plasma_pid() != pid
            or not query.get('shell_application')
            or not fnmatch.fnmatchcase(expected.get('application',''), query['shell_application'])
            or window['pid'] != pid or window['id'] != expected.get('window_id')):
        return refused(window.get('title') or 'KWin surface does not own the Plasma node')
    if not expected.get('a11y_bus') or not expected.get('a11y_path') or not expected.get('rect'):
        return refused('Plasma node lacks a live accessibility identity or placed bounds')
    connection = a11y_bus()
    target = RawNode(connection, expected['a11y_bus'], expected['a11y_path'])
    if target.get_process_id() != pid:
        return refused('Plasma accessibility bus owner changed')
    # Re-resolve this exact identity to reject stale names, roles and bounds.
    rows = accessible({'pid':pid, 'application':query['shell_application'], 'name':glob_escape(expected['name']),
                       'role':expected['role'], 'visible':True, 'showing':True, 'compositor':query['compositor']})
    same = [r for r in rows if r.get('a11y_bus') == expected['a11y_bus']
            and r.get('a11y_path') == expected['a11y_path'] and r.get('window_id') == window['id']
            and r.get('rect') == expected['rect'] and r.get('name') == expected['name']
            and normalize_role(r.get('role','')) == normalize_role(expected['role']) and r.get('pid') == pid]
    if len(same) != 1:
        return refused('Plasma actor changed or disappeared before input')
    rect = expected['rect']
    if not (rect[0] <= point[0] < rect[0]+rect[2] and rect[1] <= point[1] < rect[1]+rect[3]):
        return refused('input point is outside the live Plasma actor')
    # Translate using this surface's measured client area and raw top-level
    # origin. getAccessibleAtPoint then supplies the deepest actor, not a
    # same-process cover, a duplicate label or a guessed panel coordinate.
    tops = [top for app in desktop_root() if app is not None and app.get_process_id() == pid for top in app]
    tops.extend(root[0] for _, root in popup_roots({'pid':pid, 'compositor':query['compositor']}))
    for top in tops:
        popup = expected.get('popup')
        if popup:
            # Qt's out-of-tree QMenu has a name but no window title. Bind its
            # actual root identity, never a guessed or empty title match.
            if (getattr(top,'bus',None),getattr(top,'path',None)) != (popup.get('a11y_bus'),popup.get('a11y_path')):
                continue
        elif (top.name or '') != expected.get('window'):
            continue
        raw = extents(top)
        content = window.get('content')
        if not raw or not content or any(abs(a-b)>2 for a,b in zip(raw[2:],content[2:])):
            continue
        x, y = int(point[0]-content[0]+raw[0]), int(point[1]-content[1]+raw[1])
        node = top
        picked = []
        for _ in range(50):
            child = node.queryComponent().getAccessibleAtPoint(x,y,pyatspi.DESKTOP_COORDS)
            if child is None or child == node:
                break
            node = child
            picked.append(node)
        # The target may be a semantic parent of the picked text/icon.
        if target in picked and all(n.get_process_id() == pid for n in picked):
            return {'ok':True, 'backend':'kwin-plasma-pick', 'obscuring_window':None}
    return refused('Plasma component pick did not reach the exact live actor')


def glob_escape(value):
    import glob
    return glob.escape(value)


def plasma_pid():
    import dbus
    bus = dbus.SessionBus()
    daemon = dbus.Interface(bus.get_object('org.freedesktop.DBus', '/org/freedesktop/DBus'), 'org.freedesktop.DBus')
    return int(daemon.GetConnectionUnixProcessID('org.kde.plasmashell', timeout=3))


def shell_state(query):
    """The desktop shell process this run started on, and signs that it crashed.

    query: {'process': comm name, 'crash_windows': [title regexes],
    'since': epoch seconds for core dumps, 'full': also read windows/cores}.
    A crashed Cinnamon is restarted by cinnamon-launcher under a new PID and
    shows "Cinnamon just crashed" in fallback mode (#814); input on that
    desktop proves nothing. Only processes of this user are considered.
    """
    import pwd
    name, uid = query['process'], os.getuid()
    found = []
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        try:
            with open('/proc/' + entry + '/comm') as stream:
                if stream.read().strip() != name or os.stat('/proc/' + entry).st_uid != uid:
                    continue
            with open('/proc/' + entry + '/stat') as stream:
                start = int(stream.read().rsplit(')', 1)[1].split()[19])  # field 22, starttime
            # A crashing shell keeps its PID while the kernel writes its core
            # (a minute or more for Cinnamon); CoreDumping marks that window.
            with open('/proc/' + entry + '/status') as stream:
                dumping = any(line.split() == ['CoreDumping:', '1'] for line in stream)
            found.append({'pid': int(entry), 'start': start, 'dumping': dumping})
        except (OSError, ValueError, IndexError):
            continue
    state = {'process': name, 'instances': sorted(found, key=lambda p: p['pid'])}
    if not query.get('full'):
        return state
    windows = []
    if os.environ.get('XDG_SESSION_TYPE') == 'x11':
        for pattern in query.get('crash_windows', []):
            search = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', pattern],
                                    text=True, capture_output=True, timeout=5)
            for wid in search.stdout.split():
                try:
                    windows.append({'window_id': wid, 'name': run('xdotool', 'getwindowname', wid)})
                except subprocess.CalledProcessError:
                    windows.append({'window_id': wid, 'name': pattern})
    state['crash_windows'] = windows
    cores = None
    if query.get('since') is not None:
        try:
            listing = subprocess.run(['coredumpctl', '--no-pager', '--json=short', 'list',
                                      '--since=@' + str(int(query['since'])), 'COREDUMP_COMM=' + name],
                                     text=True, capture_output=True, timeout=10)
            if listing.returncode == 0:
                cores = [{'pid': int(c.get('pid', 0)), 'time': c.get('time'), 'signal': c.get('sig')}
                         for c in json.loads(listing.stdout or '[]') if int(c.get('uid', -1)) == uid]
            elif 'No coredumps found' in listing.stderr:
                cores = []
        except (OSError, ValueError, subprocess.TimeoutExpired):
            cores = None  # no systemd-coredump on this host: the PID witness still applies
    state['cores'] = cores
    state['user'] = pwd.getpwuid(uid).pw_name
    return state


def witness_foreground(query):
    """Wayland focus/occlusion from the compositor's own state (read-only witness).

    Pointer input: the compositor's hit-test at the point must be the target
    window (or, with an open popup, a window of the same client). Keyboard
    input: the focused window must be the target or its transient. Shell
    chrome or another client at the point refuses input and is named.
    """
    compositor, target, point = query['compositor'], query['geometry'], query.get('point')
    if target.get('gnome_selector'):
        selector = gnome_selector(point)
        same = (selector and selector['request_id'] == target.get('window_id') and
                selector['client_pid'] == target.get('pid') and
                selector['rect'] == [target[k] for k in ('x','y','width','height')])
        ok = same and selector['pointer' if point is not None else 'keyboard']
        return {'ok': bool(ok), 'backend': 'gnome-owned-selector-pick',
                'obscuring_window': None if ok else 'put.gd selector request changed or its actor is covered'}
    wid = target.get('window_id')
    if wid is None:
        raise RuntimeError('Wayland target has no compositor window identity')
    raise_window = query.get('raise')
    if raise_window and compositor.get('kind') == 'toplevel-pixels':
        # Only a toplevel that is not already active: activating again would close its open popup,
        # and put.gd's own layer surfaces (selector, bar) have no handle to activate.
        raise_window = not str(wid).startswith('layer:') and not query.get('popup') and not any(
            w['id'] == wid and w.get('focus') for w in compositor_windows(compositor)['windows'])
    if raise_window:
        if not compositor_activate(compositor, wid):
            raise RuntimeError('compositor no longer has the target window')
        deadline = time.monotonic() + 2.5
        while time.monotonic() < deadline and not any(w['id'] == wid and w.get('focus') for w in compositor_windows(compositor)['windows']):
            time.sleep(.1)
    if point is not None and query.get('popup') and compositor.get('kind') in ('toplevel-pixels', 'hyprctl'):
        # A popup is drawn above its own parent; neither compositor lists or captures it alone.
        popup = query['popup']
        rect = popup.get('rect') or []
        inside = (len(rect) == 4 and min(rect[2:]) > 0
                  and rect[0] <= point[0] < rect[0] + rect[2] and rect[1] <= point[1] < rect[1] + rect[3])
        if popup.get('pid') != target.get('pid'):
            return {'ok': False, 'backend': compositor['kind'] + '-popup', 'target': wid,
                    'obscuring_window': 'popup of another process', 'obscuring_window_id': None}
        # The popup's rectangle was read from the screen just before; the click must fall inside it.
        return {'ok': inside, 'unverified': not inside, 'backend': compositor['kind'] + '-popup', 'target': wid,
                'obscuring_window': None if inside else 'the point %s is outside the observed popup %s' % (
                    [round(v, 1) for v in point], rect), 'obscuring_window_id': None}
    if point is not None and compositor.get('kind') == 'toplevel-pixels':
        result = toplevel_pixels_foreground(compositor, target, point, display=query.get('display'))
        if (not result['ok'] and query.get('raise') and not query.get('popup')
                and not str(wid).startswith('layer:') and compositor_activate(compositor, wid)):
            # labwc keeps a fullscreen window above an activated one until that one is activated again.
            # A display read before this activation no longer says what is on top: use grim's.
            time.sleep(.3)
            result = toplevel_pixels_foreground(compositor, target, point)
        return result
    if point is not None and compositor.get('kind') == 'hyprctl':
        return hyprland_foreground(target, point)
    if point is not None:
        pick = compositor_pick(compositor, point)
        window = pick.get('window')
        ok = bool(window) and (window['id'] == wid or (bool(query.get('popup')) and window['pid'] == target.get('pid')))
        if ok:
            return {'ok': True, 'backend': compositor['kind'] + '-pick', 'target': wid, 'obscuring_window': None}
        if window:
            name = window['title'] or window['wm_class'] or 'untitled window'
        else:
            chrome = [c for c in pick.get('chain', []) if c.get('name') or c.get('accessible')]
            name = 'shell ' + ((chrome[0].get('name') or chrome[0].get('accessible')) if chrome else 'chrome')
        return {'ok': False, 'backend': compositor['kind'] + '-pick', 'target': wid, 'obscuring_window': name,
                'obscuring_window_id': window['id'] if window else None}
    if compositor.get('kind') == 'toplevel-pixels' and str(wid).startswith('layer:'):
        return toplevel_pixels_foreground(compositor, target, None)
    windows = compositor_windows(compositor)['windows']
    focused = [w for w in windows if w.get('focus')]
    ok = bool(focused) and (focused[0]['id'] == wid or (focused[0]['pid'] == target.get('pid') and focused[0].get('transient_for') == wid))
    return {'ok': ok, 'backend': compositor['kind'] + '-focus', 'target': wid,
            'obscuring_window': None if ok else (focused[0]['title'] or focused[0]['wm_class'] if focused else 'no focused window'),
            'obscuring_window_id': None if ok or not focused else focused[0]['id']}


def foreground(query):
    # Compositor-drawn content (notification banners) is not an X window: an
    # X11 hit-test at a Cinnamon banner reaches the client window beneath it.
    # Muffin/Mutter shells pick their own stage through AT-SPI on X11 too.
    if query.get('desktop_node') and query.get('desktop_hit_test') in SHELL_PICKS:
        if (query.get('compositor') or {}).get('kind') in ('gnome-lab-witness', 'kwin-script'):
            return shell_witness_hit(query)
        return shell_hit(query)
    if os.environ.get('XDG_SESSION_TYPE') == 'x11':
        return x11_foreground(query)
    if query.get('compositor') and query.get('geometry'):
        return witness_foreground(query)
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


def process_name(pid):
    try:
        with open('/proc/%d/comm' % int(pid)) as stream:
            return stream.read().strip()
    except (OSError, TypeError, ValueError):
        return None


def layout(query):
    """Every visible desktop window: frame rectangle, pid, ownership, title, stacking.

    A larger stack is higher; None is unknown (treated as above).
    X11: the server's root-child order. Wayland: the compositor witness's own
    stacking; without one this raises (fail closed).
    """
    rows = []
    if os.environ.get('XDG_SESSION_TYPE') == 'x11':
        # The X server's own stacking of the root's children (xwininfo prints
        # the top-most first). Each client is ranked by its top-level ancestor,
        # which covers WM frames, override-redirect windows and helper windows
        # the WM keeps below every client (e.g. Muffin's guard window).
        tops = [str(int(v, 16)) for v in re.findall(r'^\s+(0x[0-9a-fA-F]+)', run('xwininfo', '-root', '-children'), re.M)]
        rank = {wid: len(tops)-index for index, wid in enumerate(tops)}
        for w in x11_windows({'title': '.*', 'owned': False}):
            frame = w.get('frame') or {k: w[k] for k in ('x', 'y', 'width', 'height')}
            chain = x11_ancestors(w['window_id'])
            if len(chain) == 1 and w['window_id'] not in rank:
                continue  # the root window itself (xdotool search matches it)
            top = chain[-2] if len(chain) > 1 else chain[-1]
            rows.append({'id': w['window_id'], 'title': w['name'], 'pid': w['pid'], 'process': process_name(w['pid']),
                         'owned': bool(w['pid'] and owned(w['pid'])),
                         'rect': [frame['x'], frame['y'], frame['width'], frame['height']],
                         'stack': rank.get(top), 'minimized': w['states']['minimized']})
        return {'windows': rows, 'coordinate_space': 'framebuffer'}
    for w in compositor_windows(query.get('compositor'))['windows']:
        if w.get('frame') is None:
            # labwc/Mir witnesses report no geometry: refuse rather than invent a rectangle.
            raise RuntimeError('this compositor witness reports no window geometry, so the desktop layout is unknown')
        pid = w.get('pid') if isinstance(w.get('pid'), int) and w.get('pid') > 1 else None
        rows.append({'id': str(w['id']), 'title': w.get('title') or '', 'pid': pid, 'process': process_name(pid) if pid else None,
                     'owned': bool(pid and owned(pid)), 'rect': list(w['frame']),
                     'stack': w.get('stack'), 'minimized': bool(w.get('minimized') or w.get('hidden')),
                     'layer': w.get('layer')})
    return {'windows': rows, 'coordinate_space': 'logical'}


def close_notifications(deadline):
    """Close every open banner: post one to learn the newest ID, then close back from it."""
    bus = 'org.freedesktop.Notifications'
    call = ['gdbus', 'call', '--session', '--dest', bus, '--object-path', '/org/freedesktop/Notifications', '--method']
    reply = run(*call, bus+'.Notify', 'put.gd lab', '0', '', 'put.gd lab', 'clearing the screen', '[]', '{}', '1000')
    newest = int(re.search(r'uint32 (\d+)', reply)[1])
    for identity in range(newest, max(0, newest-64), -1):
        left = deadline - time.monotonic()
        if left <= 0:
            break
        try:
            subprocess.run([*call, bus+'.CloseNotification', str(identity)], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=left)
        except subprocess.TimeoutExpired:
            break
    quickshell_dismiss(deadline)
    return newest


def quickshell_dismiss(deadline):
    """Omarchy's quickshell keeps a closed card on screen; its own IPC takes them all down."""
    try:
        owner = run('gdbus', 'call', '--session', '--dest', 'org.freedesktop.DBus', '--object-path', '/org/freedesktop/DBus',
                    '--method', 'org.freedesktop.DBus.GetConnectionUnixProcessID', 'org.freedesktop.Notifications')
        pid = int(re.search(r'uint32 (\d+)', owner)[1])
    except (OSError, subprocess.SubprocessError, TypeError, ValueError):
        return False
    if process_name(pid) != 'quickshell' or deadline - time.monotonic() <= 0:
        return False
    runtime = os.environ.get('PIPEWIRE_RUNTIME_DIR') or os.environ.get('XDG_RUNTIME_DIR', '')
    try:
        reply = subprocess.run(['qs', 'ipc', '--pid', str(pid), 'call', 'notifications', 'dismissAll'],
                               env=dict(os.environ, XDG_RUNTIME_DIR=runtime, LANG='C.UTF-8'),
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               timeout=max(.1, deadline - time.monotonic()))
    except (OSError, subprocess.TimeoutExpired):
        return False
    return reply.returncode == 0


def stop_windows(rows):
    """Stop these windows' processes: TERM all, wait two seconds once, then KILL what's left.

    A process is known by its pid and start time, read before TERM; KILL goes only to
    that same process, never to a pid reused meanwhile.
    """
    handles = {}
    for pid in {row['pid'] for row in rows}:
        if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 1:
            continue
        start = process_start(pid)
        try:
            handle = os.pidfd_open(pid)
        except OSError:
            continue
        # The descriptor names one process for good; it must be the one whose start time was read.
        if start and process_start(pid) == start:
            handles[pid] = handle
        else:
            os.close(handle)
    try:
        for handle in handles.values():
            with contextlib.suppress(ProcessLookupError):
                signal.pidfd_send_signal(handle, signal.SIGTERM)
        deadline = time.monotonic() + (2 if handles else 0)
        while time.monotonic() < deadline and any(process_start(pid) for pid in handles):
            time.sleep(.1)
        for handle in handles.values():
            with contextlib.suppress(ProcessLookupError):
                signal.pidfd_send_signal(handle, signal.SIGKILL)  # A no-op for a process that has exited.
    finally:
        for handle in handles.values():
            os.close(handle)


def clear_screen(query):
    """Close what isn't under test. Windows of this run and the desktop shell always stay.

    `obscuring` names one window to close and `above` closes every window over
    the named one. Neither closes only banners, so desktop icons and helpers
    below the test's windows are left alone. Eight seconds for banners plus two
    for windows keeps the pass within the persistent probe's 15-second reply limit.
    """
    deadline = time.monotonic() + 8
    closed = None
    if query.get('notifications', True):
        # A session can have no notification server, and windows can still be cleared.
        with contextlib.suppress(OSError, subprocess.SubprocessError, TypeError):
            closed = close_notifications(deadline)
    rows = layout({'compositor': query.get('compositor')})['windows']
    above = next((row for row in rows if str(row['id']) == str(query.get('above'))), None)
    targets, unknown = [], 0
    for row in rows:
        unknown += row['pid'] is None and not row['owned']
        if (row['owned'] or not isinstance(row['pid'], int) or row['pid'] <= 1 or row.get('layer')
                or row.get('minimized') or row['process'] is None or row['process'] in query.get('shell', [])):
            # Nor a window that isn't showing (minimized, or on another Hyprland workspace).
            # A layer-shell surface is the desktop's own (panel, wallpaper, notifications), and a
            # surface whose client has no known process (Hyprland reports pid -1) is never signalled.
            continue
        over = above is not None and (row['stack'] is None or above['stack'] is None or row['stack'] > above['stack'])
        if over or str(row['id']) == str(query.get('obscuring')):
            targets.append(row)
    stop_windows(targets)
    return {'newest_notification': closed, 'stopped': [{'title': row['title'], 'process': row['process']} for row in targets],
            'unknown_process': unknown}


def windows(query):
    if os.environ.get('XDG_SESSION_TYPE') == 'x11':
        return x11_windows(query)
    return [row for row in accessible(query, toplevel=True) if row['role'] in WINDOW_ROLES or
            (row.get('ancestors') and row['ancestors'][-1]['role']=='application')]


def notifier_item(item):
    """Watcher identity: bus/path (KDE), bus@/path (GNOME), or just bus."""
    service, separator, suffix = str(item).partition('/')
    if separator and service.endswith('@'):
        service = service[:-1]
    path = '/'+suffix if separator else '/StatusNotifierItem'
    unique = r':[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)+'
    named = r'[A-Za-z_-][A-Za-z0-9_-]*(?:\.[A-Za-z_-][A-Za-z0-9_-]*)+'
    if not re.fullmatch('(?:'+unique+'|'+named+')',service) or not re.fullmatch(r'/(?:[A-Za-z0-9_]+(?:/[A-Za-z0-9_]+)*)?',path):
        raise ValueError('invalid StatusNotifierWatcher item identity')
    return service,path


def notifier_process(pid):
    """Pin the same-user process start across an SNI property request."""
    directory='/proc/'+str(pid)
    if os.stat(directory).st_uid!=os.getuid():
        raise ValueError('notifier belongs to another account')
    with open(directory+'/stat') as stream:
        return stream.read().split(') ',1)[1].split()[19]


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
        try:
            service,path = notifier_item(item)
            owner = str(daemon.GetNameOwner(service))
            pid = int(daemon.GetConnectionUnixProcessID(owner))
            if not owned(pid) or query.get('pid') not in (None,pid):
                continue
            start = notifier_process(pid)
            properties = dbus.Interface(bus.get_object(owner,path),'org.freedesktop.DBus.Properties').GetAll('org.kde.StatusNotifierItem')
            # A stale item or an owner change during the property request must
            # never contaminate the private-family observation.
            if (str(daemon.GetNameOwner(service))!=owner or int(daemon.GetConnectionUnixProcessID(owner))!=pid
                    or notifier_process(pid)!=start or not owned(pid)):
                continue
        except (dbus.DBusException,OSError,ValueError):
            # A watcher can retain an item after its owner exited. Continue to
            # the actual private resident; outer watcher failures still fail.
            continue
        pixmaps = []
        for width,height,data in properties.get('IconPixmap',[]):
            payload = bytes(data)
            pixmap = {'width':int(width),'height':int(height),'bytes':len(payload),'sha256':hashlib.sha256(payload).hexdigest()}
            if query.get('pixels'):
                # The frames themselves, for matching the drawn icon (tray_pixels.py).
                import base64
                pixmap['argb'] = base64.b64encode(payload).decode()
            pixmaps.append(pixmap)
        tooltip = properties.get('ToolTip',('',[],'',''))
        rows.append({'pid':pid,'start':start,'owner':owner,'service':service,'path':path,'id':str(properties.get('Id','')),
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


def process_start(pid):
    try:
        with open('/proc/%d/stat' % int(pid)) as stream:
            return stream.read().split(') ', 1)[1].split()[19]
    except (OSError, ValueError, IndexError, TypeError):
        return None


def server_pid(query):
    """The persistent probe server's own identity; it exits when its SSH channel drops."""
    return {'pid': os.getpid(), 'start': process_start(os.getpid())}


def compositor_windows_probe(query):
    return compositor_windows(query['compositor'])


def follow_channel(query):
    """Terminate this monitor (SIGTERM → its own cleanup) once the run's probe channel is gone."""
    parent = query.get('parent')
    if not parent:
        return
    import signal
    import threading
    def watch():
        while process_start(parent.get('pid')) == parent.get('start'):
            time.sleep(1)
        os.kill(os.getpid(), signal.SIGTERM)
    threading.Thread(target=watch, daemon=True).start()


def sender_pid(sender):
    reply = run('gdbus', 'call', '--session', '--dest', 'org.freedesktop.DBus', '--object-path',
                '/org/freedesktop/DBus', '--method', 'org.freedesktop.DBus.GetConnectionUnixProcessID', sender)
    found = re.search(r'uint32 (\d+)', reply)
    return int(found[1]) if found else None


def notification_monitor(query):
    """Long-running desktop observer for one run, until terminated.

    Keeps the filtered dbus-monitor blocks (when a notification server owner
    was resolved) in a bounded in-memory buffer, served to this account over
    query['socket'] (mode 0600). Notification bodies never reach a file. Runs
    popup_watch as a child that dies with it.
    """
    import collections
    import select
    import signal
    import socket
    import threading
    def die_with_parent():
        ctypes.CDLL(None).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    watcher = subprocess.Popen([sys.executable, sys.argv[0], json.dumps({'probe': 'popup_watch',
                                'query': {'compositor':query.get('compositor')}})],
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               preexec_fn=die_with_parent)
    kept, lock, children = collections.deque(maxlen=4096), threading.Lock(), [watcher]
    path = query['socket']
    server = socket.socket(socket.AF_UNIX)
    previous = os.umask(0o177)
    try:
        server.bind(path)
    finally:
        os.umask(previous)
    os.chmod(path, 0o600)
    server.listen(4)
    def serve():
        while True:
            connection, _ = server.accept()
            with connection:
                with lock:
                    data = b''.join(kept)
                connection.settimeout(3)
                try:
                    connection.sendall(data)
                except OSError:
                    pass
    threading.Thread(target=serve, daemon=True).start()
    def stop(*_):
        for child in children:
            child.terminate()
        try:
            os.unlink(path)
        except OSError:
            pass
        sys.exit(0)
    signal.signal(signal.SIGTERM, stop)
    # Bound to the run's probe channel (query['parent']): when it is gone the
    # SIGTERM above ends the watcher, dbus-monitor and the socket.
    follow_channel(query)
    if not query.get('server'):
        watcher.wait()
        stop()
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
    child = subprocess.Popen(['dbus-monitor', '--session', *rules], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, preexec_fn=die_with_parent)
    children.append(child)
    buffer, block = b'', []
    def flush():
        if block and keeper.keep(''.join(block)):
            with lock:
                kept.append(''.join(block).encode()[:65536])
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
    child.wait()
    stop()


def notification_read(query):
    """Read this run's own in-memory monitor over its private socket (bounded)."""
    import socket
    import stat as stat_
    try:
        os.kill(int(query['pid']), 0)
        alive = True
    except (OSError, ValueError, TypeError):
        alive = False
    chunks, size = [], 0
    if alive:
        info = os.lstat(query['socket'])
        if not stat_.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise RuntimeError('notification monitor socket is not private to this account')
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(5)
            connection.connect(query['socket'])
            while True:
                data = connection.recv(65536)
                if not data:
                    break
                size += len(data)
                if size > 8 * 1024 * 1024:
                    raise RuntimeError('notification memory exceeded 8 MiB')
                chunks.append(data)
    return {'alive': alive, 'text': b''.join(chunks).decode('utf-8', 'replace')}


PORTAL_REQUEST_PREFIX = '/org/freedesktop/portal/desktop/request/'
PORTAL_MARKER_INTERFACE = 'org.putgd.LabPortalMonitor'
PORTAL_RULES = ("type='method_call',interface='org.freedesktop.portal.ScreenCast'",
                "type='method_call',interface='org.freedesktop.impl.portal.ScreenCast'",
                "type='method_call',interface='org.freedesktop.impl.portal.RemoteDesktop'",
                "type='method_call',interface='org.freedesktop.impl.portal.Request',member='Close'",
                "type='signal',interface='org.freedesktop.portal.Request',member='Response'",
                "type='signal',interface='" + PORTAL_MARKER_INTERFACE + "',member='Marker'")


def portal_request_path(sender, token):
    """xdg-desktop-portal's request object path for a caller's handle_token."""
    return PORTAL_REQUEST_PREFIX + sender.lstrip(':').replace('.', '_') + '/' + token


class PortalFilter:
    """ScreenCast consent bookkeeping from dbus-monitor blocks; no payload text.

    Keeps one JSON record per relevant message: the private family's own
    org.freedesktop.portal.ScreenCast calls (sender proven owned, with the
    handle_token's request path), every frontend→backend impl ScreenCast or
    RemoteDesktop call (request handle, app id and the backend's process),
    impl Request.Close, Request::Response codes and this run's marker.
    Options dictionaries, restore tokens and stream properties are dropped.
    """
    def __init__(self, sender_owned, backend):
        self.sender_owned, self.backend = sender_owned, backend

    def record(self, block):
        header = block.split('\n', 1)[0]
        def field(name):
            found = re.search(r'\b' + name + r'=(\S+?);?(?:\s|$)', header)
            return found[1] if found else None
        sender, destination, path = field('sender'), field('destination'), field('path')
        interface, member = field('interface'), field('member')
        at = re.search(r'\btime=([0-9.]+)', header)
        at = float(at[1]) if at else None
        if not sender or not interface or not member:
            return None
        objects = re.findall(r'^\s*object path "([^"]*)"', block, re.M)
        strings = re.findall(r'^\s*string "((?:[^"\\]|\\.)*)"', block, re.M)
        if header.startswith('method call') and interface == 'org.freedesktop.portal.ScreenCast':
            if not self.sender_owned(sender):
                return None
            token = re.search(r'string "handle_token"\s*variant\s+string "([A-Za-z0-9_]{1,128})"', block)
            return {'kind': 'public', 'member': member, 'sender': sender, 'at': at,
                    'handle': portal_request_path(sender, token[1]) if token else None,
                    'token': token[1] if token else None}
        if header.startswith('method call') and interface in ('org.freedesktop.impl.portal.ScreenCast',
                                                              'org.freedesktop.impl.portal.RemoteDesktop'):
            handle = objects[0] if objects and objects[0].startswith(PORTAL_REQUEST_PREFIX) else None
            # impl Start/SelectSources: (handle, session, app_id, parent_window, options)
            app_id = strings[0] if handle and member in ('Start', 'SelectSources', 'SelectDevices') and strings else None
            pid, executable = self.backend(destination)
            return {'kind': 'impl', 'interface': interface.rsplit('.', 1)[1], 'member': member, 'sender': sender,
                    'destination': destination, 'handle': handle, 'app_id': app_id, 'at': at,
                    'backend_pid': pid, 'backend_exe': executable}
        if header.startswith('method call') and interface == 'org.freedesktop.impl.portal.Request' and member == 'Close':
            return {'kind': 'close', 'sender': sender, 'handle': path, 'at': at}
        if header.startswith('signal') and interface == 'org.freedesktop.portal.Request' and member == 'Response':
            code = re.search(r'^\s*uint32 (\d+)', block, re.M)
            return {'kind': 'response', 'sender': sender, 'destination': destination, 'handle': path,
                    'code': int(code[1]) if code else None, 'at': at}
        if header.startswith('signal') and interface == PORTAL_MARKER_INTERFACE:
            # Attachment proof only: the sender (gdbus emit) has exited before
            # its pid could be read; the fixture matches a random token instead.
            return {'kind': 'marker', 'token': strings[0] if strings else '', 'at': at}
        return None


def process_executable(pid):
    try:
        return os.path.basename(os.readlink('/proc/%d/exe' % pid))
    except (OSError, TypeError):
        return None


def portal_monitor(query):
    """Long-running ScreenCast consent observer for one fixture, until terminated."""
    import select
    import signal
    def die_with_parent():
        ctypes.CDLL(None).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    owners, backends = {}, {}
    def sender_owned(sender):
        if sender not in owners:
            try:
                owners[sender] = owned(sender_pid(sender))
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError):
                owners[sender] = False
        return owners[sender]
    def backend(name):
        if name not in backends:
            try:
                pid = sender_pid(name) if name else None
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError):
                pid = None
            backends[name] = (pid, process_executable(pid))
        return backends[name]
    keeper = PortalFilter(sender_owned, backend)
    child = subprocess.Popen(['dbus-monitor', '--session', *PORTAL_RULES], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, preexec_fn=die_with_parent)
    signal.signal(signal.SIGTERM, lambda *_: (child.terminate(), sys.exit(0)))
    follow_channel(query)
    descriptor = os.open(query['log'], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    buffer, block, written = b'', [], [0]
    def flush():
        record = keeper.record(''.join(block)) if block else None
        block.clear()
        if record is not None and written[0] < 1024 * 1024:
            line = (json.dumps(record, sort_keys=True) + '\n').encode()
            written[0] += len(line)
            os.write(descriptor, line)
    while True:
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


LONG_RUNNING = ('notification_monitor', 'popup_watch', 'portal_monitor')



def main():
    global PROCESS_STARTED
    request = json.loads(sys.argv[1])
    if request.get('probe') not in LONG_RUNNING:
        PROCESS_STARTED = time.monotonic()
        # Before the persistent channel kills a fresh child (12 s), leave the
        # stalled frame on stderr so the timeout names where it stopped.
        import faulthandler
        faulthandler.dump_traceback_later(11, exit=False)
    try:
        function = {'accessible': accessible, 'geometry': geometry, 'control': control_geometry,
                    'foreground': foreground, 'windows': windows, 'tray':tray, 'layout': layout, 'resize':resize_witness,
                    'notification_monitor': notification_monitor, 'notification_read': notification_read,
                    'popup_watch': popup_watch, 'portal_monitor': portal_monitor, 'server_pid': server_pid,
                    'shell_state': shell_state, 'compositor_windows': compositor_windows_probe,
                    'clear_screen': clear_screen}[request['probe']]
        print(json.dumps({'ok': True, 'result': function(request['query'])}))
    except Exception as error:
        print(json.dumps({'ok': False, 'error': type(error).__name__ + ': ' + str(error)}))


if __name__ == '__main__':
    main()
