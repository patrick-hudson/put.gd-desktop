"""The watchdog: findings, hangs, crashes, stuck grabs, memory growth and log criticals."""
import collections
import glob
import json
import os
import re
import threading
import time

from gui_fuzz.world import rss_kib, run

CRITICAL = re.compile(r'(QObject::|QWidget::|QPainter::|QBackingStore|QPixmap::|QFont::|QTimer::|QLayout::|QSocketNotifier|'
                      r'ASSERT|Assertion|terminate called|Segmentation|Fatal|fatal|Critical|CRITICAL|Cannot |cannot |'
                      r'Unhandled|uncaught|double free|corrupt|stack smashing|invalid pointer|bad_alloc)')
# Known lab/desktop noise that says nothing about put.gd (extend with the exact text, never a wide pattern).
NOISE = re.compile(r'(Failed to register with host portal|Could not register app ID|QFSFileEngine::open: No file name|'
                   r'\[h264_nvenc @ 0x[0-9a-f]+\] Cannot load libcuda\.so\.1|\[(VAAPI|AVHWDeviceContext) @ 0x[0-9a-f]+\] )')


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
        self.last_close = 0.0

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
        for path in glob.glob(os.path.expanduser('~/.local/state/putgd/logs/*.jsonl')):  # earlier seeds' lines were judged then
            try:
                self.log_offsets[path] = os.path.getsize(path)
            except OSError:
                pass

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
                self.findings.add('shell_unresponsive', '%s has not reported for %.1f s (%d errors, last: %s)' % (self.desk.shell_label, now - stamp, getattr(self.world.fence, 'errors', 0),
                                                                                                getattr(self.world.fence, 'last_error', '')), fatal=True)
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
                                      extra=self.shell_evidence())
                    self.session[name] = fresh[0]
                    self.shell_down_since = None
                    self.shell_restarted_at = now
                    continue
                self.findings.add('session_died', '%s (pid %d) is gone' % (name, pid), fatal=True, extra=self.shell_evidence())
        restarted = getattr(self, 'shell_restarted_at', None)
        if restarted and now - restarted > 30:
            self.shell_restarted_at = None
            problem = self.desk.shell_restart_check(now)
            if problem:
                self.findings.add('tray_lost_after_shell_restart', problem)
        if self.read_vt() != self.vt:
            self.findings.add('vt_changed', 'tty0 active %s -> %s' % (self.vt, self.read_vt()), fatal=True)
        if self.resident and not os.path.exists('/proc/%d' % self.resident):
            self.findings.add('resident_died', 'resident pid %d is gone' % self.resident, fatal=True)
        for other in self.world.snapshot()['windows']:  # a crash viewer opens after a desktop crash and takes the focus
            if other['cls'].startswith(self.desk.crash_viewers) and other['pid'] != os.getpid():
                try:
                    os.kill(other['pid'], 9)
                except OSError:
                    pass
        chrome = self.world.snapshot().get('chrome') or {}
        if chrome.get('modal') and not chrome.get('overview') and not self.world.overlays():
            self.modal_since = getattr(self, 'modal_since', None) or now
            if now - self.modal_since > 8:  # put.gd's selector pushes a modal grab; nothing of put.gd's is up, so it was not popped
                self.findings.add('shell_modal_stuck', 'Main.modalCount is %s for %.0f s with no put.gd overlay up' % (chrome['modal'], now - self.modal_since),
                                  extra={'chrome': chrome})
        else:
            self.modal_since = None
        windows = self.world.toplevels()
        active = self.world.active()
        cls = (active or {}).get('cls', '')
        # Focus on the desktop shell (its layer, a notification toast) is the user's normal state: the walk's next click returns it.
        if windows and not self.world.overlays() and not (active and self.world.mine(active)) and cls not in self.desk.shell_focus_classes:
            self.focus_lost_since = self.focus_lost_since or now
            self.paused_walk.set()
            # An app the desktop started for one of put.gd's Open actions (a file with no handler opens the app store, a link a browser)
            # holds the focus: close it as its own title bar would and count it; the walk cannot click it.
            if (active and active['normal'] and not active['popup'] and now - self.focus_lost_since > 2 and now - self.last_close > 2
                    and not self.world.mine(active)):
                self.last_close = now
                if self.desk.close_window(active):
                    self.foreign[cls] = self.foreign.get(cls, 0) + 1
                    self.log.write({'op': 'note', 'action': 'closed foreign window', 'cls': cls, 'title': active.get('title', '')[:60]})
            if now - self.focus_lost_since > 12:
                self.findings.add('focus_left_putgd', 'focus is on %s for %.0f s while put.gd shows %d windows' % (cls, now - self.focus_lost_since, len(windows)))
        else:
            self.focus_lost_since = None
            self.paused_walk.clear()

    def shell_evidence(self):
        """Who crashed the shell: its core dump and every journal line that names put.gd or its extension."""
        rc, journal, _ = run(['journalctl', '--user', '-b', '--no-pager', '-n', '4000', '-o', 'short-precise'], 20)
        lines = [line for line in journal.splitlines() if re.search(r'putgd|put\.gd|guifuzz', line, re.I)]
        rc, info, _ = run(['coredumpctl', 'info', '--no-pager', '-1'], 20)
        return {'putgd_journal_lines': lines[-40:], 'last_coredump': info[-3000:]}

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
            self.findings.add('core_dump', out.splitlines()[-1][:200] if out else 'core dump', fatal=False)
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
                if record.get('level') not in ('warning', 'error', 'critical', 'fatal'):
                    continue
                message = record.get('message', '')
                if NOISE.search(message):
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
