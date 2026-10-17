#!/usr/bin/env python3
"""QMP-compatible input agent for physical lab desktops (stdlib only, Linux).

The lab drives VMs through QMP `input-send-event`. Physical desktops have no
QMP, so this agent creates kernel uinput devices (a keyboard and an absolute
pointer modelled on QEMU's usb-tablet) and accepts the same QMP subset on an
owner-only Unix socket. Events reach the compositor through libinput exactly
like hardware input, so global shortcuts, selectors and editors see real input.

Start it with root only to open /dev/uinput; it then drops to --user before
listening. Forward the socket over pinned SSH and point the runner's
`qmp.sock` at it. Screenshots are not provided: capture frames separately.

    sudo -n python3 tools/lab/host_input.py serve --socket /run/user/1000/putgd-lab-input.sock --user patrick
"""
import argparse, fcntl, json, os, pwd, socket, struct, sys, time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SYN_REPORT, ABS_X, ABS_Y = 0, 0, 1
BTN = {'left': 0x110, 'right': 0x111, 'middle': 0x112}
ABS_MAX = 32767  # QMP absolute axis range used by the lab runner
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x401c5504, 0x5501, 0x5502
BUS_VIRTUAL = 0x06

# QEMU qcode -> Linux evdev keycode (US layout positions).
QCODES = {
    'esc': 1, 'minus': 12, 'equal': 13, 'backspace': 14, 'tab': 15, 'bracket_left': 26, 'bracket_right': 27,
    'ret': 28, 'ctrl': 29, 'semicolon': 39, 'apostrophe': 40, 'grave_accent': 41, 'shift': 42, 'backslash': 43,
    'comma': 51, 'dot': 52, 'slash': 53, 'shift_r': 54, 'kp_multiply': 55, 'alt': 56, 'spc': 57, 'caps_lock': 58,
    'ctrl_r': 97, 'alt_r': 100, 'home': 102, 'up': 103, 'pgup': 104, 'left': 105, 'right': 106, 'end': 107,
    'down': 108, 'pgdn': 109, 'insert': 110, 'delete': 111, 'meta_l': 125, 'meta_r': 126, 'menu': 127,
    'f11': 87, 'f12': 88,
    **{c: code for c, code in zip('1234567890', range(2, 12))},
    **{c: code for c, code in zip('qwertyuiop', range(16, 26))},
    **{c: code for c, code in zip('asdfghjkl', range(30, 39))},
    **{c: code for c, code in zip('zxcvbnm', range(44, 51))},
    **{f'f{i}': 58 + i for i in range(1, 11)},
}


def validate(events):
    """Return a list of (type, code, value) tuples or raise ValueError.

    Accepts exactly the QMP input-send-event shapes the runner emits."""
    if not isinstance(events, list) or not 0 < len(events) <= 64:
        raise ValueError('events must be a list of 1-64 entries')
    out = []
    for event in events:
        if not isinstance(event, dict) or set(event) != {'type', 'data'} or not isinstance(event['data'], dict):
            raise ValueError('event must have type and data')
        kind, data = event['type'], event['data']
        if kind == 'key':
            key = data.get('key', {})
            if set(data) != {'down', 'key'} or not isinstance(data['down'], bool) or key.get('type') != 'qcode' or key.get('data') not in QCODES:
                raise ValueError('unsupported key event')
            out.append((EV_KEY, QCODES[key['data']], int(data['down'])))
        elif kind == 'btn':
            if set(data) != {'down', 'button'} or not isinstance(data['down'], bool) or data['button'] not in BTN:
                raise ValueError('unsupported button event')
            out.append((EV_KEY, BTN[data['button']], int(data['down'])))
        elif kind == 'abs':
            value = data.get('value')
            if set(data) != {'axis', 'value'} or data['axis'] not in ('x', 'y') or type(value) is not int or not 0 <= value <= ABS_MAX:
                raise ValueError('unsupported absolute event')
            out.append((EV_ABS, ABS_X if data['axis'] == 'x' else ABS_Y, value))
        else:
            raise ValueError('unsupported event type')
    return out


