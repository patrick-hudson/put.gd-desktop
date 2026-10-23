#!/usr/bin/env python3
"""vmtype.py PORT key NAME... | type TEXT | frame PATH: QEMU VNC input for a guest (tools/lab/rfb.py). QEMU's VNC
server doesn't add Shift for shifted symbols, so `type` sends those as an explicit Shift chord (US layout)."""
import sys, time
sys.path.insert(0, __import__('os').path.dirname(__file__))
import rfb, afa
SHIFTED = {':': ';', '>': '.', '<': ',', '~': '`', '|': '\\', '_': '-', '"': "'", '&': '7', '?': '/', '!': '1', '@': '2',
           '#': '3', '$': '4', '%': '5', '^': '6', '*': '8', '(': '9', ')': '0', '+': '=', '{': '[', '}': ']'}
port, cmd = int(sys.argv[1]), sys.argv[2]
c = rfb.RFB(port)
if cmd == 'key':
    c.key(*sys.argv[3:])
elif cmd == 'type':
    for ch in sys.argv[3]:
        if ch in SHIFTED: c.key('shift', SHIFTED[ch])
        else: c.key(ch)
        time.sleep(.02)
elif cmd == 'click':
    c.click(int(sys.argv[3]), int(sys.argv[4]))
elif cmd == 'frame':
    f = c.frame(); afa.write_png(f, [0, 0, f[0], f[1]], sys.argv[3]); print(f[0], f[1])
c.close()
