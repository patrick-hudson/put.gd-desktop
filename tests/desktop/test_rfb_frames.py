import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/lab'))
import rfb


class Wire:
    def __init__(self, data):
        self.data = data

    def recv(self, size):
        part, self.data = self.data[:size], self.data[size:]
        return part

    def sendall(self, _data):
        pass


def update(x, width, pixels):
    return struct.pack('>BBHHHHHi', 0, 0, 1, x, 0, width, 1, 0) + pixels


class FramebufferUpdates(unittest.TestCase):
    def client(self, wire):
        client = rfb.RFB.__new__(rfb.RFB)
        client.width, client.height = 2, 1
        client.pixels = bytearray(8)
        client.sock = Wire(wire)
        return client

    def test_partial_update_preserves_previously_received_pixels(self):
        red, blue, green = bytes((0, 0, 255, 0)), bytes((255, 0, 0, 0)), bytes((0, 255, 0, 0))
        client = self.client(update(0, 2, red + blue) + update(1, 1, green))
        self.assertEqual(client.frame(), (2, 1, red + blue))
        self.assertEqual(client.frame(), (2, 1, red + green))

    def test_resize_invalidates_the_old_framebuffer(self):
        resized = struct.pack('>BBHHHHHi', 0, 0, 1, 0, 0, 1, 1, -223)
        client = self.client(update(0, 2, b'abcdefgh') + resized)
        client.frame()
        self.assertEqual(client.frame(), (1, 1, bytes(4)))


if __name__ == '__main__':
    unittest.main()
