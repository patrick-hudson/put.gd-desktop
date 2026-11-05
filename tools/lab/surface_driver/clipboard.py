"""Target-side clipboard lease. Clipboard contents stay in this process's memory.

The controller holds stdin open over one SSH channel. EOF (including controller
SIGKILL), parent death and termination all request the same bounded restoration.
No periodic clipboard queries or re-ownership are needed.
"""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import stat
import sys
import time


def private(path, directory=False):
    path = Path(path)
    info = path.lstat()
    kind = stat.S_ISDIR if directory else stat.S_ISREG
    if path.resolve() != path or not kind(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise RuntimeError('clipboard ownership metadata is not private')
    return path


def process_identity(pid):
    process = Path('/proc') / str(pid)
    fields = (process / 'stat').read_text().split(') ', 1)[1].split()
    return {'pid': pid, 'start': fields[19],
            'boot': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
            'argv': (process / 'cmdline').read_bytes().split(b'\0')[:-1],
            'uid': process.stat().st_uid, 'state': fields[0], 'parent': int(fields[1])}


def stop_exact(identity, sig=signal.SIGTERM, timeout=5, restore_state=None):
    """Pin before validating, so PID reuse cannot redirect the signal."""
    try:
        descriptor = os.pidfd_open(identity['pid'])
    except ProcessLookupError:
        return
    try:
        try:
            current = process_identity(identity['pid'])
        except FileNotFoundError:
            return
        if current['state'] == 'Z':
            return
        for key in ('pid', 'start', 'boot', 'argv', 'uid'):
            if current[key] != identity[key]:
                raise RuntimeError('clipboard process identity changed')
        if current['uid'] != os.getuid():
            raise RuntimeError('clipboard process is not owned')
        restoration_failed = False
        if restore_state is not None:
            state = private(restore_state)
            if state.read_text() not in ('ready', 'restored'):
                raise RuntimeError('legacy clipboard state is invalid')
            if state.read_text() == 'ready':
                signal.pidfd_send_signal(descriptor, signal.SIGUSR1)
                deadline = time.monotonic() + 5
                while state.read_text() != 'restored':
                    if time.monotonic() >= deadline:
                        restoration_failed = True
                        break
                    time.sleep(.02)
        signal.pidfd_send_signal(descriptor, sig)
        if not select.select([descriptor], [], [], timeout)[0]:
            signal.pidfd_send_signal(descriptor, signal.SIGKILL)
            if not select.select([descriptor], [], [], 2)[0]:
                raise RuntimeError('clipboard process did not exit after exact SIGKILL')
            raise RuntimeError('clipboard process required exact SIGKILL')
        if restoration_failed:
            raise RuntimeError('legacy clipboard restoration failed before exact cleanup')
    finally:
        os.close(descriptor)


def reap_legacy(source):
    """Recover only an orphan with core's exact argv, private root and source.

    Core predates identity receipts. Discovery enumerates processes; a name or
    substring match never authorizes signalling one. Active keepers are refused.
    """
    digest = hashlib.sha256(source.encode()).digest()
    stopped = []
    for process in Path('/proc').iterdir():
        if not process.name.isdecimal():
            continue
        try:
            identity = process_identity(int(process.name))
            argv = identity['argv']
            if len(argv) != 3 or argv[0] != b'python3':
                continue
            script = Path(os.fsdecode(argv[1]))
            root = script.parent
            if script.name != 'clipboard-keeper.py' or argv[2] != os.fsencode(root / 'clipboard-state'):
                continue
            if identity['uid'] != os.getuid():
                continue
            env = (process / 'environ').read_bytes().split(b'\0')
            expected = [('XDG_RUNTIME_DIR', 'runtime'), ('XDG_CONFIG_HOME', 'config'),
                        ('XDG_DATA_HOME', 'data'), ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache')]
            if any(os.fsencode(key + '=' + str(root / suffix)) not in env for key, suffix in expected):
                continue
            missing_root = not root.exists()
            if missing_root:
                # Successful core cleanup removed the script while its owner
                # remained alive. Require the complete canonical core argv and
                # all five private homes, then pin UID/boot/PID/start/argv below.
                token = root.name.removeprefix('sd-')
                if root.parent != Path('/tmp') or len(token) != 8 or any(c not in '0123456789abcdef' for c in token):
                    continue
            else:
                private(root, directory=True)
                private(script)
                if hashlib.sha256(script.read_bytes()).digest() != digest:
                    continue
            if identity['parent'] != 1:
                raise RuntimeError('another clipboard preservation run is active')
            # Legacy helpers have no run-bound pipe. A live private resident
            # makes this an active run, even though nohup reparented its keeper.
            for other in Path('/proc').iterdir():
                try:
                    if ((other / 'comm').read_text().strip() in ('putgd', 'putgd-ui') and
                            os.fsencode('XDG_RUNTIME_DIR=' + str(root / 'runtime')) in
                            (other / 'environ').read_bytes().split(b'\0')):
                        raise RuntimeError('another clipboard preservation run is active')
                except (FileNotFoundError, PermissionError, NotADirectoryError):
                    pass
            stop_exact(identity, restore_state=None if missing_root else root / 'clipboard-state')
            stopped.append(identity['pid'])
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return stopped


def reap_current(exclude=None):
    """Recover a previous lease only with its complete saved process identity."""
    stopped = []
    for process in Path('/proc').iterdir():
        if not process.name.isdecimal():
            continue
        try:
            current = process_identity(int(process.name))
            if current['pid'] == exclude:
                continue
            argv = current['argv']
            if len(argv) not in (3, 4) or argv[0] != b'python3' or (len(argv) == 4 and argv[3] != b'--disposable'):
                continue
            script = Path(os.fsdecode(argv[1]))
            root = script.parent
            if script.name != 'clipboard-keeper.py' or argv[2] != os.fsencode(root):
                continue
            private(root, directory=True)
            private(script)
            record = json.loads(private(root / 'clipboard-identity.json').read_text())
            if record.get('protocol') != 'putgd-clipboard-lease-v1':
                continue
            if hashlib.sha256(script.read_bytes()).hexdigest() != record.get('source_sha256'):
                raise RuntimeError('clipboard lease source identity changed')
            record['argv'] = [os.fsencode(a) for a in record['argv']]
            for key in ('pid', 'start', 'boot', 'argv', 'uid'):
                if current[key] != record[key]:
                    raise RuntimeError('clipboard lease identity no longer matches')
            if current['parent'] != 1:
                raise RuntimeError('another clipboard preservation run is active')
            stop_exact(record)
            stopped.append(record['pid'])
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return stopped


# Mutter's clipboard manager retains one preferred supported format. Keep its
# limits explicit: native desktops must never silently lose an offered format.
GNOME_MIMES = ('image/tiff', 'image/bmp', 'image/gif', 'image/jpeg', 'image/webp',
               'image/png', 'image/svg+xml', 'text/plain', 'text/plain;charset=utf-8')
PROTOCOL_TARGETS = frozenset(('TARGETS', 'MULTIPLE', 'TIMESTAMP', 'SAVE_TARGETS'))


def x11_persistence_available():
    """The ICCCM persistence manager, queried without changing any selection."""
    import ctypes.util
    library = ctypes.util.find_library('X11')
    if not library:
        return False
    x11 = ctypes.CDLL(library)
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XGetSelectionOwner.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XGetSelectionOwner.restype = ctypes.c_ulong
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    if not display:
        return False
    try:
        atom = x11.XInternAtom(display, b'CLIPBOARD_MANAGER', True)
        return bool(atom and x11.XGetSelectionOwner(display, atom))
    finally:
        x11.XCloseDisplay(display)


def restoration_plan(payloads, *, gnome, disposable, persistence=True):
    """Return the formats that can survive owner exit, plus explicit review notes."""
    formats = list(payloads)
    if not formats:
        return formats, []
    if not gnome:
        if not disposable and not persistence:
            raise RuntimeError('native clipboard has no verified persistence manager; cannot restore MIME types '
                               'after keeper exit: ' + ', '.join(formats))
        return formats, []
    supported = [mime for mime in GNOME_MIMES if mime in payloads]
    preferred = supported[-1:]  # Same preference order as Mutter.
    omitted = [mime for mime in formats if mime not in preferred]
    if preferred and preferred[0].startswith('text/') and len(payloads[preferred[0]]) > 4 * 1024 * 1024:
        preferred = []
        omitted = formats
    if not disposable and omitted:
        raise RuntimeError('native GNOME clipboard cannot be restored exactly after keeper exit; '
                           'Mutter persists one supported MIME type (text limit 4 MiB); '
                           'unrestorable MIME types: ' + ', '.join(omitted))
    reviews = []
    if omitted:
        reviews.append('Disposable GNOME clipboard restoration preserves ' +
                       (preferred[0] if preferred else 'no supported persistent MIME type') +
                       '; other MIME types were not restorable: ' + ', '.join(omitted))
    return preferred, reviews


def serve(root, disposable=False):
    # This helper exposes no controls. Registering it with AT-SPI is unnecessary.
    os.environ.pop('QT_LINUX_ACCESSIBILITY_ALWAYS_ON', None)
    os.environ['QT_ACCESSIBILITY'] = '0'
    os.environ['NO_AT_BRIDGE'] = '1'
    from PyQt6.QtCore import QMimeData, QSocketNotifier, QTimer
    from PyQt6.QtGui import QGuiApplication

    root = private(root, directory=True)
    state = root / 'clipboard-state'
    parent = os.getppid()
    signals = (signal.SIGTERM, signal.SIGHUP, signal.SIGINT, signal.SIGUSR1)
    reader, writer = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
    signal.set_wakeup_fd(writer)
    for sig in signals:
        signal.signal(sig, lambda *_: None)
    if ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGTERM) != 0:
        raise OSError(ctypes.get_errno(), 'clipboard parent-death binding failed')

    identity = process_identity(os.getpid())
    identity['argv'] = [os.fsdecode(a) for a in identity['argv']]
    identity['protocol'] = 'putgd-clipboard-lease-v1'
    script = root / 'clipboard-keeper.py'
    identity['source_sha256'] = hashlib.sha256(script.read_bytes()).hexdigest() if script.exists() else None
    with os.fdopen(os.open(root / 'clipboard-identity.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as record:
        json.dump(identity, record)
    print(json.dumps({'identity': identity}), flush=True)

    # EOF is the run lease. A wedged Qt clipboard transfer must not prevent
    # process exit: one sleeping guardian arms a kernel deadline only at EOF or
    # termination. It performs no clipboard work and has no idle wakeups.
    import threading
    ending, finished = threading.Event(), threading.Event()
    end_lock = threading.Lock()
    signal.signal(signal.SIGALRM, signal.SIG_DFL)
    def bound_exit():
        with end_lock:
            if not ending.is_set() and not finished.is_set():
                ending.set()
                signal.alarm(8)
    def guard_lease():
        try:
            select.select([0, reader], [], [])
            bound_exit()
        except OSError:
            pass
    threading.Thread(target=guard_lease, daemon=True).start()

    app = QGuiApplication([])
    clipboard = app.clipboard()
    saved = QMimeData()
    payloads = {}
    source = clipboard.mimeData()
    size = 0
    if source:
        for mime in source.formats():
            if mime in PROTOCOL_TARGETS:
                continue  # X11 protocol requests, never saved application payloads.
            data = source.data(mime)
            size += len(data)
            if size > 32 * 1024 * 1024:
                raise RuntimeError('clipboard exceeds preservation budget')
            payloads[mime] = bytes(data)
    gnome = os.environ.get('XDG_SESSION_TYPE') == 'wayland' and 'GNOME' in os.environ.get('XDG_CURRENT_DESKTOP', '')
    try:
        persistence = disposable or gnome or (app.platformName() == 'xcb' and x11_persistence_available())
        formats, reviews = restoration_plan(payloads, gnome=gnome, disposable=disposable, persistence=persistence)
    except RuntimeError as error:
        print(json.dumps({'blocked': str(error)}), flush=True)
        return
    for mime in formats:
        saved.setData(mime, payloads[mime])
    payloads.clear()
    restored = False

    def restore():
        nonlocal restored
        if restored:
            return
        restored = True
        bound_exit()
        control.setEnabled(False)
        clipboard.setMimeData(saved)
        # Give the native session clipboard manager a bounded opportunity to
        # fetch its persistent copy; this is a one-shot exit, never an idle poll.
        QTimer.singleShot(300, app.quit)
        state.write_text('restored')

    def control_ready(*_):
        command = os.read(0, 4096)
        if not command or command == b'restore\n':
            restore()
        else:
            # Invalid control input also restores before ending the lease.
            restore()

    def signal_ready(*_):
        try:
            os.read(reader, 4096)
        except BlockingIOError:
            return
        restore()

    control = QSocketNotifier(0, QSocketNotifier.Type.Read)
    control.activated.connect(control_ready)
    wakeup = QSocketNotifier(reader, QSocketNotifier.Type.Read)
    wakeup.activated.connect(signal_ready)
    state.write_text('ready')
    print(json.dumps({'ready': True, 'preserved_mimes': formats, 'reviews': reviews,
                      'transport': 'x11-selection-bridge' if gnome else app.platformName()}), flush=True)
    if os.getppid() != parent or parent == 1:
        QTimer.singleShot(0, restore)
    try:
        app.exec()
    finally:
        with end_lock:
            finished.set()
            signal.alarm(0)
        signal.set_wakeup_fd(-1)
        os.close(reader)
        os.close(writer)


class Lease:
    """Controller half: one pipe bound to the runner, plus exact-PID cleanup."""
    def __init__(self, target, legacy):
        self.target, self.legacy = target, legacy
        self.process = None
        self.identity = None
        self.buffer = b''
        self.report = {'restored_and_stopped': False, 'reviews': []}

    def start(self):
        import shlex
        import subprocess
        from .api import RecipeBlocked
        from tools.lab import lab
        source = Path(__file__).read_text()
        path = self.target.root + '/clipboard-keeper.py'
        code = 'from pathlib import Path\nPath(' + repr(path) + ').write_text(' + repr(source) + ')\n'
        result = self.target.execute("python3 - <<'SD_KEEPER'\n" + code + 'SD_KEEPER\n')
        if result['returncode']:
            raise RecipeBlocked('cannot stage private clipboard preservation')
        keys = ('DISPLAY', 'XAUTHORITY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_SESSION_TYPE',
                'XDG_CURRENT_DESKTOP', 'WAYLAND_DISPLAY', 'QT_QPA_PLATFORM',
                'XDG_RUNTIME_DIR', 'XDG_CONFIG_DIRS', 'XDG_DATA_DIRS')
        environment = ('import os,json,subprocess\ne={k:os.environ[k] for k in ' + repr(keys) + ' if k in os.environ}\n'
                       'if e.get("XDG_SESSION_TYPE")=="wayland" and "GNOME" in e.get("XDG_CURRENT_DESKTOP",""):\n'
                       ' p=subprocess.run(["systemctl","--user","show-environment"],capture_output=True,text=True,timeout=3)\n'
                       ' for line in p.stdout.splitlines():\n'
                       '  k,_,v=line.partition("=")\n'
                       '  if k in ("DISPLAY","XAUTHORITY") and v:e.setdefault(k,v)\n'
                       'print(json.dumps(e))\n')
        native = json.loads(self.target.session("python3 - <<'SD_CLIP_ENV'\n" + environment + 'SD_CLIP_ENV\n'))
        if native.get('XDG_SESSION_TYPE') == 'wayland' and 'GNOME' in native.get('XDG_CURRENT_DESKTOP', ''):
            # A headless Wayland client has no focused data-device selection.
            # Use Mutter's existing X11 selection bridge for this clipboard-only
            # helper; recipes and their compositor witnesses remain native.
            native['QT_QPA_PLATFORM'] = 'xcb'

        command = 'set -eu\n' + ''.join('export ' + k + '=' + shlex.quote(v) + '\n' for k, v in native.items())
        command += self.target.prefix() + 'exec python3 ' + shlex.quote(path) + ' ' + shlex.quote(self.target.root)
        if getattr(self.target, 'placement', 'native' if self.target.physical else 'guest') in ('boat', 'guest'):
            command += ' --disposable'
        args = self.target.physical.ssh_arguments + [self.target.physical.endpoint] if self.target.physical else lab.ssh_args(self.target.run)
        self.process = subprocess.Popen(args + [command], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, bufsize=0)
        try:
            line, self.buffer = read_line(self.process.stdout, 8, self.buffer)
            self.identity = json.loads(line)['identity']
            self.identity['argv'] = [os.fsencode(a) for a in self.identity['argv']]
            if type(self.identity['pid']) is not int or self.identity['pid'] < 2:
                raise ValueError('invalid keeper identity')
            line, self.buffer = read_line(self.process.stdout, 8, self.buffer)
            ready = json.loads(line)
            if ready.get('blocked'):
                raise RecipeBlocked(ready['blocked'])
            if not ready.get('ready'):
                raise ValueError('clipboard snapshot unavailable')
            self.report.update(ready)
            self.report.update({k:self.identity[k] for k in ('pid','start','boot')})
            # Inspect the current clipboard policy before terminating any old
            # owner. A blocked native run must leave that owner untouched.
            recovery = ('import json,runpy\nn=runpy.run_path(' + repr(path) +
                        ',run_name="clipboard_recovery")\nprint(json.dumps(n["reap_legacy"](' +
                        repr(self.legacy) + ') + n["reap_current"](' + str(self.identity['pid']) + ')))\n')
            result = self.target.execute("python3 - <<'SD_CLIP_REAP'\n" + recovery + 'SD_CLIP_REAP\n', timeout=12)
            if result['returncode']:
                raise RecipeBlocked('exact clipboard orphan recovery failed; no resident started')
            self.report['reaped_pids'] = json.loads(result['stdout'])
        except (OSError, ValueError, RuntimeError, RecipeBlocked) as error:
            self.close()
            if isinstance(error, RecipeBlocked):
                raise
            raise RecipeBlocked('clipboard preservation unavailable; no resident started') from error

    def close(self):
        import subprocess
        from .api import RecipeBlocked
        if self.process is None:
            return
        process = self.process
        if not process.stdin.closed:
            process.stdin.close()  # EOF requests restoration even if the runner is killed.
        try:
            process.wait(timeout=8)
            if process.returncode:
                raise RecipeBlocked('clipboard keeper failed to restore and exit')
            self.report['restored_and_stopped'] = bool(self.report.get('ready'))
        except subprocess.TimeoutExpired:
            raise RecipeBlocked('clipboard keeper restoration deadline exceeded') from None
        finally:
            # Transport exit is not proof about its remote child. Pin and signal
            # the exact guest PID if it remains, even on an SSH failure path.
            try:
                if self.identity:
                    import inspect
                    source = 'import os,signal,select,stat,time\nfrom pathlib import Path\n' + ''.join(
                        inspect.getsource(function) + '\n' for function in (private, process_identity, stop_exact))
                    code = ('namespace={"__name__":"clipboard_cleanup"}\nexec(compile(' + repr(source) +
                            ',"clipboard_cleanup","exec"),namespace)\nnamespace["stop_exact"](' + repr(self.identity) + ')\n')
                    result = self.target.execute("python3 - <<'SD_CLIP_STOP'\n" + code + 'SD_CLIP_STOP\n', timeout=8)
                    if result['returncode']:
                        raise RecipeBlocked('exact clipboard keeper cleanup failed')
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)
                process.stdout.close()
                self.process = None


def read_line(stream, timeout, buffer=b''):
    """Read one bounded metadata line, never a clipboard payload."""
    import time
    deadline = time.monotonic() + timeout
    while b'\n' not in buffer:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([stream], [], [], remaining)[0]:
            raise RuntimeError('clipboard readiness timed out')
        data = os.read(stream.fileno(), 4096)
        if not data or len(buffer) + len(data) > 16384:
            raise RuntimeError('clipboard readiness unavailable')
        buffer += data
    return buffer.split(b'\n', 1)


if __name__ == '__main__':
    serve(sys.argv[1], '--disposable' in sys.argv[2:])
