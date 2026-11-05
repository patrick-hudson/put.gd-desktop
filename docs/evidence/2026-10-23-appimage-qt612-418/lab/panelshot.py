#!/usr/bin/env python3
"""panelshot.py NAME OUT : a full-desktop frame from the run's loopback VNC (for locating the tray icon); stays on the host."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import afa
run = afa.Run(sys.argv[1]); frames, inp, size = afa.tools(run); afa.park(run, inp, size)
f = frames.grab(); afa.write_png(f, [0, 0, f[0], f[1]], Path(sys.argv[2])); print(sys.argv[2])
