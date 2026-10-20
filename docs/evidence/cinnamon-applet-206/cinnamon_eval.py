#!/usr/bin/env python3
"""Run a Cinnamon Eval script from a file; prints its (success, JSON) reply. Read-only inspection unless the script acts."""
import subprocess, sys
code = open(sys.argv[1]).read()
for i, value in enumerate(sys.argv[2:]):
    code = code.replace('@' + str(i), value)
out = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.Cinnamon', '--object-path', '/org/Cinnamon',
                      '--method', 'org.Cinnamon.Eval', code], capture_output=True, text=True, timeout=10,
                     env={'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/run/user/1000/bus', 'PATH': '/usr/bin:/bin'})
print(out.stdout.strip() or out.stderr.strip())
