"""Every input primitive, fenced and logged. A replay re-issues the logged primitives unchanged."""
import json
import subprocess
import time

from gui_fuzz.uinput import KEYS, MODIFIERS
from gui_fuzz.world import run

# Keys the walk may press. Never Meta/Super, Alt+Tab, Alt+F4, Ctrl+Alt+anything, F-keys other than
# put.gd's own chords, power keys or Print: those cannot be built from this list.
PLAIN = {'esc', 'enter', 'tab', 'space', 'backspace', 'delete', 'home', 'end', 'pageup', 'pagedown', 'up', 'down', 'left', 'right'} \
    | set('abcdefghijklmnopqrstuvwxyz0123456789') | {'minus', 'equal', 'comma', 'dot', 'slash'}
CTRL_DENIED = {'q'}  # Ctrl+Q quits the resident, which would end the run by design
ALT_DENIED = {'tab', 'space', 'f4', 'esc', 'backspace', 'delete', 'enter'}
HOTKEYS = {'region': 'ctrl+shift+1', 'window': 'ctrl+shift+2', 'desktop': 'ctrl+shift+3', 'edit': 'ctrl+shift+4',
           'monitor': 'ctrl+shift+5', 'repeat': 'ctrl+shift+6', 'text': 'ctrl+shift+7', 'scroll': 'ctrl+shift+8',
           'record': 'ctrl+shift+9', 'clipboard': 'ctrl+shift+0'}
HOTKEY_CHORDS = set(HOTKEYS.values())
EDGE = 8  # keep clear of the desktop's screen edges and hot corners


class Denied(Exception):
    pass


def chord_allowed(chord, mine_focused):
    keys = chord.split('+')
    mods = [k for k in keys if k in MODIFIERS]
    base = [k for k in keys if k not in MODIFIERS]
    if len(base) != 1 or base[0] not in PLAIN | {'f%d' % i for i in range(1, 11)}:
        return False
    if chord in HOTKEY_CHORDS:
        return True
    if base[0].startswith('f') and len(base[0]) > 1:
        return False  # function keys only inside put.gd's own hotkeys, never freely
    if not mine_focused:
        return False
    if 'alt' in mods and (base[0] in ALT_DENIED or 'ctrl' in mods):
        return False
    if 'ctrl' in mods and base[0] in CTRL_DENIED:
        return False
    return True


