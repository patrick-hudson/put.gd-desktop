"""The labwc/Mir window witness against a scripted Wayland wire peer (#1275)."""
import os
import socket
import struct
import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
import wayland_toplevels as wt  # noqa: E402


def event(object_id, opcode, payload=b''):
    return wt.message(object_id, opcode, payload)


def string(text):
    return wt.encode_string(text)


class Peer(threading.Thread):
    """Answers the witness's two-phase snapshot like a compositor would."""

    def __init__(self, path, active='beta'):
        super().__init__(daemon=True)
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(path)
        self.server.listen(1)
        self.active = active
        self.requests = []

    def run(self):
        conn, _ = self.server.accept()
        buffer, syncs = b'', 0
        with conn:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    return
                messages, buffer = wt.split_messages(buffer + chunk)
                for object_id, opcode, payload in messages:
                    self.requests.append((object_id, opcode))
                    if object_id != 1 or opcode != 0:
                        continue
                    callback = struct.unpack('<I', payload)[0]
                    syncs += 1
                    out = b''
                    if syncs == 1:
                        out += event(2, 0, struct.pack('<I', 10) + string('ext_foreign_toplevel_list_v1') + struct.pack('<I', 1))
                        out += event(2, 0, struct.pack('<I', 11) + string('zwlr_foreign_toplevel_manager_v1') + struct.pack('<I', 3))
                        out += event(2, 0, struct.pack('<I', 12) + string('wl_seat') + struct.pack('<I', 9))
                    if syncs == 2:
                        for handle, (app, title, ident) in enumerate([('alpha', 'Alpha', 'a1'), ('beta', 'Beta', 'b2')]):
                            ext, wlr = 0xff000000 + handle, 0xff000010 + handle
                            out += event(4, 0, struct.pack('<I', ext))
                            out += event(ext, 2, string(title)) + event(ext, 3, string(app)) + event(ext, 4, string(ident)) + event(ext, 1)
                            states = struct.pack('<I', 2) if app == self.active else b''
                            out += event(5, 0, struct.pack('<I', wlr))
                            out += event(wlr, 0, string(title)) + event(wlr, 1, string(app))
                            out += event(wlr, 4, struct.pack('<I', len(states)) + states) + event(wlr, 5)
                    out += event(callback, 0, struct.pack('<I', 0))
                    conn.sendall(out)


class WaylandToplevelsTest(unittest.TestCase):
    def snapshot(self, active):
        with tempfile.TemporaryDirectory(prefix='wt-') as root:
            path = os.path.join(root, 'w')
            peer = Peer(path, active)
            peer.start()
            result = wt.snapshot(path)
            return result, peer.requests

    def test_lists_both_protocols_and_matches_the_active_window(self):
        result, requests = self.snapshot('beta')
        self.assertEqual([w['identifier'] for w in result['ext']], ['a1', 'b2'])
        self.assertEqual(result['active'], {'title': 'Beta', 'app_id': 'beta', 'identifier': 'b2'})
        self.assertEqual(result['protocols'], {'ext_foreign_toplevel_list_v1': 1, 'zwlr_foreign_toplevel_manager_v1': 3})
        self.assertEqual([w['activated'] for w in result['wlr']], [False, True])
        # get_registry, sync, two binds, two syncs; nothing else is requested.
        self.assertEqual(requests, [(1, 1), (1, 0), (2, 0), (2, 0), (1, 0), (1, 0)])

    def test_no_active_window_reports_none(self):
        result, _ = self.snapshot(None)
        self.assertIsNone(result['active'])

    def test_wire_round_trip(self):
        data = wt.message(7, 3, wt.encode_string('héllo') + struct.pack('<I', 5))
        messages, rest = wt.split_messages(data + b'\x01\x02')
        self.assertEqual(rest, b'\x01\x02')
        (object_id, opcode, payload), = messages
        reader = wt.Reader(payload)
        self.assertEqual((object_id, opcode, reader.string(), reader.uint()), (7, 3, 'héllo', 5))


if __name__ == '__main__':
    unittest.main()
