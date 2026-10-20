"""Wall VNC watchdog contracts against a fake local RFB server; no builder, desktop or unit is touched."""
from pathlib import Path
import socket
import struct
import sys
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
import wall_vnc_watchdog as watchdog


class Clock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


class Decisions(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.dog = watchdog.Watchdog(stale=300, grace=60, max_backoff=1800, now=self.clock)

    def test_live_stream_is_left_alone(self):
        for _ in range(20):
            self.clock.now += 60
            self.dog.updated()
            self.assertIsNone(self.dog.reason())

    def test_stale_stream_restarts_after_the_threshold(self):
        self.clock.now += 299
        self.assertIsNone(self.dog.reason())
        self.clock.now += 1
        self.assertIn('no framebuffer update for 300 s', self.dog.reason())

    def test_reset_waits_for_grace_and_is_not_lost(self):
        self.clock.now += 30
        self.dog.reset_logged()
        self.assertIsNone(self.dog.reason())
        self.clock.now += 30
        self.assertIn('GPU hang', self.dog.reason())
        self.dog.restarted()
        self.assertIsNone(self.dog.reason())

    def test_failed_restarts_back_off_and_an_update_clears_them(self):
        waits = []
        for _ in range(7):
            self.dog.restarted()
            waits.append(self.dog.wait_after_restart())
        self.assertEqual(waits, [60, 120, 240, 480, 960, 1800, 1800])
        self.dog.updated()
        self.assertEqual(self.dog.wait_after_restart(), 60)

    def test_reset_pattern_matches_kernel_and_kwin_but_not_own_log(self):
        self.assertTrue(watchdog.RESET_PATTERN.search(
            'i915 0000:00:02.0: [drm] GPU HANG: ecode 9:1:85dffffb, in kwin_wayland [1495]'))
        self.assertTrue(watchdog.RESET_PATTERN.search(
            'kwin_scene_opengl: A graphics reset attributable to the current GL context occurred.'))
        self.assertFalse(watchdog.RESET_PATTERN.search(
            '2026-10-20T03:00:00Z restarting w0vncserver.service: the journal reports a GPU hang or KWin GL reset'))


class FakeServer:
    """Answers the handshake, then sends whatever rectangles each update request should produce."""

    def __init__(self, updates):
        self.updates = list(updates)  # one list of (w, h) rectangles per request; [] means stay silent
        self.requests = []
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        conn, _ = self.listener.accept()
        with conn:
            conn.sendall(b'RFB 003.008\n')
            conn.recv(12)
            conn.sendall(b'\x01\x01')
            conn.recv(1)
            conn.sendall(struct.pack('>I', 0))
            self.shared = conn.recv(1)
            name = b'fake'
            conn.sendall(struct.pack('>HH', 64, 32) + bytes(16) + struct.pack('>I', len(name)) + name)
            conn.recv(20)  # SetPixelFormat
            conn.recv(8)   # SetEncodings with one encoding
            while True:
                request = conn.recv(10)
                if len(request) < 10:
                    return
                self.requests.append(request[1])
                rects = self.updates.pop(0) if self.updates else []
                if rects:
                    payload = struct.pack('>BxH', 0, len(rects))
                    for w, h in rects:
                        payload += struct.pack('>HHHHi', 0, 0, w, h, 0) + bytes(w * h * 4)
                    conn.sendall(payload)


class Reader(unittest.TestCase):
    def test_counts_updates_and_times_out_on_a_frozen_server(self):
        server = FakeServer([[(64, 32)], [(4, 2), (1, 1)], []])
        reader = watchdog.UpdateReader(server.port)
        self.addCleanup(reader.close)
        self.assertEqual((reader.width, reader.height), (64, 32))
        reader.request(incremental=False)
        self.assertEqual(reader.wait_update(5), 64 * 32)
        reader.request()
        self.assertEqual(reader.wait_update(5), 9)
        reader.request()
        self.assertIsNone(reader.wait_update(0.3))
        self.assertEqual(server.shared, b'\x01')
        self.assertEqual(server.requests, [0, 1, 1])


if __name__ == '__main__':
    unittest.main()
