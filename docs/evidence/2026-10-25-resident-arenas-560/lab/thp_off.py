#!/usr/bin/env python3
"""Diagnostic equalizer: disable THP for this process tree (prctl PR_SET_THP_DISABLE), then exec."""
import ctypes, os, sys
libc=ctypes.CDLL(None,use_errno=True)
if libc.prctl(41,1,0,0,0)!=0:raise SystemExit('prctl_failed')
os.execv(sys.argv[1],sys.argv[1:])