class Device:
    def __init__(self, name, keys, absolute=False):
        self.fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for code in keys:
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        if absolute:
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
            for axis in (ABS_X, ABS_Y):
                fcntl.ioctl(self.fd, UI_SET_ABSBIT, axis)
                # struct uinput_abs_setup: u16 code, pad, input_absinfo(value,min,max,fuzz,flat,resolution)
                fcntl.ioctl(self.fd, UI_ABS_SETUP, struct.pack('<HxxiiiiII', axis, 0, 0, ABS_MAX, 0, 0, 0))
        # struct uinput_setup: input_id(bustype,vendor,product,version), char name[80], u32 ff_effects_max
        fcntl.ioctl(self.fd, UI_DEV_SETUP, struct.pack('<HHHH80sI', BUS_VIRTUAL, 0x1209, 0x5047, 1, name.encode()[:79], 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)

    def emit(self, events):
        now = time.time()
        sec, usec = int(now), int((now % 1) * 1e6)
        payload = b''.join(struct.pack('llHHi', sec, usec, t, c, v) for t, c, v in events)
        os.write(self.fd, payload + struct.pack('llHHi', sec, usec, EV_SYN, SYN_REPORT, 0))

    def close(self):
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        finally:
            os.close(self.fd)


def serve(path, user):
    keyboard = Device('put.gd lab keyboard', sorted(set(QCODES.values())))
    tablet = Device('put.gd lab tablet', sorted(BTN.values()), absolute=True)
    account = pwd.getpwnam(user)
    if os.getuid() == 0:
        os.setgroups([]); os.setgid(account.pw_gid); os.setuid(account.pw_uid)
    if os.getuid() != account.pw_uid:
        raise SystemExit('agent must run as the desktop user after opening uinput')
    os.umask(0o077)
    if os.path.lexists(path):
        os.unlink(path)
    server = socket.socket(socket.AF_UNIX)
    server.bind(path); os.chmod(path, 0o600); server.listen(4)
    time.sleep(.5)  # let the compositor enumerate the new devices
    print(json.dumps({'listening': path, 'devices': ['put.gd lab keyboard', 'put.gd lab tablet']}), flush=True)
    try:
        while True:
            connection, _ = server.accept()
            with connection, connection.makefile('rwb') as stream:
                handle(stream, keyboard, tablet)
    finally:
        keyboard.close(); tablet.close(); server.close(); os.unlink(path)


def handle(stream, keyboard, tablet):
    def send(message):
        stream.write(json.dumps(message).encode() + b'\n'); stream.flush()
    send({'QMP': {'version': {'qemu': {'major': 0, 'minor': 0, 'micro': 0}, 'package': 'put.gd host_input'}, 'capabilities': []}})
    for line in stream:
        ident = None
        try:
            request = json.loads(line)
            if not isinstance(request, dict):
                raise ValueError('request must be an object')
            name, ident = request.get('execute'), request.get('id')
            if name == 'qmp_capabilities':
                send({'return': {}, 'id': ident}); continue
            if name != 'input-send-event' or set(request.get('arguments', {})) != {'events'}:
                send({'error': {'class': 'CommandNotFound', 'desc': 'only input-send-event is supported'}, 'id': ident}); continue
            events = validate(request['arguments']['events'])
            is_key = lambda e: e[0] == EV_KEY and e[1] not in BTN.values()
            keys = [e for e in events if is_key(e)]
            pointer = [e for e in events if not is_key(e)]
            if keys: keyboard.emit(keys)
            if pointer: tablet.emit(pointer)
            send({'return': {}, 'id': ident})
        except (ValueError, json.JSONDecodeError) as error:
            send({'error': {'class': 'GenericError', 'desc': str(error)}, 'id': ident})


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='action', required=True)
    run = sub.add_parser('serve')
    run.add_argument('--socket', required=True)
    run.add_argument('--user', required=True)
    args = parser.parse_args()
    serve(args.socket, args.user)


if __name__ == '__main__':
    main()
