"""A uinput virtual keyboard and relative mouse (stdlib, Linux; needs write access to /dev/uinput)."""
import fcntl
import os
import struct
import subprocess
import time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = 0x40045564, 0x40045565, 0x40045566
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x5501, 0x5502
EV_SYN, EV_KEY, EV_REL = 0, 1, 2
REL_X, REL_Y, REL_HWHEEL, REL_WHEEL, REL_WHEEL_HI_RES = 0, 1, 6, 8, 11
BTN = {'left': 0x110, 'right': 0x111, 'middle': 0x112}

KEYS = {'esc': 1, 'backspace': 14, 'tab': 15, 'enter': 28, 'space': 57, 'delete': 111, 'home': 102, 'end': 107,
        'pageup': 104, 'pagedown': 109, 'up': 103, 'down': 108, 'left': 105, 'right': 106,
        'ctrl': 29, 'shift': 42, 'alt': 56, 'minus': 12, 'equal': 13, 'comma': 51, 'dot': 52, 'slash': 53}
for _i, _c in enumerate('1234567890'):
    KEYS[_c] = 2 + _i
for _c, _code in zip('qwertyuiop', range(16, 26)):
    KEYS[_c] = _code
for _c, _code in zip('asdfghjkl', range(30, 39)):
    KEYS[_c] = _code
for _c, _code in zip('zxcvbnm', range(44, 51)):
    KEYS[_c] = _code
for _i in range(1, 11):
    KEYS['f%d' % _i] = 58 + _i
MODIFIERS = ('ctrl', 'shift', 'alt')


def _create(name, keys, rels):
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    for code in keys:
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    if rels:
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_REL)
        for code in rels:
            fcntl.ioctl(fd, UI_SET_RELBIT, code)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_SYN)
    setup = struct.pack('<HHHH80sI', 0x03, 0x1209, 0x7067 + len(rels), 1, name.encode(), 0)
    fcntl.ioctl(fd, UI_DEV_SETUP, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd


def _event(fd, kind, code, value):
    os.write(fd, struct.pack('<qqHHi', 0, 0, kind, code, value))


class Devices:
    """Keyboard and mouse; every call is one frame of real input events."""

    def __init__(self, tag='gfz'):
        self.kb = _create(tag + ' keyboard', sorted(set(KEYS.values())), [])
        self.mouse = _create(tag + ' mouse', list(BTN.values()), [REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES])
        self.name = tag + ' mouse'
        self.down = set()
        time.sleep(0.8)  # let udev and the compositor pick the devices up

    def close(self):
        for key in list(self.down):
            self.key(key, False)
        for fd in (self.kb, self.mouse):
            try:
                fcntl.ioctl(fd, UI_DEV_DESTROY)
            except OSError:
                pass
            os.close(fd)

    def key(self, name, pressed):
        code = KEYS[name]
        _event(self.kb, EV_KEY, code, 1 if pressed else 0)
        _event(self.kb, EV_SYN, 0, 0)
        (self.down.add if pressed else self.down.discard)(name)

    def button(self, name, pressed):
        _event(self.mouse, EV_KEY, BTN[name], 1 if pressed else 0)
        _event(self.mouse, EV_SYN, 0, 0)
        key = 'btn-' + name
        (self.down.add if pressed else self.down.discard)(key)

    def rel(self, dx, dy):
        if dx:
            _event(self.mouse, EV_REL, REL_X, int(dx))
        if dy:
            _event(self.mouse, EV_REL, REL_Y, int(dy))
        _event(self.mouse, EV_SYN, 0, 0)

    def wheel(self, notches, horizontal=False):
        step = 1 if notches > 0 else -1
        for _ in range(abs(notches)):
            if not horizontal:
                _event(self.mouse, EV_REL, REL_WHEEL_HI_RES, 120 * step)
            _event(self.mouse, EV_REL, REL_HWHEEL if horizontal else REL_WHEEL, step)
            _event(self.mouse, EV_SYN, 0, 0)
            time.sleep(0.012)

    def flatten_acceleration(self):
        """Make REL units pixels: KWin's flat pointer acceleration profile for our mouse."""
        try:
            tree = subprocess.run(['busctl', '--user', 'tree', 'org.kde.KWin', '--list'], capture_output=True, text=True, timeout=10).stdout
            for path in tree.split():
                if '/org/kde/KWin/InputDevice/event' not in path:
                    continue
                name = subprocess.run(['busctl', '--user', 'get-property', 'org.kde.KWin', path, 'org.kde.KWin.InputDevice', 'name'],
                                      capture_output=True, text=True, timeout=10).stdout
                if self.name in name:
                    subprocess.run(['busctl', '--user', 'set-property', 'org.kde.KWin', path, 'org.kde.KWin.InputDevice',
                                    'pointerAccelerationProfileFlat', 'b', 'true'], capture_output=True, timeout=10)
                    return path
        except (OSError, subprocess.SubprocessError):
            pass
        return None
