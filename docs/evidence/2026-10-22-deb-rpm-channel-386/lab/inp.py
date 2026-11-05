#!/usr/bin/env python3
"""Tiny client for tools/lab/host_input.py: inp.py SOCK move X Y | click X Y | dblclick X Y | key a+b (qcodes)."""
import json, socket, sys, time
W, H = 1920, 1080
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile('rwb')
def call(cmd):
    f.write(json.dumps(cmd).encode() + b'\n'); f.flush(); return json.loads(f.readline())
f.readline(); call({'execute': 'qmp_capabilities'})
ev = lambda events: call({'execute': 'input-send-event', 'arguments': {'events': events}})
def move(x, y): ev([{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (W - 1))}}, {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (H - 1))}}])
def click():
    ev([{'type': 'btn', 'data': {'down': True, 'button': 'left'}}]); time.sleep(.05); ev([{'type': 'btn', 'data': {'down': False, 'button': 'left'}}])
op = sys.argv[2]
if op == 'move': move(int(sys.argv[3]), int(sys.argv[4]))
elif op in ('click', 'dblclick'):
    move(int(sys.argv[3]), int(sys.argv[4])); time.sleep(.15); click()
    if op == 'dblclick': time.sleep(.08); click()
elif op == 'key':
    keys = sys.argv[3].split('+')
    ev([{'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': k}}} for k in keys]); time.sleep(.05)
    ev([{'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': k}}} for k in reversed(keys)])
print('ok', op)
