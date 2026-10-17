"""Minimal RFB 3.8 client for physical-desktop acceptance over loopback tunnels.
Security type None only (the wall's tunnels are loopback-only, unauthenticated).
Provides real key/pointer input and raw framebuffer screenshots saved as PNG."""
import socket, struct, time, zlib

KEYSYMS = {'ctrl': 0xffe3, 'shift': 0xffe1, 'alt': 0xffe9, 'super': 0xffeb, 'ret': 0xff0d, 'esc': 0xff1b,
           'tab': 0xff09, 'backspace': 0xff08, 'delete': 0xffff, 'left': 0xff51, 'up': 0xff52, 'right': 0xff53, 'down': 0xff54,
           **{f'f{i}': 0xffbd + i for i in range(1, 13)}}

class RFB:
    def __init__(self, port, host='127.0.0.1', timeout=10):
        self.sock = socket.create_connection((host, port), timeout)
        version = self._read(12)
        if not version.startswith(b'RFB '):
            raise RuntimeError('not an RFB server')
        self.sock.sendall(b'RFB 003.008\n')
        count = self._read(1)[0]
        if count == 0:
            raise RuntimeError('server refused: ' + self._read(struct.unpack('>I', self._read(4))[0]).decode(errors='replace'))
        types = list(self._read(count))
        self.security_types = types
        if 1 not in types:
            raise RuntimeError(f'no None security type offered: {types}')
        self.sock.sendall(b'\x01')
        if struct.unpack('>I', self._read(4))[0] != 0:
            raise RuntimeError('security handshake failed')
        self.sock.sendall(b'\x01')  # shared session: keep the wall connected
        self.width, self.height = struct.unpack('>HH', self._read(4))
        self._read(16)
        self.name = self._read(struct.unpack('>I', self._read(4))[0]).decode(errors='replace')
        # 32bpp little-endian true colour: bytes B,G,R,X
        self.sock.sendall(struct.pack('>BxxxBBBBHHHBBBxxx', 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
        self.sock.sendall(struct.pack('>BxHi', 2, 1, 0))  # raw encoding only
        self.buttons = 0

    def _read(self, n):
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise RuntimeError('connection closed')
            data += chunk
        return data

    def key(self, *names, hold=0.05):
        codes = [KEYSYMS.get(n, ord(n) if len(n) == 1 else None) for n in names]
        if None in codes:
            raise ValueError(names)
        for code in codes:
            self.sock.sendall(struct.pack('>BBxxI', 4, 1, code)); time.sleep(.01)
        time.sleep(hold)
        for code in reversed(codes):
            self.sock.sendall(struct.pack('>BBxxI', 4, 0, code)); time.sleep(.01)

    def type(self, text):
        for c in text:
            self.key(c)

    def pointer(self, x, y, down=None):
        if down is True: self.buttons |= 1
        if down is False: self.buttons &= ~1
        self.sock.sendall(struct.pack('>BBHH', 5, self.buttons, max(0, min(self.width - 1, int(x))), max(0, min(self.height - 1, int(y)))))

    def drag(self, x0, y0, x1, y1, steps=12, settle=.25):
        self.pointer(x0, y0); time.sleep(.1); self.pointer(x0, y0, True); time.sleep(settle)
        for i in range(1, steps + 1):
            self.pointer(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps); time.sleep(.03)
        time.sleep(settle); self.pointer(x1, y1, False)

    def click(self, x, y):
        self.pointer(x, y); time.sleep(.05); self.pointer(x, y, True); time.sleep(.05); self.pointer(x, y, False)

    def frame(self):
        self.sock.sendall(struct.pack('>BBHHHH', 3, 0, 0, 0, self.width, self.height))
        pixels = bytearray(self.width * self.height * 4)
        while True:
            kind = self._read(1)[0]
            if kind == 0:
                self._read(1); rects = struct.unpack('>H', self._read(2))[0]
                for _ in range(rects):
                    x, y, w, h, enc = struct.unpack('>HHHHi', self._read(12))
                    if enc == -223:  # DesktopSize
                        self.width, self.height = w, h; pixels = bytearray(w * h * 4); continue
                    if enc != 0:
                        raise RuntimeError(f'unexpected encoding {enc}')
                    data = self._read(w * h * 4)
                    for row in range(h):
                        start = ((y + row) * self.width + x) * 4
                        pixels[start:start + w * 4] = data[row * w * 4:(row + 1) * w * 4]
                return self.width, self.height, bytes(pixels)
            if kind == 1:  # colour map
                self._read(3); n = struct.unpack('>H', self._read(2))[0]; self._read(n * 6)
            elif kind == 2:  # bell
                pass
            elif kind == 3:  # server cut text
                self._read(3); self._read(struct.unpack('>I', self._read(4))[0])
            else:
                raise RuntimeError(f'unexpected server message {kind}')

    def screenshot(self, path):
        w, h, bgrx = self.frame()
        raw = bytearray()
        for row in range(h):
            raw.append(0)
            line = bgrx[row * w * 4:(row + 1) * w * 4]
            rgb = bytearray(w * 3); rgb[0::3] = line[2::4]; rgb[1::3] = line[1::4]; rgb[2::3] = line[0::4]
            raw += rgb
        chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
        open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                               chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
        return w, h, bgrx

    def close(self):
        self.sock.close()

if __name__ == '__main__':
    import sys
    c = RFB(int(sys.argv[1]))
    print({'name': c.name, 'size': [c.width, c.height], 'security': c.security_types})
    if len(sys.argv) > 2:
        print(c.screenshot(sys.argv[2])[:2])
    c.close()
