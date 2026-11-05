#!/usr/bin/env python3
"""grab.py PORT OUT.png : one full frame from a loopback VNC server (stays on the host until copied)."""
import sys; sys.path.insert(0, '/home/patrick/work/put.gd-afa2/tool')
import afa, rfb
f = rfb.RFB(int(sys.argv[1])).frame()
afa.write_png(f, [0, 0, f[0], f[1]], __import__('pathlib').Path(sys.argv[2])); print(f[0], f[1])
