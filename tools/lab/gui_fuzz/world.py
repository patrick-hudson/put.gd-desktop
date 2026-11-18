"""What the fuzzer knows about the session: put.gd's processes and windows, the fence and the action log."""
import collections
import json
import os
import subprocess
import threading
import time

# Every executable put.gd ships (cmake/Executables.cmake and the launcher): a dump or a window of any of them is put.gd's.
PUTGD_COMMS = ('putgd', 'putgd-launcher', 'putgd-cli', 'putgd-ui', 'putgd-record', 'putgd-region', 'putgd-capture', 'putgd-ocr',
               'putgd-clipboard-keeper', 'putgd-s3-worker', 'putgd-b2-worker', 'putgd-http-worker')


def putgd_processes():
    """{pid: comm} for put.gd's own processes of this user (resident, UI, helpers), from /proc."""
    found = {}
    uid = os.getuid()
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        try:
            if os.stat('/proc/' + entry).st_uid != uid:
                continue
            exe = os.readlink('/proc/%s/exe' % entry)
        except OSError:
            continue
        base = os.path.basename(exe)
        if base in PUTGD_COMMS:
            found[int(entry)] = base
    return found


def rss_kib(pid):
    try:
        with open('/proc/%d/status' % pid) as stream:
            for line in stream:
                if line.startswith('VmRSS:'):
                    return int(line.split()[1])
    except OSError:
        pass
    return None


class Log:
    """JSON Lines action log: every input primitive with its time, so a run can be replayed."""

    def __init__(self, path, seed):
        self.stream = open(path, 'a', buffering=1)
        self.lock = threading.Lock()
        self.recent = collections.deque(maxlen=60)
        self.step = 0
        self.last_input = 0.0   # the time of the last input the actor sent (a window that appears soon after may be what it opened)
        self.last_quit_at = 0.0  # the time the actor last activated a Quit item
        self.t0 = time.time()
        self.write({'op': 'start', 'seed': seed, 'epoch': self.t0})

    def write(self, record):
        record = dict(record, t=round(time.time(), 3), step=self.step)
        if record.get('op') in ('move', 'btn', 'wheel', 'chord', 'text', 'cli'):
            self.last_input = record['t']
        with self.lock:
            self.recent.append(record)
            self.stream.write(json.dumps(record, separators=(',', ':')) + '\n')

    def tail(self):
        with self.lock:
            return list(self.recent)


class World:
    def __init__(self, fence, desk, procs=putgd_processes):
        self.fence = fence
        self.desk = desk
        self.procs = procs
        self._procs = {}
        self._procs_at = 0.0
        self.seen = {}  # window id -> when this fence first saw it
        self.choosers = []  # (from, until) of each chooser request put.gd made
        self.chooser_ok = set()  # portal windows accepted as put.gd's chooser

    def expect_chooser(self, seconds=15.0, at=None):
        """put.gd asked the desktop portal for a file chooser (its own log says so, with the time): a portal window that first appeared
        within a second before that and `seconds` after it is put.gd's."""
        at = time.time() if at is None else at
        self.choosers = (self.choosers + [(at - 1.0, at + seconds)])[-50:]

    def note_seen(self, window):
        self.seen.setdefault(window['id'], time.time())

    def putgd_now(self):
        """put.gd's processes read from /proc now, not from the cache."""
        self._procs, self._procs_at = self.procs(), time.time()
        return self._procs

    def pids(self):
        if time.time() - self._procs_at > 0.5:
            self._procs, self._procs_at = self.procs(), time.time()
        return self._procs

    def snapshot(self):
        return self.fence.get()[0]

    def owned_by_putgd(self, window):
        return window['pid'] in self.pids() or window['cls'].startswith('putgd')

    def mine(self, window):
        """put.gd's own window, or (on KDE) a dialog the desktop portal opened for it. On GNOME and Cinnamon a portal window counts only when it is
        transient for a put.gd window or first appeared while put.gd had a chooser request pending: a Nautilus window of the user's is not put.gd's."""
        if self.owned_by_putgd(window):
            return True
        if window['cls'] in self.desk.portal_classes and window['normal']:
            if not self.desk.verify_portal:
                return True  # KDE: put.gd's file choosers (opened with no parent when the toolkit has no window handle to export)
            return self.portal_ok(window)
        parent = window.get('tr')
        if parent:
            by_id = {x['id']: x for x in self.snapshot()['windows']}
            owner = by_id.get(parent)
            return bool(owner and self.owned_by_putgd(owner))
        return False

    def portal_ok(self, window):
        wid = window['id']
        if wid in self.chooser_ok:
            return True
        parent = window.get('tr')
        owner = {x['id']: x for x in self.snapshot()['windows']}.get(parent) if parent else None
        first = self.seen.setdefault(wid, time.time())
        if (owner and self.owned_by_putgd(owner)) or any(low <= first <= high for low, high in self.choosers):
            self.chooser_ok.add(wid)
            return True
        return False

    def is_layer(self, window):
        return window['kind'] in self.desk.layer_kinds

    def layer(self):
        """put.gd's overlay surfaces: layer-shell surfaces on KDE (the selector's dimming strips, the recording bar, the countdown pill),
        Shell actors on GNOME (the selector, the recording frame, the countdown)."""
        return [w for w in self.snapshot()['windows'] if self.mine(w) and not w['min'] and self.is_layer(w)]

    def windows(self):
        """put.gd's windows that make fair click targets, bottom to top: dashboard, editor, popups, bar, pill."""
        return [w for w in self.snapshot()['windows'] if self.mine(w) and not w['min'] and w['frame'][2] >= 24 and w['frame'][3] >= 24
                and not (self.is_layer(w) and self.overlays())]

    def overlays(self):
        """The region selector: put.gd layer surfaces that together cover most of an output."""
        layer = self.layer()
        area = sum(w['frame'][2] * w['frame'][3] for w in layer)
        outputs = self.outputs()
        if layer and outputs and area >= 0.5 * min(o['geom'][2] * o['geom'][3] for o in outputs):
            return layer
        return []

    def popups(self):
        return [w for w in self.windows() if w['popup']]

    def toplevels(self):
        return [w for w in self.windows() if not w['popup'] and not self.is_layer(w)]

    def outputs(self):
        return self.snapshot()['outputs']

    def cursor(self):
        return tuple(self.snapshot()['cursor'])

    def active(self):
        for window in self.snapshot()['windows']:
            if window['active']:
                return window
        return None

    def inside(self, x, y):
        """The fence: a point put.gd owns. On KDE, with the overlay up, that is every output; otherwise (and always on GNOME and Cinnamon) the
        topmost window at the point must be put.gd's, so a Shell overview, panel or notification above the selector denies the point."""
        if self.overlays() and not self.desk.strict_input:
            return any(o['geom'][0] <= x < o['geom'][0] + o['geom'][2] and o['geom'][1] <= y < o['geom'][1] + o['geom'][3] for o in self.outputs())
        for w in reversed(self.snapshot()['windows']):  # the topmost window at the point must be put.gd's own
            fx, fy, fw, fh = w['frame']
            if w['min'] or not (fx <= x < fx + fw and fy <= y < fy + fh):
                continue
            return self.mine(w)
        return False

    def on_output(self, x, y):
        return any(o['geom'][0] <= x < o['geom'][0] + o['geom'][2] and o['geom'][1] <= y < o['geom'][1] + o['geom'][3] for o in self.outputs())


def run(args, timeout=10):
    """A small helper command; never raises."""
    try:
        done = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return done.returncode, done.stdout, done.stderr
    except (OSError, subprocess.SubprocessError) as error:
        return -1, '', str(error)
