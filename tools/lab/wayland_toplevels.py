#!/usr/bin/env python3
"""List a Wayland compositor's toplevel windows and the active one.

A lab witness for compositors with no scripting interface (labwc, Mir): a
minimal Wayland client written against the wire protocol, standard library
only, so it runs in any guest. It binds ext-foreign-toplevel-list-v1 (the
identifiers capture uses) and zwlr-foreign-toplevel-management-v1 (the only
list with an activated state), reads one complete snapshot and prints JSON:

  {"ext": [{"identifier", "app_id", "title"}],
   "wlr": [{"app_id", "title", "activated", "minimized", "maximized", "fullscreen"}],
   "active": {"identifier", "app_id", "title"} | null, "protocols": {name: version}}

Neither protocol reports a PID or geometry; the witness says what it can.
"""
import argparse
import json
import os
import socket
import struct
import sys

WLR_STATES = {0: 'maximized', 1: 'minimized', 2: 'activated', 3: 'fullscreen'}


def pad(length):
    return (length + 3) & ~3


def encode_string(text):
    data = text.encode() + b'\0'
    return struct.pack('<I', len(data)) + data + b'\0' * (pad(len(data)) - len(data))


def message(object_id, opcode, payload=b''):
    return struct.pack('<II', object_id, ((8 + len(payload)) << 16) | opcode) + payload


class Reader:
    def __init__(self, payload):
        self.data, self.at = payload, 0

    def uint(self):
        value = struct.unpack_from('<I', self.data, self.at)[0]
        self.at += 4
        return value

    def string(self):
        length = self.uint()
        if length == 0:
            return None
        text = self.data[self.at:self.at + length - 1].decode('utf-8', 'replace')
        self.at += pad(length)
        return text

    def array(self):
        length = self.uint()
        value = self.data[self.at:self.at + length]
        self.at += pad(length)
        return value


def split_messages(buffer):
    """Complete (object, opcode, payload) messages and the unconsumed rest."""
    messages = []
    while len(buffer) >= 8:
        object_id, word = struct.unpack_from('<II', buffer)
        size = word >> 16
        if size < 8 or len(buffer) < size:
            break
        messages.append((object_id, word & 0xffff, buffer[8:size]))
        buffer = buffer[size:]
    return messages, buffer


class Client:
    def __init__(self, path, timeout=3.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.next_id = 2  # 1 is wl_display
        self.objects = {1: 'wl_display'}
        self.buffer = b''
        self.globals = {}
        self.ext, self.wlr = {}, {}
        self.done_callbacks = set()

    def new_id(self, kind):
        object_id = self.next_id
        self.next_id += 1
        self.objects[object_id] = kind
        return object_id

    def send(self, data):
        self.sock.sendall(data)

    def sync(self):
        callback = self.new_id('wl_callback')
        self.send(message(1, 0, struct.pack('<I', callback)))
        while callback not in self.done_callbacks:
            self.read()

    def read(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise RuntimeError('the compositor closed the connection')
        messages, self.buffer = split_messages(self.buffer + chunk)
        for object_id, opcode, payload in messages:
            self.dispatch(object_id, opcode, Reader(payload))

    def dispatch(self, object_id, opcode, r):
        kind = self.objects.get(object_id)
        if kind == 'wl_display' and opcode == 0:
            r.uint()
            code = r.uint()
            raise RuntimeError('protocol error %d: %s' % (code, r.string()))
        if kind == 'wl_registry' and opcode == 0:
            name, interface, version = r.uint(), r.string(), r.uint()
            self.globals[interface] = (name, version)
        elif kind == 'wl_callback' and opcode == 0:
            self.done_callbacks.add(object_id)
        elif kind == 'ext_list' and opcode == 0:
            handle = r.uint()
            self.objects[handle] = 'ext_handle'
            self.ext[handle] = {'pending': {}, 'current': None}
        elif kind == 'ext_handle':
            window = self.ext[object_id]
            if opcode == 0:
                window['closed'] = True
            elif opcode == 1:
                window['current'] = dict(window['pending'])
            elif opcode in (2, 3, 4):
                window['pending'][{2: 'title', 3: 'app_id', 4: 'identifier'}[opcode]] = r.string()
        elif kind == 'wlr_manager' and opcode == 0:
            handle = r.uint()
            self.objects[handle] = 'wlr_handle'
            self.wlr[handle] = {'pending': {}, 'current': None}
        elif kind == 'wlr_handle':
            window = self.wlr[object_id]
            if opcode == 0:
                window['pending']['title'] = r.string()
            elif opcode == 1:
                window['pending']['app_id'] = r.string()
            elif opcode == 4:
                raw = r.array()
                states = {WLR_STATES.get(v) for v in struct.unpack('<%dI' % (len(raw) // 4), raw)}
                for state in WLR_STATES.values():
                    window['pending'][state] = state in states
            elif opcode == 5:
                window['current'] = dict(window['pending'])
            elif opcode == 6:
                window['closed'] = True

    def bind(self, interface, version, kind):
        name, offered = self.globals[interface]
        object_id = self.new_id(kind)
        version = min(version, offered)
        self.send(message(self.registry, 0, struct.pack('<I', name) + encode_string(interface)
                          + struct.pack('<II', version, object_id)))
        return version

    def snapshot(self):
        self.registry = self.new_id('wl_registry')
        self.send(message(1, 1, struct.pack('<I', self.registry)))
        self.sync()
        protocols = {}
        if 'ext_foreign_toplevel_list_v1' in self.globals:
            protocols['ext_foreign_toplevel_list_v1'] = self.bind('ext_foreign_toplevel_list_v1', 1, 'ext_list')
        if 'zwlr_foreign_toplevel_manager_v1' in self.globals:
            protocols['zwlr_foreign_toplevel_manager_v1'] = self.bind('zwlr_foreign_toplevel_manager_v1', 3, 'wlr_manager')
        # Handles, then their properties and done.
        self.sync()
        self.sync()
        return protocols


def snapshot(path):
    client = Client(path)
    try:
        protocols = client.snapshot()
    finally:
        client.sock.close()
    ext = [w['current'] for w in client.ext.values() if w['current'] and not w.get('closed')]
    wlr = [w['current'] for w in client.wlr.values() if w['current'] and not w.get('closed')]
    active = [w for w in wlr if w.get('activated')]
    match = None
    if len(active) == 1:
        matches = [w for w in ext if w.get('app_id') == active[0].get('app_id') and w.get('title') == active[0].get('title')]
        match = matches[0] if len(matches) == 1 else None
    return {'ext': ext, 'wlr': wlr, 'active': match, 'protocols': protocols}


def socket_path(display=None):
    display = display or os.environ.get('WAYLAND_DISPLAY', 'wayland-0')
    return display if display.startswith('/') else os.path.join(os.environ['XDG_RUNTIME_DIR'], display)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--display', help='WAYLAND_DISPLAY name or socket path')
    args = parser.parse_args(argv)
    json.dump(snapshot(socket_path(args.display)), sys.stdout)
    sys.stdout.write('\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
