"""The seeded random walk: real entry points first, then clicks, drags, keys, windows, monitors, clipboard."""
import base64
import random
import re
import struct
import subprocess
import time
import zlib

import dbus

from gui_fuzz.actor import EDGE, HOTKEYS, Denied

WORDS = ['hello', 'put.gd', 'Screen Capture 2026', 'a' * 120, 'ünïcode ✓ 日本語', '0123456789', 'the quick brown fox', '', 'x', 'Ctrl', 'redact me']
CHORDS_IN_APP = ['ctrl+a', 'ctrl+c', 'ctrl+v', 'ctrl+x', 'ctrl+z', 'ctrl+shift+z', 'ctrl+y', 'ctrl+s', 'ctrl+f', 'ctrl+0', 'ctrl+1', 'ctrl+equal',
                 'ctrl+minus', 'ctrl+enter', 'ctrl+shift+c', 'ctrl+w', 'ctrl+n', 'ctrl+o', 'ctrl+tab', 'ctrl+shift+tab', 'tab', 'shift+tab',
                 'enter', 'space', 'esc', 'delete', 'backspace', 'left', 'right', 'up', 'down', 'home', 'end', 'pageup', 'pagedown',
                 'shift+left', 'shift+right', 'shift+up', 'shift+down', 'alt+left', 'alt+right', 'f2', 'shift+delete'] \
    + list('abcdefghijklmnoprstuvwxyz0123456789')
FIRST_NO_F = [c for c in CHORDS_IN_APP if not c.startswith('f')]
ACTIONS = ['region', 'window', 'monitor', 'desktop', 'repeat', 'clipboard', 'edit', 'text']


