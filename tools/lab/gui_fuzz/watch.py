"""The watchdog: findings, hangs, crashes, stuck grabs, memory growth and log criticals."""
import collections
import datetime
import glob
import json
import os
import re
import subprocess
import threading
import time

from gui_fuzz.world import PUTGD_COMMS, rss_kib, run

CRITICAL = re.compile(r'(QObject::|QWidget::|QPainter::|QBackingStore|QPixmap::|QFont::|QTimer::|QLayout::|QSocketNotifier|'
                      r'ASSERT|Assertion|terminate called|Segmentation|Fatal|fatal|Critical|CRITICAL|Cannot |cannot |'
                      r'Unhandled|uncaught|double free|corrupt|stack smashing|invalid pointer|bad_alloc)')
# Known lab/desktop noise that says nothing about put.gd (extend with the exact text, never a wide pattern).
NOISE = re.compile(r'(Failed to register with host portal|Could not register app ID|QFSFileEngine::open: No file name)')
# ffmpeg's encoder probes in a VM without a GPU: these three exact messages and nothing wider (GNOME and Cinnamon runs).
PROBE_NOISE = re.compile(r'^(\[h264_nvenc @ 0x[0-9a-f]+\] Cannot load libcuda\.so\.1|'
                         r'\[VAAPI @ 0x[0-9a-f]+\] libva: /usr/lib/x86_64-linux-gnu/dri/virtio_gpu_drv_video\.so init failed|'
                         r'\[VAAPI @ 0x[0-9a-f]+\] Failed to initialise VAAPI connection: 2 \(resource allocation failed\)\.)$')
CORE_LINE = re.compile(r'\s(\d+)\s+\d+\s+\d+\s+SIG\w+\s+\S+\s+(/\S+)')


SHELL_ERROR = re.compile(r'(JS ERROR|JS LOG: .*[Ee]rror|Gjs-CRITICAL|Gjs-WARNING|CRITICAL|TypeError|ReferenceError|RangeError|SyntaxError|is not a function|'
                         r'is undefined|Stack trace|Window manager warning|assertion|Extension .* error|Failed to|g_object_|GLib-GObject)')


def norm(message):
    return re.sub(r'0x[0-9a-f]+|\d+', 'N', message)[:200]


class Findings:
    def __init__(self, outdir, log, world, seed):
        self.dir = os.path.join(outdir, 'findings')
        os.makedirs(self.dir, exist_ok=True)
        self.log, self.world, self.seed = log, world, seed
        self.seen = {}
        self.fatal = threading.Event()
        self.marks = open(os.path.join(outdir, 'marks.log'), 'a', buffering=1)
        self.lock = threading.Lock()

    def add(self, kind, detail, fatal=False, extra=None):
        key = (kind, norm(detail)[:80])
        with self.lock:
            self.seen[key] = self.seen.get(key, 0) + 1
            if self.seen[key] > 1:
                return None
            number = len(self.seen)
        now = time.time()
        rc, journal, _ = run(['journalctl', '--user', '-n', '60', '--no-pager', '-o', 'short-precise'], 10)
        rc, cores, _ = run(['coredumpctl', 'list', '--no-pager', '--no-legend'], 10)
        record = {'kind': kind, 'detail': detail, 'time': now, 'seed': self.seed, 'step': self.log.step, 'fatal': fatal,
                  'recent_ops': self.log.tail(), 'windows': self.world.snapshot()['windows'], 'outputs': self.world.outputs(),
                  'processes': {str(p): [c, rss_kib(p)] for p, c in self.world.pids().items()},
                  'coredumps': cores.splitlines()[-6:], 'journal_tail': journal.splitlines()[-60:], 'extra': extra}
        path = os.path.join(self.dir, '%03d-%s.json' % (number, kind))
        with open(path, 'w') as stream:
            json.dump(record, stream, indent=1)
        self.marks.write('%.3f %s %s\n' % (now, kind, detail[:120].replace('\n', ' ')))
        print('FINDING', kind, detail[:160], flush=True)
        if fatal:
            self.fatal.set()
        return path


