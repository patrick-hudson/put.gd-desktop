#!/usr/bin/env python3
"""Clipboard lease contracts; simulated Qt is not desktop acceptance evidence."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

# A small event dispatcher supplies real pipe/EOF and signal delivery, without
# loading Qt or touching the workstation's clipboard. The legacy timer is also
# implemented so these tests exercise core's actual leak, rather than an import.
FAKE_QT = r'''
import json, os, select, signal, sys, time, types
from pathlib import Path
result_root=sys.argv[1]
os.environ['XDG_SESSION_TYPE']='x11'
notifiers=[]; timers=[]; single=[]; wakeups=0; sets=[]; quitting=False
class Event:
 def connect(self, fn):self.fn=fn
 def fire(self):self.fn()
class Mime:
 def __init__(self):self.values={}
 def formats(self):return list(self.values)
 def data(self,mime):return self.values[mime]
 def setData(self,mime,data):self.values[mime]=data
source=Mime();source.setData('text/plain',b'line 1\r\nline 2\x00\xff')
source.setData('application/x-putgd-unit',bytes(range(256)))
class Clipboard:
 def mimeData(self):return source
 def setMimeData(self,mime):sets.append({k:bytes(v).hex() for k,v in mime.values.items()})
 def ownsClipboard(self):return True
class App:
 def __init__(self,*a):pass
 def clipboard(self):return Clipboard()
 def platformName(self):return 'simulated-x11'
 def quit(self):
  global quitting
  quitting=True
 def exec(self):
  global wakeups
  while not quitting:
   active=[n for n in notifiers if n.enabled]
   ready=select.select([n.fd for n in active],[],[],.01)[0]
   for n in active:
    if n.fd in ready:n.activated.fire()
   now=time.monotonic()
   for timer in timers:
    if now>=timer.next:
     timer.next=now+timer.interval;wakeups+=1;timer.timeout.fire()
   for deadline, fn in list(single):
    if now>=deadline:single.remove((deadline,fn));fn()
class Notifier:
 Type=types.SimpleNamespace(Read=1)
 def __init__(self,fd,kind):self.fd=fd;self.enabled=True;self.activated=Event();notifiers.append(self)
 def setEnabled(self,value):self.enabled=value
class Timer:
 def __init__(self):self.timeout=Event()
 def start(self,interval):self.interval=interval/1000;self.next=time.monotonic()+self.interval;timers.append(self)
 @staticmethod
 def singleShot(delay,fn):single.append((time.monotonic()+delay/1000,fn))
qt=types.ModuleType('PyQt6');core=types.ModuleType('PyQt6.QtCore');gui=types.ModuleType('PyQt6.QtGui')
core.QMimeData=Mime;core.QTimer=Timer;core.QSocketNotifier=Notifier;gui.QGuiApplication=App
sys.modules.update({'PyQt6':qt,'PyQt6.QtCore':core,'PyQt6.QtGui':gui})
def summary(*_):
 Path(result_root+'/unit-result.json').write_text(json.dumps(dict(wakeups=wakeups,sets=sets)))
 if _:os._exit(0)
signal.signal(signal.SIGUSR2,summary)
'''


class ClipboardLeaseTests(unittest.TestCase):
    def source(self):
        path = ROOT / 'tools/lab/surface_driver/clipboard.py'
        if path.exists():
            return path.read_text(), True
        from tools.lab.surface_driver.target import CLIPBOARD
        return CLIPBOARD, False

    def helper(self, directory):
        source, current = self.source()
        root = str(Path(directory).resolve())
        code = FAKE_QT + '\nnamespace={"__name__":"clipboard_test"}\nexec(compile(' + repr(source) + ',"clipboard_test","exec"),namespace)\n'
        if current:
            code += 'namespace["serve"](sys.argv[1],True)\nsummary()\n'
        else:
            # Legacy code executes at module scope and takes its state-file path.
            code = FAKE_QT + '\nsys.argv.append(sys.argv[1]);sys.argv[1]+="/clipboard-state"\nexec(compile(' + repr(source) + ',"clipboard_test","exec"))\n'
        process = subprocess.Popen([sys.executable, '-c', code, root], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop, process)
        deadline = time.monotonic() + 3
        while not (Path(root) / 'clipboard-state').exists():
            if process.poll() is not None:
                self.fail('helper exited before ready: ' + process.stderr.read().decode())
            if time.monotonic() > deadline:
                self.fail('helper never became ready')
            time.sleep(.01)
        return process

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.kill()
        process.wait(timeout=2)
        for stream in (process.stdin, process.stdout, process.stderr):
            stream.close()

    def test_runner_pipe_eof_restores_all_bytes_and_exits(self):
        with tempfile.TemporaryDirectory() as root:
            process = self.helper(root)
            process.stdin.close()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.fail('keeper leaked after runner input closed')
            self.assertEqual(process.returncode, 0, process.stderr.read().decode())
            result = json.loads((Path(root) / 'unit-result.json').read_text())
            self.assertEqual(result['sets'], [{'text/plain': b'line 1\r\nline 2\x00\xff'.hex(),
                                               'application/x-putgd-unit': bytes(range(256)).hex()}])

    def test_exact_pid_guard_refuses_reused_identity(self):
        from tools.lab.surface_driver.clipboard import process_identity, stop_exact
        child = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(10)'])
        self.addCleanup(lambda: (child.kill(), child.wait()) if child.poll() is None else None)
        identity = process_identity(child.pid)
        identity['start'] = str(int(identity['start']) + 1)
        with self.assertRaisesRegex(RuntimeError, 'identity changed'):
            stop_exact(identity)
        self.assertIsNone(child.poll())

    def test_native_clipboard_requires_a_verified_persistence_manager(self):
        from tools.lab.surface_driver.clipboard import restoration_plan
        payloads = {'text/plain': b'original', 'application/x-unit': b'\x00\xff'}
        with self.assertRaisesRegex(RuntimeError, 'native clipboard has no verified persistence manager'):
            restoration_plan(payloads, gnome=False, disposable=False, persistence=False)
        self.assertEqual(restoration_plan(payloads, gnome=False, disposable=True, persistence=False),
                         (list(payloads), []))
        self.assertEqual(restoration_plan({}, gnome=False, disposable=False, persistence=False), ([], []))

    def test_native_gnome_refuses_multimime_before_ownership_changes(self):
        from tools.lab.surface_driver.clipboard import restoration_plan
        payloads = {'text/plain': b'original', 'text/html': b'<b>original</b>',
                    'application/x-private': b'\x00\xff'}
        with self.assertRaisesRegex(RuntimeError, 'native GNOME clipboard cannot be restored exactly.*text/html.*application/x-private'):
            restoration_plan(payloads, gnome=True, disposable=False)
        self.assertEqual(payloads['application/x-private'], b'\x00\xff')

    def test_native_gnome_guard_never_takes_clipboard_ownership(self):
        source, current = self.source()
        self.assertTrue(current)
        with tempfile.TemporaryDirectory() as root:
            code = FAKE_QT + "\nos.environ.update(XDG_SESSION_TYPE='wayland',XDG_CURRENT_DESKTOP='GNOME')\n"
            code += 'namespace={"__name__":"clipboard_test"}\nexec(compile(' + repr(source) + ',"clipboard_test","exec"),namespace)\n'
            code += 'namespace["serve"](sys.argv[1],False)\nsummary()\n'
            result = subprocess.run([sys.executable, '-c', code, root], input=b'', capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertIn('native GNOME clipboard cannot be restored exactly', result.stdout.decode())
            self.assertEqual(json.loads((Path(root)/'unit-result.json').read_text())['sets'], [])


if __name__ == '__main__':
    unittest.main()
