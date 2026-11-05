import sys, time
sys.path.insert(0, sys.argv[1])
from tools.lab import lab
run = sys.argv[2]
q = '''set +e
export DISPLAY=:0
xinput query-state "QEMU QEMU USB Tablet" | grep -E "button\\[1\\]"
xinput query-state "AT Translated Set 2 keyboard" | grep -E "key\\[50\\]"
true'''
lab.input_events(run, [{'type': 'btn', 'data': {'button': 'left', 'down': True}},
                       {'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': 'shift'}}}])
time.sleep(.3)
print('held:', lab.guest_session(run, q).split())
lab.input_events(run, [{'type': 'btn', 'data': {'button': 'left', 'down': False}},
                       {'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': 'shift'}}}])
time.sleep(.3)
print('released:', lab.guest_session(run, q).split())
