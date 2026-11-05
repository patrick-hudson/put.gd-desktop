# #333: drive a drag through the host_input socket with lab.pointer's mapping.
import json, math, socket, sys, time
import os; MODE = os.environ.get("MAP", "lab")
sock, W, H = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
x0, y0, x1, y1 = map(int, sys.argv[4:8])
s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile('rwb')
def call(cmd):
    f.write(json.dumps(cmd).encode() + b'\n'); f.flush(); return json.loads(f.readline())
f.readline(); call({'execute': 'qmp_capabilities'})
def ptr(x, y, down=None):
    ev = [{'type': 'abs', 'data': {'axis': 'x', 'value': min(32767, int(x * 32767 / (W - 1)) if MODE == "afa" else x * 32767 // W if MODE == "vnc" else math.ceil(x * 32768 / W))}},
          {'type': 'abs', 'data': {'axis': 'y', 'value': min(32767, int(y * 32767 / (H - 1)) if MODE == "afa" else y * 32767 // H if MODE == "vnc" else math.ceil(y * 32768 / H))}}]
    if down is not None: ev.append({'type': 'btn', 'data': {'button': 'left', 'down': down}})
    r = call({'execute': 'input-send-event', 'arguments': {'events': ev}}); assert 'return' in r, r
ptr(x0, y0); time.sleep(.2); ptr(x0, y0, True); time.sleep(.2)
for i in range(1, 11):
    ptr(x0 + (x1 - x0) * i // 10, y0 + (y1 - y0) * i // 10); time.sleep(.03)
time.sleep(.2); ptr(x1, y1, False); time.sleep(.2)
print('values', [math.ceil(v * 32768 / d) for v, d in ((x0, W), (y0, H), (x1, W), (y1, H))])
