"""Mutter's view of the desktop, read through the test-only Shell extension (shell-extension/guifuzz@put.gd.test).

The extension exports Eval(code) on the Shell's connection; witness.js, sent through it once, installs the snapshot
function that this fence polls about 30 times a second. A snapshot holds Meta windows (stacking order), the Shell's own
chrome (panel, dock, message banner, overview) and put.gd's St actors (the region selector, the recording frame, the
countdown), which on GNOME are not windows. They are turned into the same dictionaries the KWin fence reports.
"""
import json
import os
import threading
import time

import dbus
import dbus.bus

HERE = os.path.dirname(os.path.abspath(__file__))
IFACE = 'org.putgd.GuiFuzz'
SHELL_STYLES = ('rgba(0, 0, 0, 0.43)', 'rgba(255, 255, 255, 0.92)', 'rgba(0, 0, 0, 0.78)', 'rgba(0, 0, 0, 0)')


def pseudo(ident, cls, frame, z, kind='ShellOverlay', title=''):
    frame = [int(v or 0) for v in frame]
    return {'id': ident, 'pid': 0, 'cls': cls, 'title': title, 'frame': frame, 'content': frame, 'active': False, 'min': False, 'popup': False,
            'dock': False, 'desktop': False, 'normal': False, 'notif': False, 'fullscreen': False, 'tr': None, 'kind': kind, 'z': z}


def convert(raw):
    """The witness's JSON into the fence snapshot: windows bottom to top, put.gd's Shell actors, then the Shell's chrome on top."""
    windows = []
    for w in raw['windows']:
        w = dict(w, content=w['frame'])
        if w.pop('hidden', False):
            w['min'] = True
        windows.append(w)
    z = len(windows)
    selector = False
    for index, actor in enumerate(raw.get('actors', [])):
        style = actor.get('style', '')
        if actor['t'].endswith('RegionSelector'):
            selector = True
            windows.append(pseudo('sel%d' % index, 'putgd-shell-selector', actor['frame'], z))
            for kid in actor.get('kids', []):
                if kid['name'] or kid.get('text'):
                    windows.append(pseudo('selk%d' % z, 'putgd-shell-selector-part', kid['frame'], z + 1, title=kid['name'] or kid.get('text', '')))
                    z += 1
        elif actor['t'] == 'StWidget' and any(s in style for s in SHELL_STYLES) and actor['name'] == '':
            windows.append(pseudo('frm%d' % index, 'putgd-shell-frame', actor['frame'], z))
        elif actor['t'] == 'StLabel' and actor.get('reactive') and actor.get('text', '').strip().isdigit():
            windows.append(pseudo('cnt%d' % index, 'putgd-shell-countdown', actor['frame'], z, title=actor['text']))
        elif actor['name'] == 'dashtodockContainer':
            windows.append(pseudo('dock', 'gnome-shell-dock', actor['frame'], z + 100, kind='ShellChrome'))
        z += 1
    chrome = raw.get('chrome', {})
    top = z + 200
    if chrome.get('panel'):
        windows.append(pseudo('panel', 'gnome-shell-panel', chrome['panel'], top, kind='ShellChrome'))
    if chrome.get('banner'):
        windows.append(pseudo('banner', 'gnome-shell-banner', chrome['banner'], top + 1, kind='ShellChrome'))
    if chrome.get('overview') or (chrome.get('modal') and not selector and not any(w['cls'] == 'putgd-shell-frame' for w in windows)):
        for index, output in enumerate(raw['outputs']):
            windows.append(pseudo('modal%d' % index, 'gnome-shell-overview' if chrome.get('overview') else 'gnome-shell-modal', output['geom'], top + 2, kind='ShellChrome'))
    return {'windows': windows, 'outputs': raw['outputs'], 'cursor': raw['cursor'], 'chrome': chrome}


class GnomeFence:
    """Same interface as the KWin fence: start(), stop(), get() -> (snapshot, time), serial."""

    def __init__(self):
        self.snapshot = {'windows': [], 'outputs': [], 'cursor': [0, 0], 'chrome': {}}
        self.stamp = 0.0
        self.serial = 0
        self.errors = 0
        self.last_error = ''
        self.lock = threading.Lock()
        self.stopping = threading.Event()
        self.thread = None
        self.proxy = None

    def connect(self):
        bus = dbus.bus.BusConnection(dbus.bus.BUS_SESSION)
        self.proxy = dbus.Interface(bus.get_object('org.gnome.Shell', '/org/putgd/GuiFuzz', introspect=False), IFACE)

    def eval(self, code, timeout=3):
        return str(self.proxy.Eval(code, timeout=timeout))

    def install(self):
        with open(os.path.join(HERE, 'witness.js')) as stream:
            reply = self.eval(stream.read())
        if reply != 'installed':
            raise RuntimeError('witness.js: ' + reply[:300])

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
                text = self.eval('return ctx.store.snap()', timeout=2)
                if text.startswith('ERROR') or text.startswith('TypeError'):
                    raise RuntimeError(text[:200])
                self.update(convert(json.loads(text)))
            except dbus.DBusException as error:
                self.errors += 1
                self.last_error = str(error)[:200]
                if 'UnknownMethod' in str(error) or 'ServiceUnknown' in str(error) or 'NoReply' in str(error):
                    time.sleep(0.5)
            except (RuntimeError, ValueError, KeyError) as error:
                self.errors += 1
                self.last_error = str(error)[:200]
                try:  # the Shell restarted its extension: send the witness again
                    self.install()
                except (dbus.DBusException, RuntimeError):
                    time.sleep(0.5)
            self.stopping.wait(max(0.0, 0.03 - (time.time() - started)))

    def start(self):
        self.connect()
        self.install()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
        deadline = time.time() + 5
        while self.serial == 0 and time.time() < deadline:
            time.sleep(0.05)
        if self.serial == 0:
            raise RuntimeError('the GNOME witness never reported: ' + self.last_error)

    def stop(self):
        self.stopping.set()

    def clipboard(self, kind, payload=b''):
        import base64
        return self.eval('return ctx.store.setClipboard(%s, %s)' % (json.dumps(kind), json.dumps(base64.b64encode(payload).decode())), timeout=5)

    def close(self, window_id):
        return self.eval('return ctx.store.close(%s)' % json.dumps(str(window_id)), timeout=4)
