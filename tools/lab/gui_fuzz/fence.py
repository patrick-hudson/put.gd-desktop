"""KWin's view of the desktop: a persistent KWin script that reports windows, popups and the cursor.

The script calls back over D-Bus; the receiver answers after a short delay, which paces the script
into a timer (KWin's JS has none). Reports arrive about 25 times a second, always at least 5 a second.
"""
import json
import os
import threading
import time

import dbus
import dbus.bus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

SCRIPT = r"""
const R = %(receiver)s, P = '/fence', I = 'org.putgd.Fuzz';
const rect = g => [Math.round(g.x), Math.round(g.y), Math.round(g.width), Math.round(g.height)];
function snap() {
    const list = workspace.stackingOrder, windows = [];
    for (let i = 0; i < list.length; i++) {
        const w = list[i];
        windows.push({id: w.internalId.toString(), pid: w.pid, cls: w.resourceClass, title: w.caption, frame: rect(w.frameGeometry),
                      content: rect(w.clientGeometry), active: !!w.active, min: !!w.minimized, popup: !!w.popupWindow, dock: !!w.dock,
                      desktop: !!w.desktopWindow, normal: !!w.normalWindow, notif: !!(w.notification || w.criticalNotification),
                      fullscreen: !!w.fullScreen, tr: w.transientFor ? w.transientFor.internalId.toString() : null, kind: String(w).split('(')[0], z: i});
    }
    const outputs = workspace.screens.map(o => ({name: o.name, geom: rect(o.geometry), scale: o.devicePixelRatio}));
    return JSON.stringify({windows: windows, outputs: outputs, cursor: [workspace.cursorPos.x, workspace.cursorPos.y]});
}
function tick() { callDBus(R, P, I, 'Tick', snap(), function () { tick(); }); }
tick();
"""


class Receiver(dbus.service.Object):
    def __init__(self, bus, path, fence):
        super().__init__(bus, path)
        self.fence = fence

    @dbus.service.method('org.putgd.Fuzz', in_signature='s', out_signature='b', async_callbacks=('reply', 'error'))
    def Tick(self, text, reply, error):
        try:
            self.fence.update(json.loads(str(text)))
        except ValueError:
            pass
        GLib.timeout_add(35, lambda: (reply(True), False)[1])


class Fence:
    def __init__(self):
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        self.bus = dbus.bus.BusConnection(dbus.bus.BUS_SESSION, mainloop=dbus.mainloop.glib.DBusGMainLoop())
        self.name = 'org.putgd.Fuzz.p%d' % os.getpid()
        self._busname = dbus.service.BusName(self.name, self.bus)
        self.snapshot = {'windows': [], 'outputs': [], 'cursor': [0, 0]}
        self.stamp = 0.0
        self.serial = 0
        self.lock = threading.Lock()
        self.receiver = Receiver(self.bus, '/fence', self)
        self.loop = GLib.MainLoop()
        self.thread = threading.Thread(target=self.loop.run, daemon=True)
        self.plugin = 'putgd-gui-fuzz-%d' % os.getpid()
        self.path = '/tmp/gfz-fence-%d.js' % os.getpid()

    def update(self, snapshot):
        with self.lock:
            self.snapshot, self.stamp, self.serial = snapshot, time.time(), self.serial + 1

    def get(self):
        with self.lock:
            return self.snapshot, self.stamp

    def start(self):
        self.thread.start()
        with open(self.path, 'w') as stream:
            stream.write(SCRIPT % {'receiver': json.dumps(self.name)})
        scripting = dbus.Interface(self.bus.get_object('org.kde.KWin', '/Scripting'), 'org.kde.kwin.Scripting')
        try:
            scripting.unloadScript(self.plugin)
        except dbus.DBusException:
            pass
        identifier = int(scripting.loadScript(self.path, self.plugin, signature='ss'))
        dbus.Interface(self.bus.get_object('org.kde.KWin', '/Scripting/Script%d' % identifier), 'org.kde.kwin.Script').run()
        deadline = time.time() + 5
        while self.serial == 0 and time.time() < deadline:
            time.sleep(0.05)
        if self.serial == 0:
            raise RuntimeError('KWin fence script never reported')

    def stop(self):
        try:
            dbus.Interface(self.bus.get_object('org.kde.KWin', '/Scripting'), 'org.kde.kwin.Scripting').unloadScript(self.plugin)
        except dbus.DBusException:
            pass
        self.loop.quit()
        try:
            os.unlink(self.path)
        except OSError:
            pass
