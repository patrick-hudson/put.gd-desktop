#!/usr/bin/env python3
"""Which files the loader maps when put.gd's S3 module is loaded the way the app does it.

AppRun sets LD_LIBRARY_PATH=usr/lib:usr/lib/putgd; S3Module dlopens usr/lib/putgd/libputgd-s3.so
by absolute path. argv: SQUASHFS_ROOT. Re-executes itself with that environment."""
import ctypes, json, os, sys
root = os.path.realpath(sys.argv[1])
if os.environ.get("PUTGD_LOADED_COPIES") != root:
    env = dict(os.environ, PUTGD_LOADED_COPIES=root,
               LD_LIBRARY_PATH=root + "/usr/lib:" + root + "/usr/lib/putgd")
    os.execve(sys.executable, [sys.executable, "-I", __file__, root], env)
ctypes.CDLL(root + "/usr/lib/putgd/libputgd-s3.so", mode=os.RTLD_NOW)
mapped = sorted({line.split()[-1] for line in open("/proc/self/maps") if root in line})
print(json.dumps([path[len(root) + 1:] for path in mapped], indent=1))
