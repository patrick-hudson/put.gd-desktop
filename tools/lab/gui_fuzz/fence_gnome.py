"""Mutter's view of the desktop, read through the test-only Shell extension (shell-extension/guifuzz@put.gd.test).

The extension exports three fixed methods on the Shell's connection while the lab marker file exists (Snapshot, SetClipboard,
Close); this fence polls Snapshot about 30 times a second. It evaluates nothing it is sent. A snapshot holds Meta windows (stacking order), the Shell's own
chrome (panel, dock, message banner, overview) and put.gd's St actors (the region selector, the recording frame, the
countdown), which on GNOME are not windows. They are turned into the same dictionaries the KWin fence reports.
"""
import base64
import json
import os
import threading
import time

import dbus
import dbus.bus

from gui_fuzz.snapshot import pseudo, stamp_first_seen

IFACE = 'org.putgd.GuiFuzz'
SHELL_STYLES = ('rgba(0, 0, 0, 0.43)', 'rgba(255, 255, 255, 0.92)', 'rgba(0, 0, 0, 0.78)', 'rgba(0, 0, 0, 0)')


def convert(raw):
    """The witness's JSON into the fence snapshot: windows bottom to top, put.gd's Shell actors, then the Shell's chrome on top."""
    # put.gd's selector has the keyboard and the pointer only while it is the top of Mutter's modal grab stack (the last entry): a Shell dialog,
    # a menu or the overview grabbed above it means the input goes there, and the fence denies.
    modal = (raw.get('chrome') or {}).get('modal_actors', [])
    grab = bool(modal) and modal[-1].endswith('RegionSelector')
    chrome_raw = raw.get('chrome') or {}
    foreign = bool(chrome_raw.get('overview')) or (bool(chrome_raw.get('modal')) and not grab)  # the input goes elsewhere: keys as well as the pointer are denied
    windows = []
    for w in raw['windows']:
        w = dict(w, content=w['frame'])
        if w.pop('hidden', False):
            w['min'] = True
        if grab or foreign:
            w['active'] = False
        windows.append(w)
    z = len(windows)
    selector = False
    for index, actor in enumerate(raw.get('actors', [])):
        style = actor.get('style', '')
        if actor['t'].endswith('RegionSelector'):
            selector = True
            windows.append(pseudo('sel%d' % index, 'putgd-shell-selector', actor['frame'], z, active=grab and not chrome_raw.get('overview')))
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
    if foreign:
        for index, output in enumerate(raw['outputs']):
            windows.append(pseudo('modal%d' % index, 'gnome-shell-overview' if chrome.get('overview') else 'gnome-shell-modal', output['geom'], top + 2, kind='ShellChrome'))
    return {'windows': windows, 'outputs': raw['outputs'], 'cursor': raw['cursor'], 'chrome': chrome}


class GnomeFence:
    """Same interface as the KWin fence: start(), stop(), get() -> (snapshot, time), serial."""

    def __init__(self):
        self.snapshot = {'windows': [], 'outputs': [], 'cursor': [0, 0], 'chrome': {}}
        self.stamp = 0.0
        self.serial = 0
        self.first_seen = {}
        self.errors = 0
        self.last_error = ''
        self.lock = threading.Lock()
        self.stopping = threading.Event()
        self.thread = None
        self.proxy = None

    def connect(self):
        bus = dbus.bus.BusConnection(dbus.bus.BUS_SESSION)
        self.proxy = dbus.Interface(bus.get_object('org.gnome.Shell', '/org/putgd/GuiFuzz', introspect=False), IFACE)

    def update(self, snapshot):
        with self.lock:
            stamp_first_seen(self.first_seen, self.serial + 1, snapshot['windows'])
            self.snapshot, self.stamp, self.serial = snapshot, time.time(), self.serial + 1

    def get(self):
        with self.lock:
            return self.snapshot, self.stamp

    def run(self):
        while not self.stopping.is_set():
            started = time.time()
            try:
                text = str(self.proxy.Snapshot(timeout=2))
                if text.startswith('ERROR'):
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
            raise RuntimeError('the GNOME witness never reported: ' + self.last_error)

    def stop(self):
        self.stopping.set()

    def clipboard(self, kind, payload=b''):
        return str(self.proxy.SetClipboard(kind, base64.b64encode(payload).decode(), timeout=5))

    def close(self, window_id):
        return str(self.proxy.Close(str(window_id), timeout=4))
