#!/usr/bin/env python3
"""Click at X Y on $DISPLAY (a private Xvfb) through XTest, via ctypes."""
import ctypes
import sys
import time

x, y = int(sys.argv[1]), int(sys.argv[2])
xlib = ctypes.CDLL('libX11.so.6')
xtst = ctypes.CDLL('libXtst.so.6')
xlib.XOpenDisplay.restype = ctypes.c_void_p
display = xlib.XOpenDisplay(None)
if not display:
    sys.exit('no display')
display = ctypes.c_void_p(display)
xtst.XTestFakeMotionEvent(display, -1, x, y, 0)
xlib.XFlush(display)
time.sleep(0.2)
xtst.XTestFakeButtonEvent(display, 1, True, 0)
xlib.XFlush(display)
time.sleep(0.05)
xtst.XTestFakeButtonEvent(display, 1, False, 0)
xlib.XFlush(display)
xlib.XCloseDisplay(display)
