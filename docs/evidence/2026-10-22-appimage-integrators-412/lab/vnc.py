"""vnc.py PORT shot OUT | scroll X Y N(+down/-up) | click X Y: the i412 guest's VNC through the workstation tunnel."""
import struct, sys, time
sys.path.insert(0, 'tools/lab')
import rfb
r = rfb.RFB(int(sys.argv[1])); cmd = sys.argv[2]
if cmd == 'shot': r.screenshot(sys.argv[3])
elif cmd == 'click': r.click(int(sys.argv[3]), int(sys.argv[4]))
elif cmd == 'scroll':
    x, y, n = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]); mask = 16 if n > 0 else 8
    for _ in range(abs(n)):
        r.sock.sendall(struct.pack('>BBHH', 5, mask, x, y)); time.sleep(.03); r.sock.sendall(struct.pack('>BBHH', 5, 0, x, y)); time.sleep(.05)
time.sleep(.3); r.close()