class Watchdog(threading.Thread):
    def __init__(self, outdir, world, findings, log):
        super().__init__(daemon=True)
        self.dir, self.world, self.findings, self.log = outdir, world, findings, log
        self.desk = world.desk
        self.stop = threading.Event()
        self.paused_walk = threading.Event()  # set while the walk must stand still
        self.resident = None
        self.session = {}
        self.vt = self.read_vt()
        self.focus_lost_since = None
        self.cores = self.count_cores()
        self.pings = {'n': 0, 'slow': 0, 'max_ms': 0}
        self.memory = open(os.path.join(outdir, 'memory.jsonl'), 'a', buffering=1)
        self.log_offsets = {}
        self.warnings = {}
        self.fence_stale_since = None
        self.last = {'mem': 0, 'ping': 0, 'core': 0, 'log': 0, 'shell': 0}
        self.shell_seen = collections.deque(maxlen=4000)
        self.shell_warnings = {}
        self.foreign = {}
        self.quits = 0
        self.last_close = 0.0
        self.born = {}  # window id -> (first seen, the time of the actor's last input then), or None for a window that was there at the start
        self.pending_close = {}  # window id -> (class, asked at)

    @staticmethod
    def read_vt():
        try:
            with open('/sys/class/tty/tty0/active') as stream:
                return stream.read().strip()
        except OSError:
            return None

    @staticmethod
    def count_cores():
        rc, out, _ = run(['coredumpctl', 'list', '--no-pager', '--no-legend'], 10)
        return len(out.splitlines())

    def pid_of(self, comm):
        rc, out, _ = run(['pgrep', '-u', str(os.getuid()), '-x', comm], 5)
        return [int(p) for p in out.split()]

    def prime(self):
        residents = [p for p, c in self.world.pids().items() if c == 'putgd']
        self.resident = residents[0] if residents else None
        self.session = {name: (self.pid_of(name) or [None])[0] for name in self.desk.session_procs}
        if self.desk.skip_old_logs:
            for path in glob.glob(os.path.expanduser('~/.local/state/putgd/logs/*.jsonl')):  # earlier seeds' lines were judged then
                try:
                    self.log_offsets[path] = os.path.getsize(path)
                except OSError:
                    pass
        for window in self.world.snapshot()['windows']:  # what was on screen before the walk is not something the walk opened
            self.born[window['id']] = None

    def run(self):
        while not self.stop.is_set():
            now = time.time()
            try:
                self.fast_checks(now)
                if now - self.last['ping'] > 3:
                    self.last['ping'] = now
                    threading.Thread(target=self.ping, daemon=True).start()
                if now - self.last['mem'] > 15:
                    self.last['mem'] = now
                    self.sample_memory(now)
                if now - self.last['core'] > 5:
                    self.last['core'] = now
                    self.check_cores()
                if now - self.last['log'] > 4:
                    self.last['log'] = now
                    self.scan_logs()
                if self.desk.journal_args and now - self.last['shell'] > 5:
                    self.last['shell'] = now
                    self.scan_shell_journal()
            except Exception as error:  # the watchdog must outlive its own bugs
                print('watchdog error', repr(error), flush=True)
            self.stop.wait(0.5)

    def fast_checks(self, now):
        stamp = self.world.fence.get()[1]
        if now - stamp > 3:
            self.fence_stale_since = self.fence_stale_since or now
            if now - self.fence_stale_since > 3:
                detail = '%s has not reported for %.1f s' % (self.desk.shell_label, now - stamp)
                if hasattr(self.world.fence, 'errors'):
                    detail += ' (%d errors, last: %s)' % (self.world.fence.errors, self.world.fence.last_error)
                self.findings.add(self.desk.unresponsive_kind, detail, fatal=True)
        else:
            self.fence_stale_since = None
        for name, pid in list(self.session.items()):
            if pid and not os.path.exists('/proc/%d' % pid):
                fresh = self.pid_of(name)
                restartable = name in self.desk.restartable
                if restartable and not fresh:
                    # the shell may be restarted by systemd or the session: wait for it before calling the session dead
                    self.shell_down_since = getattr(self, 'shell_down_since', None) or now
                    if now - self.shell_down_since < 20:
                        continue
                if restartable and fresh:
                    # A shell crash on output churn is the desktop's own bug (seen: QWaylandXdgSurface::applyConfigure in plasmashell 6.8.2).
                    self.findings.add('desktop_%s_restarted' % name.replace('-', '_'), '%s %d is gone, %d runs now' % (name, pid, fresh[0]),
                                      extra=self.shell_evidence(pid))
                    self.session[name] = fresh[0]
                    self.shell_down_since = None
                    self.shell_restarted_at = now
                    continue
                self.findings.add('session_died', '%s (pid %d) is gone' % (name, pid), fatal=True, extra=self.shell_evidence(pid))
        restarted = getattr(self, 'shell_restarted_at', None)
        if restarted and now - restarted > 30:
            self.shell_restarted_at = None
            problem = self.desk.shell_restart_check(now)
            if problem:
                self.findings.add('tray_lost_after_shell_restart', problem)
        if self.read_vt() != self.vt:
            self.findings.add('vt_changed', 'tty0 active %s -> %s' % (self.vt, self.read_vt()), fatal=True)
        if self.resident and not os.path.exists('/proc/%d' % self.resident):
            self.resident_gone(now)
        for other in self.world.snapshot()['windows']:  # a crash viewer opens after a desktop crash and takes the focus
            if other['cls'].startswith(self.desk.crash_viewers):
                self.kill_crash_viewer(other)
        self.observe_windows(now)
        chrome = self.world.snapshot().get('chrome') or {}
        if chrome.get('modal') and not chrome.get('overview') and not self.world.overlays():
            self.modal_since = getattr(self, 'modal_since', None) or now
            if now - self.modal_since > 8:  # put.gd's selector pushes a modal grab; nothing of put.gd's is up, so it was not popped
                self.findings.add('shell_modal_stuck', 'Main.modalCount is %s for %.0f s with no put.gd overlay up' % (chrome['modal'], now - self.modal_since),
                                  extra={'chrome': chrome})
        else:
            self.modal_since = None
        self.focus_step(now)

    def kill_crash_viewer(self, window):
        """Kill a crash viewer by its exact PID, and only when the PID is real, alive and a process of the expected name."""
        pid = window.get('pid') or 0
        if not isinstance(pid, int) or pid <= 0 or pid == os.getpid():
            return False
        try:
            with open('/proc/%d/comm' % pid) as stream:
                comm = stream.read().strip()
        except OSError:
            return False
        if comm not in self.desk.crash_viewer_comms:
            return False
        try:
            os.kill(pid, 9)
        except OSError:
            return False
        return True

    def observe_windows(self, now):
        """When each window first appeared, and when the actor last sent input before that."""
        for window in self.world.snapshot()['windows']:
            self.world.note_seen(window)
            if window['id'] not in self.born:
                self.born[window['id']] = (now, self.log.last_input)

    def opened_by_walk(self, window):
        """A window that appeared within 10 s after the actor's input, and was not there when the walk began."""
        birth = self.born.get(window['id'])
        return bool(birth and birth[1] > 0 and 0 <= birth[0] - birth[1] <= 10)

    def focus_step(self, now):
        windows = self.world.toplevels()
        active = self.world.active()
        cls = (active or {}).get('cls', '')
        # Focus on the desktop shell (its layer, a notification toast) is the user's normal state: the walk's next click returns it.
        if windows and not self.world.overlays() and not (active and self.world.mine(active)) and cls not in self.desk.shell_focus_classes:
            self.focus_lost_since = self.focus_lost_since or now
            self.paused_walk.set()
            if now - self.focus_lost_since > 6:
                # Recorded first, whatever happens to the window next.
                self.findings.add('focus_left_putgd', 'focus is on %s for %.0f s while put.gd shows %d windows' % (cls, now - self.focus_lost_since, len(windows)))
                if self.desk.closes_foreign:
                    self.close_foreign(active, now)
        else:
            self.focus_lost_since = None
            self.paused_walk.clear()
        self.settle_closes(now)

    def close_foreign(self, window, now):
        """Ask an app the walk's input opened (a file with no handler opens the app store, a link a browser) to close, as its title bar would.
        A window that was there before, that nobody's input explains, or that is not an ordinary window is left alone."""
        if (not window or not window['normal'] or window['popup'] or self.world.mine(window) or not self.opened_by_walk(window)
                or window['id'] in self.pending_close or now - self.last_close < 2):
            return
        self.last_close = now
        if self.desk.close_window(window):
            self.pending_close[window['id']] = (window['cls'], now)
            self.log.write({'op': 'note', 'action': 'asked foreign window to close', 'cls': window['cls'], 'title': window.get('title', '')[:60]})

    def settle_closes(self, now):
        """A close counts only once the window is gone."""
        present = {w['id'] for w in self.world.snapshot()['windows']}
        for wid, (cls, asked) in list(self.pending_close.items()):
            if wid not in present:
                self.foreign[cls] = self.foreign.get(cls, 0) + 1
                self.log.write({'op': 'note', 'action': 'closed foreign window', 'cls': cls})
                del self.pending_close[wid]
            elif now - asked > 10:
                self.log.write({'op': 'note', 'action': 'foreign window did not close', 'cls': cls})
                del self.pending_close[wid]

    def resident_gone(self, now):
        """The resident's process is gone. A restart needs the actor to have just activated the Quit item and the resident's own log to say it
        stopped with exit code 0; any other exit is a finding and ends the walk."""
        quit_asked = self.log.last_quit_at > 0 and now - self.log.last_quit_at < 15
        if self.desk.restarts_resident and quit_asked and self.clean_quit(self.resident) and self.quits < 30:
            self.restart_resident(now)
        elif self.desk.restarts_resident and self.clean_quit(self.resident):
            self.findings.add('resident_exited', 'resident pid %d exited with code 0 without the Quit item being used' % self.resident, fatal=True)
        else:
            self.findings.add('resident_died', 'resident pid %d is gone' % self.resident, fatal=True)

    def clean_quit(self, pid):
        """True when the resident's own log says it stopped with exit code 0."""
        for path in glob.glob(os.path.expanduser('~/.local/state/putgd/logs/*-%d-*.jsonl' % pid)):
            try:
                with open(path, 'rb') as stream:
                    stream.seek(0, os.SEEK_END)
                    stream.seek(max(0, stream.tell() - 4000))
                    tail = stream.read().decode('utf-8', 'replace')
            except OSError:
                continue
            if '"message":"Resident stopped"' in tail and '"exitCode":0' in tail:
                return True
        return False

    def restart_resident(self, now):
        """Start the installed resident again as the lab does (putgd-launcher --background) and wait for it to answer."""
        self.quits += 1
        self.findings.add('resident_quit', 'resident pid %d stopped with exit code 0 after the Quit item; restarted' % self.resident)
        self.paused_walk.set()
        subprocess.Popen(['putgd-launcher', '--background'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        deadline = time.time() + 40
        while time.time() < deadline:
            time.sleep(1.0)
            rc, out, _ = run(['putgd-cli', 'ui.viewState'], 8)
            fresh = [p for p, c in self.world.putgd_now().items() if c == 'putgd']
            if rc == 0 and fresh:
                self.resident = fresh[0]
                self.paused_walk.clear()
                return
        self.findings.add('resident_died', 'the resident did not come back 40 s after a clean quit', fatal=True)

    def shell_evidence(self, pid=None):
        """Who ended the shell: the journal lines that name put.gd or its extension, and the core dump of that shell's own PID when it left one."""
        rc, journal, _ = run(['journalctl', '--user', '-b', '--no-pager', '-n', '4000', '-o', 'short-precise'], 20)
        lines = [line for line in journal.splitlines() if re.search(r'putgd|put\.gd|guifuzz', line, re.I)]
        dump = ''
        if isinstance(pid, int) and pid > 0:
            rc, info, _ = run(['coredumpctl', 'info', '--no-pager', str(pid)], 20)
            if rc == 0 and ('PID: %d ' % pid) in info:
                dump = info[-3000:]
        return {'putgd_journal_lines': lines[-40:], 'coredump_of_shell_pid': dump}

    def ping(self):
        started = time.time()
        rc, out, _ = run(['putgd-cli', 'ui.viewState'], 8)
        ms = int((time.time() - started) * 1000)
        self.pings['n'] += 1
        self.pings['max_ms'] = max(self.pings['max_ms'], ms)
        if rc == -1 or ms > 5000:
            self.pings['slow'] += 1
            self.findings.add('hang', 'ui.viewState took %d ms (rc %s)' % (ms, rc), extra={'pings': self.pings})

    def sample_memory(self, now):
        rows = {str(p): [c, rss_kib(p)] for p, c in self.world.pids().items()}
        for name, pid in self.session.items():  # the shell's own size: a leak in put.gd's extension shows here
            if pid:
                rows[str(pid)] = [name, rss_kib(pid)]
        self.memory.write(json.dumps({'t': round(now, 1), 'rss': rows, 'windows': len(self.world.windows())}) + '\n')

    def check_cores(self):
        count = self.count_cores()
        if count > self.cores:
            rc, out, _ = run(['coredumpctl', 'list', '--no-pager', '--no-legend'], 10)
            if not self.desk.split_cores:
                self.findings.add('core_dump', out.splitlines()[-1][:200] if out else 'core dump', fatal=False)
            else:
                lines = [line for line in out.splitlines() if line.strip() and not line.startswith('--')]
                for line in lines[-(count - self.cores):]:
                    match = CORE_LINE.search(line)
                    pid, exe = (int(match.group(1)), os.path.basename(match.group(2))) if match else (0, '')
                    # A put.gd process (every executable it ships) dumping core is a finding; a desktop process's crash (seen: nemo-desktop
                    # on RandR output churn) is recorded apart. A line that cannot be read counts as put.gd's. Each dump is read by its own PID.
                    kind = 'core_dump' if exe in PUTGD_COMMS or not exe else 'desktop_core_dump_' + re.sub(r'\W', '_', exe)
                    rc, info, _ = run(['coredumpctl', 'info', '--no-pager', str(pid)], 20) if pid else (0, '', '')
                    self.findings.add(kind, line[:200], fatal=False, extra={'info': info[-3500:]})
            self.cores = count

    def scan_logs(self):
        for path in glob.glob(os.path.expanduser('~/.local/state/putgd/logs/*.jsonl')):
            try:
                with open(path) as stream:
                    stream.seek(self.log_offsets.get(path, 0))
                    lines = stream.readlines()
                    self.log_offsets[path] = stream.tell()
            except OSError:
                continue
            for line in lines:
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                message = record.get('message', '')
                if self.desk.verify_portal and re.search(r'portal (OpenFile|SaveFile|SaveFiles)\b', message):
                    try:  # put.gd's own log says it asked the portal for a chooser, and when
                        stamp = datetime.datetime.strptime(record['time'][:23], '%Y-%m-%dT%H:%M:%S.%f').replace(tzinfo=datetime.timezone.utc).timestamp()
                    except (KeyError, ValueError):
                        stamp = time.time()
                    self.world.expect_chooser(at=stamp)
                if record.get('level') not in ('warning', 'error', 'critical', 'fatal'):
                    continue
                if NOISE.search(message) or (self.desk.narrow_noise and PROBE_NOISE.match(message)):
                    continue
                key = norm(message)
                self.warnings[key] = self.warnings.get(key, 0) + 1
                if CRITICAL.search(message) or record.get('level') in ('critical', 'fatal'):
                    self.findings.add('log_critical', message[:300], extra={'record': record})

    def scan_shell_journal(self):
        """The shell's own journal: a script error that names put.gd (its extension) is a finding; other shell warnings are counted."""
        rc, out, _ = run(['journalctl', '--user', '--no-pager', '-o', 'short-unix', '--since', '-40s'] + list(self.desk.journal_args), 10)
        for line in out.splitlines():
            if line in self.shell_seen:
                continue
            self.shell_seen.append(line)
            message = line.split(' ', 2)[-1] if ' ' in line else line
            if not SHELL_ERROR.search(message):
                continue
            key = norm(message)
            self.shell_warnings[key] = self.shell_warnings.get(key, 0) + 1
            if re.search(r'putgd|put\.gd', message, re.I) and not re.search(r'guifuzz', message):
                self.findings.add('shell_js_error', message[:300], extra={'line': line})

    def settle_checks(self, actor):
        """After Escape: nothing of put.gd's may still grab the screen."""
        problems = []
        if self.world.overlays():
            problems.append('region overlay still up after Escape')
        if self.world.popups():
            problems.append('popup still up after Escape')
        return problems
