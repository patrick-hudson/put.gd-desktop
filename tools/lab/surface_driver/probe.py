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
    raise RuntimeError('no compositor window witness configured for this Wayland profile')


def compositor_activate(compositor, window_id):
    """Explicit raise for focus(raise_window=True) on Wayland: the compositor's own activation."""
    kind = (compositor or {}).get('kind')
    if kind == 'gnome-lab-witness':
        import dbus
        return bool(gnome_witness('Activate', dbus.UInt64(window_id)))
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
        x, y, width, height = rect
        deadline = time.monotonic() + self.MAP_SETTLE
        while True:
            # Qt's frameless selector sets an accessible name, but no caption.
            # This exception is only for the installed put.gd helper's exact,
            # undecorated cover size; ordinary anonymous windows stay refused.
            anonymous_selector = title in ('Capture region', 'Recording region') and owned(pid) and selector_process(pid)
            found = [w for w in self.windows() if w['pid'] == pid and
                     (w['title'] == title or (anonymous_selector and not w['title'] and w['frame'][2:] == [width, height])) and
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

    def recording_bar(self, pid, title, rect):
        """Narrow captionless KWin layer-shell association; never a title wildcard.

        Layer-shell carries no xdg_toplevel title. Require the actual KWin
        surface class, exact undecorated client size, owned recording helper
        PID and a compositor identity new since recording.prepare. Ambiguous
        surfaces still fail in _toplevel; foreground still picks the exact ID.
        """
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
            if row['popup']:
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
    toolTip and text, so "List" also finds "Show history as a list".
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
    return verified_helper_process(pid, ('putgd-capture', 'putgd-kde-region', 'putgd-ui'))


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
    if query.get('raise'):
        if not compositor_activate(compositor, wid):
            raise RuntimeError('compositor no longer has the target window')
        deadline = time.monotonic() + 2.5
        while time.monotonic() < deadline and not any(w['id'] == wid and w.get('focus') for w in compositor_windows(compositor)['windows']):
            time.sleep(.1)
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
        rows.append({'id': str(w['id']), 'title': w.get('title') or '', 'pid': w.get('pid'), 'process': process_name(w.get('pid')),
                     'owned': bool(w.get('pid') and owned(w['pid'])), 'rect': list(w['frame']),
                     'stack': w.get('stack'), 'minimized': bool(w.get('minimized') or w.get('hidden'))})
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
    return newest


def signal_processes(pids, number):
    for pid in pids:
        with contextlib.suppress(ProcessLookupError):
            os.kill(pid, number)


def stop_windows(rows):
    """Stop these windows' processes: TERM all, wait two seconds once, then KILL what's left."""
    pids = {row['pid'] for row in rows}
    signal_processes(pids, signal.SIGTERM)
    deadline = time.monotonic() + (2 if pids else 0)
    while time.monotonic() < deadline and any(process_start(pid) for pid in pids):
        time.sleep(.1)
    signal_processes([pid for pid in pids if process_start(pid)], signal.SIGKILL)


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
    targets = []
    for row in rows:
        if row['owned'] or not row['pid'] or row['process'] in query.get('shell', []):
            continue
        over = above is not None and (row['stack'] is None or above['stack'] is None or row['stack'] > above['stack'])
        if over or str(row['id']) == str(query.get('obscuring')):
            targets.append(row)
    stop_windows(targets)
    return {'newest_notification': closed, 'stopped': [{'title': row['title'], 'process': row['process']} for row in targets]}


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
            pixmaps.append({'width':int(width),'height':int(height),'bytes':len(payload),'sha256':hashlib.sha256(payload).hexdigest()})
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
