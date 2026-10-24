"""Checked Linux lifecycle for an isolated campaign resident and History window."""
import ctypes as C
import os
from pathlib import Path
import select
import signal
import subprocess
import time

from bench_ui_process import EvidenceError, identity, same_identity, stat_fields


def require(value, code):
    if not value:
        raise EvidenceError(code)


def generation_alive(expected):
    try:
        root = Path('/proc') / str(expected['pid'])
        fields = stat_fields((root / 'stat').read_text())
        return fields['startTicks'] == expected['startTicks'] and root.stat().st_uid == expected['uid']
    except FileNotFoundError:
        return False


def profile_inventory(runtime, data, group):
    rows = []
    for root in Path('/proc').iterdir():
        if not root.name.isdigit():
            continue
        try:
            if root.stat().st_uid != os.getuid():
                continue
            fields = stat_fields((root / 'stat').read_text())
            same_group = os.getpgid(int(root.name)) == group
            try:
                env = (root / 'environ').read_bytes().split(b'\0')
            except PermissionError:
                require(same_group or not (root / 'comm').read_text().startswith('putgd'),
                        'product_cleanup_inventory_permission_denied')
                env = []
            if (same_group or f'XDG_RUNTIME_DIR={runtime}'.encode() in env or f'XDG_DATA_HOME={data}'.encode() in env):
                rows.append({'pid': int(root.name), 'startTicks': fields['startTicks'], 'uid': root.stat().st_uid})
        except (FileNotFoundError, ProcessLookupError):
            pass
    return rows


def cleanup(process, known, runtime, data, socket, quit_request, grace=15, drain=5):
    """Never equate forced cleanup, exit7 or remaining descendants with success."""
    receipt = {'gracefulRequest': False, 'exitCode': None, 'forced': False,
               'remaining': [], 'socketAbsent': False, 'passed': False, 'errors': [],
               'inventoryComplete': True, 'absenceVerified': False}
    owned = {(row['pid'], row['startTicks']): row for row in known}
    def discover():
        try:
            for row in profile_inventory(runtime, data, process.pid):
                owned[(row['pid'], row['startTicks'])] = row
        except (OSError, EvidenceError) as error:
            receipt['inventoryComplete'] = False
            receipt['errors'].append(str(error) if isinstance(error, EvidenceError) else type(error).__name__)
        # An incomplete unrelated inventory must neither suppress the known
        # resident's graceful request nor prevent its authenticated cleanup.
        return [row for row in owned.values() if generation_alive(row)]
    try:
        discover()
        if process.poll() is None:
            quit_request()
            receipt['gracefulRequest'] = True
        else:
            receipt['errors'].append('resident_exited_before_graceful_request')
        try:
            receipt['exitCode'] = process.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            receipt['errors'].append('resident_graceful_exit_timeout')
        deadline = time.monotonic() + drain
        while discover() and time.monotonic() < deadline:
            time.sleep(.05)
    except (OSError, EvidenceError, subprocess.SubprocessError) as error:
        receipt['errors'].append(str(error) if isinstance(error, EvidenceError) else type(error).__name__)
    # Always attempt cleanup of authenticated owned generations, even after a
    # discovery/request failure. A signal is failure context, never a pass.
    remaining = discover()
    for sig in (signal.SIGTERM, signal.SIGKILL):
        if not remaining:
            break
        receipt['forced'] = True
        for row in remaining:
            try:
                fd = os.pidfd_open(row['pid'])
                try:
                    if generation_alive(row):
                        signal.pidfd_send_signal(fd, sig)
                finally:
                    os.close(fd)
            except (ProcessLookupError, FileNotFoundError):
                pass
        try:
            receipt['exitCode'] = process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            pass
        deadline = time.monotonic() + 1
        while any(generation_alive(row) for row in remaining) and time.monotonic() < deadline:
            time.sleep(.05)
        remaining = [row for row in owned.values() if generation_alive(row)]
    receipt['remaining'] = discover()
    receipt['socketAbsent'] = not Path(socket).exists()
    receipt['absenceVerified'] = receipt['inventoryComplete'] and not receipt['remaining']
    receipt['passed'] = (receipt['gracefulRequest'] and receipt['exitCode'] == 0 and not receipt['forced']
                         and receipt['absenceVerified'] and receipt['socketAbsent'] and not receipt['errors'])
    receipt['inventoryDefinition'] = 'observed PID/start/UID generations + resident process group + exact isolated runtime/data environment; no command lines exported'
    return receipt


class HistoryGuard:
    """Kernel exit readiness + queued X11 unmap/destroy events cannot be sampled away."""
    def __init__(self, owner, window, resident_pid):
        from campaign_ui import Drawable
        self.owner, self.resident_pid = owner, resident_pid
        self.pidfd = os.pidfd_open(owner['pid'])
        self.watch = Drawable(window, owner)
        self.x = self.watch.x
        self.x.XSelectInput.argtypes = [C.c_void_p, C.c_ulong, C.c_long]
        self.x.XSync.argtypes = [C.c_void_p, C.c_int]
        self.x.XPending.argtypes = [C.c_void_p]
        self.x.XNextEvent.argtypes = [C.c_void_p, C.c_void_p]
        self.x.XSelectInput(self.watch.display, self.watch.window, 1 << 17)
        self.x.XSync(self.watch.display, 0)
        self.checks = 0
        try:
            self.watch.pixels([0, 0, 1, 1])
            self.check()
        except BaseException:
            self.close()
            raise

    def events(self):
        self.x.XSync(self.watch.display, 0)
        result = []
        while self.x.XPending(self.watch.display):
            event = (C.c_long * 24)()
            self.x.XNextEvent(self.watch.display, C.byref(event))
            result.append(C.cast(event, C.POINTER(C.c_int))[0])
        return result

    def check(self):
        require(not select.select([self.pidfd], [], [], 0)[0], 'history_helper_exited_during_interval')
        require(same_identity(identity(self.owner['pid']), self.owner), 'history_helper_generation_changed')
        require(stat_fields(Path(f"/proc/{self.owner['pid']}/stat").read_text())['ppid'] == self.resident_pid,
                'history_helper_parent_changed')
        require(not any(event in (17, 18) for event in self.events()), 'history_window_vanished_during_interval')
        self.checks += 1

    def close(self):
        self.watch.close()
        os.close(self.pidfd)


def normal_helper_exit(status, previous_generation):
    return (status.get('code') == 0 and status.get('status') == 'normal'
            and status.get('generation', 0) > previous_generation)