class Actor:
    def __init__(self, dev, world, log, rng, replaying=False):
        self.dev, self.world, self.log, self.rng = dev, world, log, rng
        self.replaying = replaying
        self.denied = 0
        self.ops = 0

    def sleep(self, seconds):
        self.log.write({'op': 'sleep', 's': round(seconds, 3)})
        time.sleep(seconds)

    def pause(self, low=0.05, high=0.4):
        self.sleep(self.rng.uniform(low, high))

    # -- pointer ---------------------------------------------------------------------------------
    def sent(self):
        """Note the fence's snapshot number at the moment an input goes out: a window first seen in a later snapshot may be what it opened."""
        self.world.input_serials.append(getattr(self.world.fence, 'serial', 0))
        self.ops += 1

    def checked(self, what):
        """The pointer is where the fence says put.gd owns, read after the last input and 1 px inside the window; else the input is denied."""
        if self.replaying:
            return
        cursor = self.world.fresh_cursor()
        if cursor is None:
            self.denied += 1
            raise Denied('%s: no fresh snapshot of the pointer arrived in time' % what)
        x, y = cursor
        if not self.world.inside(x, y, margin=1):
            self.denied += 1
            raise Denied('%s with the pointer at %d,%d, which is not inside a put.gd window' % (what, x, y))

    def move(self, x, y, drag=False):
        """Jump the pointer to (x, y) inside the fence, closed loop on the compositor's own cursor position. The pointer's actual position
        is read again afterwards and fenced: a jump that landed on an edge or on something else leaves nothing pressed."""
        x, y = int(x), int(y)
        if not self.replaying:
            if not self.world.on_output(x, y) or not (drag or self.world.inside(x, y, margin=1)):
                self.denied += 1
                raise Denied('pointer target outside the fence: %d,%d' % (x, y))
        self.log.write({'op': 'move', 'x': x, 'y': y, 'drag': drag})
        for _ in range(8):
            cx, cy = self.world.cursor()
            dx, dy = x - cx, y - cy
            if abs(dx) <= 1 and abs(dy) <= 1:
                break
            self.sent()  # an input is recorded only when device events really go out: a move to where the pointer already is records nothing
            self.dev.rel(dx, dy)
            time.sleep(0.045)
        else:
            cx, cy = self.world.cursor()
            if not drag and (abs(cx - x) > 3 or abs(cy - y) > 3):
                self.missed = getattr(self, 'missed', 0) + 1  # the compositor clamped the jump (a gap between outputs): never click there
                raise Denied('pointer stopped at %d,%d, short of %d,%d' % (cx, cy, x, y))
        if not drag:
            self.checked('pointer moved')

    def button(self, name, pressed):
        if pressed:
            self.checked('button press')  # the target is checked again before every press, a multi-click's second and third included
        self.log.write({'op': 'btn', 'b': name, 'down': pressed})
        self.sent()
        self.dev.button(name, pressed)

    def click(self, x, y, button='left', count=1):
        self.move(x, y)
        for _ in range(count):
            self.button(button, True)
            time.sleep(self.rng.uniform(0.02, 0.09))
            self.button(button, False)
            time.sleep(self.rng.uniform(0.03, 0.12))

    def drag(self, points, button='left', hold=0.0):
        """Press at the first point, glide through the rest, release at the last."""
        self.move(*points[0])
        self.button(button, True)
        try:
            time.sleep(0.05 + hold)
            for point in points[1:]:
                self.move(point[0], point[1], drag=True)
                time.sleep(self.rng.uniform(0.012, 0.05))
        finally:
            self.button(button, False)

    def wheel(self, notches, horizontal=False):
        """One wheel event at a time: each is checked against the fence, logged as it is sent, and recorded."""
        step = 1 if notches > 0 else -1
        for _ in range(abs(notches)):
            self.checked('wheel')
            self.log.write({'op': 'wheel', 'n': step, 'h': horizontal})
            self.sent()
            self.dev.wheel(step, horizontal)

    # -- keys ------------------------------------------------------------------------------------
    def key_view(self):
        """The world as a snapshot taken after this call shows it, when keys may go anywhere at all: None when no such snapshot arrives within
        0.3 s, or while the GNOME overview is visible (it takes every key, put.gd's own hotkeys included)."""
        view = self.world.fresh_view()
        if view is None or (view.snapshot().get('chrome') or {}).get('overview'):
            return None
        return view

    @staticmethod
    def focused_in(view, desk):
        """Positive evidence from that snapshot: KDE counts the selector while it is up; GNOME and Cinnamon need a put.gd window with the focus, or the
        selector holding the Shell's modal grab or the X input focus (the fence marks it active)."""
        if not desk.strict_input and view.overlays():
            return True
        active = view.active()
        return bool(active and view.mine(active))

    def mine_focused(self):
        """Keys go to put.gd only on positive evidence from a snapshot taken after this call, within 0.3 s; with none, the key is denied and the
        cached active window is never used."""
        view = self.key_view()
        return view is not None and self.focused_in(view, self.world.desk)

    def quit_item(self):
        """Note that the next resident exit is the Quit item the actor activated."""
        self.log.last_quit_at = time.time()
        self.log.write({'op': 'note', 'action': 'quit item'})

    def chord(self, text, hold=0.03):
        if not self.replaying:
            view = self.key_view()  # without a fresh snapshot, or under the overview, not even put.gd's global hotkeys go out
            if view is None or not chord_allowed(text, self.focused_in(view, self.world.desk)):
                self.denied += 1
                raise Denied('key chord not allowed here: ' + text)
        keys = text.split('+')
        ordered = [k for k in keys if k in MODIFIERS] + [k for k in keys if k not in MODIFIERS]
        self.log.write({'op': 'chord', 'k': text})
        self.sent()
        for key in ordered:
            self.dev.key(key, True)
            time.sleep(0.004)
        time.sleep(hold)
        for key in reversed(ordered):
            self.dev.key(key, False)

    def text(self, value):
        if not self.replaying and not self.mine_focused():
            self.denied += 1
            raise Denied('typing needs put.gd focus')
        self.log.write({'op': 'text', 'v': value})
        for char in value:
            lower = char.lower()
            name = {' ': 'space', '-': 'minus', '=': 'equal', ',': 'comma', '.': 'dot', '/': 'slash'}.get(char, lower)
            if name not in KEYS:
                continue
            self.sent()
            if char.isupper():
                self.dev.key('shift', True)
            self.dev.key(name, True)
            self.dev.key(name, False)
            if char.isupper():
                self.dev.key('shift', False)
            time.sleep(self.rng.uniform(0.005, 0.03))

    def release_all(self):
        self.log.write({'op': 'release_all'})
        for key in list(self.dev.down):
            if key.startswith('btn-'):
                self.dev.button(key[4:], False)
            else:
                self.dev.key(key, False)

    # -- session methods and desktop helpers -----------------------------------------------------
    def cli(self, method, params=None, timeout=12):
        self.log.write({'op': 'cli', 'm': method, 'p': params})
        args = ['putgd-cli', method] + ([json.dumps(params)] if params is not None else [])
        started = time.time()
        try:
            done = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
            try:
                reply = json.loads(done.stdout)
            except ValueError:
                reply = {'ok': False, '_raw': done.stdout[-300:] + done.stderr[-300:], '_rc': done.returncode}
        except subprocess.TimeoutExpired:
            reply = {'ok': False, '_timeout': True}
        reply['_ms'] = int((time.time() - started) * 1000)
        return reply

    def sh(self, *args, timeout=15):
        self.log.write({'op': 'sh', 'a': list(args)})
        return run(list(args), timeout)