def png(width=96, height=64):
    rows = b''.join(b'\x00' + bytes(v for x in range(width) for v in ((x * 3 + y * 5) % 256, (x * 7) % 256, (y * 11) % 256)) for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


class Walker:
    def __init__(self, actor, world, watch, findings, rng, log, checkpoint_every=(18, 40), chaos=False):
        self.chaos = chaos
        self.a, self.w, self.watch, self.findings, self.rng, self.log = actor, world, watch, findings, rng, log
        self.bus = dbus.SessionBus(private=True)
        self.counts = {}
        self.checkpoint_every = checkpoint_every
        self.since_checkpoint = 0
        self.next_checkpoint = rng.randint(*checkpoint_every)
        self.last = None
        self.harness_errors = 0
        self.screens_off = False

    # -- helpers ---------------------------------------------------------------------------------
    def rand_point(self, window, margin=6):
        """A point inside the window and on an output (a window can hang over a monitor edge)."""
        x, y, w, h = window['frame']
        point = (x + margin, y + margin)
        for _ in range(12):
            point = (self.rng.randint(x + margin, max(x + margin, x + w - margin - 1)), self.rng.randint(y + margin, max(y + margin, y + h - margin - 1)))
            if self.w.on_output(*point) and self.w.inside(*point):
                break
        return point

    def rand_window(self):
        windows = [w for w in self.w.windows() if not w['popup'] or self.rng.random() < 0.5]
        return self.rng.choice(windows) if windows else None

    def rand_screen_point(self):
        out = self.rng.choice(self.w.outputs())
        x, y, w, h = out['geom']
        return (self.rng.randint(x + EDGE, x + w - EDGE - 1), self.rng.randint(y + EDGE, y + h - EDGE - 1))

    def cli_check(self, reply, what):
        if reply.get('_timeout'):
            self.findings.add('cli_timeout', '%s did not answer in 12 s' % what)
        error = (reply.get('error') or {}).get('code', '')
        if error and re.search(r'internal|exception|crash|unexpected|assert|bad_alloc', error, re.I):
            self.findings.add('cli_internal_error', '%s -> %s' % (what, error), extra=reply)
        return reply

    # -- actions ---------------------------------------------------------------------------------
    def hotkey(self, name=None):
        name = name or self.rng.choice(list(HOTKEYS))
        self.log.write({'op': 'note', 'action': 'hotkey', 'name': name})
        self.a.chord(HOTKEYS[name])
        self.a.pause(0.2, 0.9)

    def click(self):
        window = self.rand_window()
        if not window:
            raise Denied('no window')
        x, y = self.rand_point(window)
        self.a.click(x, y, self.rng.choice(['left'] * 6 + ['right', 'middle']), self.rng.choice([1, 1, 1, 2, 3]))

    def drag(self):
        window = self.rand_window()
        if not window:
            raise Denied('no window')
        x, y = self.rand_point(window)
        n = self.rng.randint(3, 16)
        ex, ey = self.rand_point(window if self.rng.random() < 0.8 else (self.rand_window() or window))
        points = [(x + (ex - x) * i // n + self.rng.randint(-3, 3), y + (ey - y) * i // n + self.rng.randint(-3, 3)) for i in range(n + 1)]
        self.a.drag(points, self.rng.choice(['left'] * 5 + ['right']), hold=self.rng.choice([0, 0, 0.3]))

    def wheel(self):
        window = self.rand_window()
        if not window:
            raise Denied('no window')
        self.a.move(*self.rand_point(window))
        modifier = self.rng.choice([None, None, None, 'ctrl', 'shift'])
        if modifier:
            self.a.dev.key(modifier, True)
        try:
            self.a.wheel(self.rng.choice([-1, 1]) * self.rng.randint(1, 8), horizontal=self.rng.random() < 0.15)
        finally:
            if modifier:
                self.a.dev.key(modifier, False)

    def chord(self):
        self.a.chord(self.rng.choice(FIRST_NO_F))

    def hold(self):
        """A key held long enough for the toolkit's auto-repeat, then released."""
        self.a.chord(self.rng.choice(['left', 'right', 'up', 'down', 'tab', 'backspace', 'a', 'pagedown', 'delete', 'space']), hold=self.rng.uniform(0.5, 1.6))

    def spam(self):
        key = self.rng.choice(['esc', 'esc', 'enter', 'tab', 'space'])
        for _ in range(self.rng.randint(8, 25)):
            self.a.chord(key, hold=0.005)
            time.sleep(self.rng.uniform(0, 0.03))

    def tab_walk(self):
        for _ in range(self.rng.randint(3, 14)):
            self.a.chord(self.rng.choice(['tab', 'shift+tab']))
            time.sleep(0.05)
        self.a.chord(self.rng.choice(['space', 'enter']))

    def typing(self):
        self.a.text(self.rng.choice(WORDS))

    def escape(self):
        self.a.chord('esc')

    def overlay_drag(self):
        p1, p2 = self.rand_screen_point(), self.rand_screen_point()
        n = self.rng.randint(3, 14)
        points = [(p1[0] + (p2[0] - p1[0]) * i // n, p1[1] + (p2[1] - p1[1]) * i // n) for i in range(n + 1)]
        mid = self.rng.random()
        if mid < 0.12:  # Escape in the middle of the drag
            self.a.move(*points[0])
            self.a.button('left', True)
            try:
                for point in points[1:n // 2 + 1]:
                    self.a.move(point[0], point[1], drag=True)
                    time.sleep(0.02)
                self.a.chord('esc')
            finally:
                self.a.button('left', False)
        elif mid < 0.2:  # the other button joins in
            self.a.move(*points[0])
            self.a.button('left', True)
            try:
                for point in points[1:]:
                    self.a.move(point[0], point[1], drag=True)
                self.a.click(*points[-1], button='right')
            finally:
                self.a.button('left', False)
        else:
            self.a.drag(points)
        self.a.pause(0.1, 0.8)

    def overlay_misc(self):
        choice = self.rng.randint(0, 5)
        if choice == 0:
            self.a.chord(self.rng.choice(['left', 'right', 'up', 'down', 'shift+left', 'shift+up', 'enter', 'tab', 'space']))
        elif choice == 1:
            self.a.click(*self.rand_screen_point(), count=self.rng.choice([1, 2]))
        elif choice == 2:
            self.a.move(*self.rand_screen_point())
            self.a.wheel(self.rng.choice([-1, 1]) * self.rng.randint(1, 5))
        elif choice == 3:
            self.a.click(*self.rand_screen_point(), button='right')
        elif choice == 4:
            self.a.move(*self.rand_screen_point())
        else:
            self.hotkey(self.rng.choice(list(HOTKEYS)))  # another hotkey while the selector is open

    def tray(self):
        names = [n for n in self.bus.list_names() if n.startswith('org.kde.StatusNotifierItem.putgd')]
        if not names:
            raise Denied('no put.gd tray item')
        item = self.bus.get_object(names[0], '/StatusNotifierItem', introspect=False)
        path = item.Get('org.kde.StatusNotifierItem', 'Menu', dbus_interface='org.freedesktop.DBus.Properties')
        menu = dbus.Interface(self.bus.get_object(names[0], path, introspect=False), 'com.canonical.dbusmenu')
        menu.AboutToShow(0)
        _, layout = menu.GetLayout(0, -1, dbus.Array([], signature='s'))
        leaves = []

        def walk(node):
            ident, props, children = node
            label = str(props.get('label', ''))
            if children:
                for child in children:
                    walk(child)
            elif ident and props.get('enabled', True) and props.get('visible', True) and props.get('type', 'standard') == 'standard' \
                    and not re.search(r'quit|exit', label, re.I):
                leaves.append((int(ident), label))
        walk(layout)
        if not leaves:
            raise Denied('empty tray menu')
        ident, label = self.rng.choice(leaves)
        self.log.write({'op': 'note', 'action': 'tray', 'label': label})
        menu.Event(ident, 'clicked', dbus.String('', variant_level=1), dbus.UInt32(int(time.time())), timeout=15)

    def cli_action(self):
        kind = self.rng.randint(0, 6)
        if kind <= 2:
            params = {'action': self.rng.choice(ACTIONS)}
            if params['action'] != 'clipboard' and self.rng.random() < 0.25:
                params['delaySeconds'] = 3
            self.cli_check(self.a.cli('action.invoke', params, timeout=55), 'action.invoke ' + params['action'])
        elif kind == 3:
            self.cli_check(self.a.cli('editor.open', {'source': self.rng.choice(['desktop', 'clipboard', 'region'])}), 'editor.open')
        elif kind == 4:
            self.cli_check(self.a.cli('history.list', {'limit': self.rng.choice([1, 20, 200])}), 'history.list')
        elif kind == 5:
            self.cli_check(self.a.cli('recording.capabilities'), 'recording.capabilities')
        else:
            self.cli_check(self.a.cli('operation.recent'), 'operation.recent')

    def window_geometry(self):
        window = self.rand_window()
        if not window or window['popup'] or window['cls'] == 'putgd-region':
            raise Denied('no window')
        x, y, w, h = window['frame']
        if self.rng.random() < 0.5:  # resize from the bottom-right corner
            self.a.drag([(x + w - 2, y + h - 2), (x + w + self.rng.randint(-200, 200), y + h + self.rng.randint(-150, 150))])
        else:  # move by the title bar
            self.a.drag([(x + w // 2, y + 8), (x + w // 2 + self.rng.randint(-250, 250), y + 8 + self.rng.randint(-60, 120))])

    def outputs(self):
        names = [o['name'] for o in self.w.outputs()]
        choice = self.rng.randint(0, 3)
        if choice == 0 and 'Virtual-2' in names or (choice == 1 and 'Virtual-2' not in names):
            self.a.sh('kscreen-doctor', 'output.Virtual-2.disable')
        elif choice == 1:
            self.a.sh('kscreen-doctor', 'output.Virtual-2.enable')
        elif choice == 2:
            self.a.sh('kscreen-doctor', 'output.Virtual-1.scale.%s' % self.rng.choice(['1', '1.25', '1.5', '2']))
        else:
            self.a.sh('kscreen-doctor', 'output.Virtual-2.enable')
            self.a.sh('kscreen-doctor', 'output.Virtual-2.scale.%s' % self.rng.choice(['1', '1.5']))
        self.a.sleep(1.5)
        self.close_gap()

    def close_gap(self):
        """Keep the two outputs side by side: a gap between them stops the pointer at the edge of the first."""
        outs = {o['name']: o['geom'] for o in self.w.outputs()}
        if 'Virtual-1' in outs and 'Virtual-2' in outs and outs['Virtual-2'][0] != outs['Virtual-1'][2]:
            self.a.sh('kscreen-doctor', 'output.Virtual-2.position.%d,0' % outs['Virtual-1'][2])
            self.a.sleep(1.0)

    def clipboard(self):
        choice = self.rng.randint(0, 3)
        if choice == 0:
            self.a.sh('busctl', '--user', 'call', 'org.kde.klipper', '/klipper', 'org.kde.klipper.klipper', 'setClipboardContents', 's', self.rng.choice(WORDS) or 'x')
        elif choice == 1:
            self.log.write({'op': 'note', 'action': 'wl-copy image'})
            subprocess.run(['wl-copy', '--type', 'image/png'], input=png(self.rng.choice([32, 96, 640]), self.rng.choice([32, 64, 360])),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        elif choice == 2:
            self.log.write({'op': 'note', 'action': 'wl-copy uri-list'})
            subprocess.run(['wl-copy', '--type', 'text/uri-list'], input=b'file:///etc/hostname\r\n', stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        else:
            self.a.sh('wl-copy', '--clear')

    def rapid(self):
        if not self.last:
            raise Denied('nothing to repeat')
        name, action = self.last
        for _ in range(self.rng.randint(2, 6)):
            self.log.write({'op': 'note', 'action': 'rapid ' + name})
            action()
            time.sleep(self.rng.uniform(0, 0.04))

    # -- the walk --------------------------------------------------------------------------------
    def table(self):
        table = self.base_table()
        if self.chaos:  # monitors coming and going and rapid repeats dominate, with the pauses cut short
            return [(f, w * 6 if f in (self.outputs, self.rapid, self.spam) else w) for f, w in table]
        return table

    def base_table(self):
        if self.w.overlays():
            return [(self.overlay_drag, 30), (self.overlay_misc, 22), (self.escape, 10), (self.outputs, 3), (self.cli_action, 3), (self.clipboard, 2),
                    (self.rapid, 3), (self.hotkey, 4)]
        if self.w.windows():
            return [(self.hotkey, 16), (self.click, 18), (self.drag, 8), (self.wheel, 5), (self.chord, 9), (self.typing, 6), (self.escape, 4), (self.hold, 3), (self.spam, 2), (self.tab_walk, 3),
                    (self.tray, 7), (self.cli_action, 6), (self.window_geometry, 5), (self.outputs, 3), (self.clipboard, 3), (self.rapid, 4)]
        return [(self.hotkey, 24), (self.tray, 20), (self.cli_action, 12), (self.clipboard, 3), (self.outputs, 3)]

    def step(self):
        actions = self.table()
        action = self.rng.choices([f for f, _ in actions], [w for _, w in actions])[0]
        name = action.__name__
        self.log.step += 1
        self.log.write({'op': 'step', 'action': name, 'windows': len(self.w.windows()), 'overlay': len(self.w.overlays())})
        try:
            action()
            if name not in ('rapid',):
                self.last = (name, action)
            self.counts[name] = self.counts.get(name, 0) + 1
        except Denied as why:
            self.counts['denied'] = self.counts.get('denied', 0) + 1
            self.log.write({'op': 'denied', 'why': str(why)})
        except (dbus.DBusException, KeyError, IndexError, OSError, subprocess.SubprocessError, ValueError) as error:
            self.harness_errors += 1
            self.log.write({'op': 'harness_error', 'action': name, 'error': repr(error)[:200]})
        self.a.pause(0.0, 0.12) if self.chaos else self.a.pause(0.05, 0.5)
        self.since_checkpoint += 1
        if self.since_checkpoint >= self.next_checkpoint:
            self.checkpoint()

    def checkpoint(self):
        """Quiet the session and check that nothing is left grabbed, hung or inconsistent."""
        self.since_checkpoint = 0
        self.next_checkpoint = self.rng.randint(*self.checkpoint_every)
        self.log.write({'op': 'checkpoint'})
        self.a.release_all()
        caps = self.a.cli('recording.capabilities').get('result') or {}
        if caps.get('active'):  # a ready or running recording legitimately keeps its selector and bar up
            self.cli_check(self.a.cli('recording.cancel', {'id': caps['active']}), 'recording.cancel')
            time.sleep(1.0)
        recent = (self.a.cli('operation.recent').get('result') or {}).get('operations') or []
        if self.rng.random() < 0.6:  # an edit left open blocks every capture: its rejection names the operation to cancel
            probe = self.a.cli('action.invoke', {'action': 'edit'}, timeout=20)
            error = probe.get('error') or {}
            if error.get('code') == 'editor_active' and (error.get('details') or {}).get('operationId'):
                self.cli_check(self.a.cli('editor.cancel', {'id': error['details']['operationId']}), 'editor.cancel')
                time.sleep(1.0)
            else:
                time.sleep(2.0)  # the probe started an edit selector: let it open so the Escape below closes it
        for item in recent:  # a scroll session waiting for Start keeps its selector and bar up, as a recording does
            if item.get('action') == 'scroll' and item.get('state') not in ('completed', 'failed', 'cancelled'):
                self.cli_check(self.a.cli('scroll.cancel', {'id': item['id']}), 'scroll.cancel')
                time.sleep(1.0)
        for dialog in [x for x in self.w.windows() if x.get('tr') or x['cls'].startswith('org.freedesktop.impl.portal')][:4]:  # file choosers the walk opened and left behind
            try:
                self.a.click(*self.rand_point(dialog))
                self.a.chord('esc')
            except Denied:
                pass
            time.sleep(0.6)
        for _ in range(3):
            if self.w.overlays() or self.w.popups():
                try:
                    self.a.chord('esc')
                except Denied:
                    pass
                time.sleep(0.8)
        time.sleep(2.0)
        for problem in self.watch.settle_checks(self.a):
            helper = [open('/proc/%d/cmdline' % p, 'rb').read().replace(b'\0', b' ').decode()[:200] for p, c in self.w.pids().items() if c == 'putgd-record']
            self.findings.add('stuck_grab', problem, extra={'helpers': helper, 'recording': (self.a.cli('recording.capabilities').get('result') or {}).get('active'),
                                                            'operations': [o.get('action') + ':' + o.get('state', '') for o in ((self.a.cli('operation.recent').get('result') or {}).get('operations') or [])[:5]]})
        names = [o['name'] for o in self.w.outputs()]
        if 'Virtual-2' not in names:  # keep two monitors for most of the run
            self.a.sh('kscreen-doctor', 'output.Virtual-2.enable')
            self.a.sleep(1.5)
        self.close_gap()
        counts = self.cli_check(self.a.cli('history.counts'), 'history.counts')
        listing = self.cli_check(self.a.cli('history.list', {'limit': 200}), 'history.list')
        self.consistency(counts, listing)
        settings = self.cli_check(self.a.cli('settings.get'), 'settings.get')
        meta = (settings.get('result') or {}).get('metadata') or {}
        if meta and meta.get('acceptedRevision') != meta.get('observedRevision'):
            self.findings.add('settings_inconsistent', 'accepted %s observed %s' % (meta.get('acceptedRevision'), meta.get('observedRevision')))
        operations = self.cli_check(self.a.cli('operation.recent'), 'operation.recent')
        for item in (operations.get('result') or {}).get('operations', []) if isinstance(operations.get('result'), dict) else []:
            if item.get('state') in ('running', 'acquiring', 'uploading') and item.get('ageSeconds', 0) > 180:
                self.findings.add('operation_stuck', '%s %s for %ss' % (item.get('action'), item.get('state'), item.get('ageSeconds')))

    def recover(self):
        """Focus has been away from put.gd: give it back with an Escape or a click inside a put.gd window."""
        self.recoveries = getattr(self, 'recoveries', 0) + 1
        self.log.write({'op': 'note', 'action': 'recover', 'n': self.recoveries})
        if self.recoveries > 6:
            self.findings.add('focus_unrecoverable', 'focus would not return to put.gd after %d tries' % self.recoveries)
            self.recoveries = 0
            self.watch.paused_walk.clear()
            return
        try:
            if self.w.overlays():
                self.a.chord('esc')
            else:
                window = next(iter(self.w.toplevels()), None)
                if window:
                    self.a.click(*self.rand_point(window))
        except Denied:
            pass
        except (OSError, KeyError, IndexError):
            self.harness_errors += 1

    def consistency(self, counts, listing):
        result = counts.get('result') or {}
        entries = (listing.get('result') or {}).get('records') or []
        total = result.get('total', result.get('all'))
        if isinstance(total, int) and isinstance(entries, list) and total <= 200 and total != len(entries):
            self.findings.add('history_inconsistent', 'history.counts total %s but history.list returned %s' % (total, len(entries)), extra={'counts': result})
