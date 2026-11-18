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


class Frozen:
    """A fence that always returns one snapshot."""

    def __init__(self, snapshot):
        self.snapshot = snapshot
        self.serial = 0

    def get(self):
        return self.snapshot, time.time()


class World:
    def __init__(self, fence, desk, procs=putgd_processes):
        self.fence = fence
        self.desk = desk
        self.procs = procs
        self._procs = {}
        self._procs_at = 0.0
        self.input_serials = collections.deque(maxlen=4000)  # the fence's snapshot number when each input was sent
        self.freshness = 0.04  # a reading is fresh when its snapshot was taken at least this long after it was asked for
        self.fresh_timeout = 0.3  # and is waited for this long

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
        """put.gd's own window, or a window transient for one. A portal file chooser with no put.gd parent (put.gd opens its choosers with none) is
        not put.gd's on any desktop, whatever it looks like: the fence denies it."""
        if self.owned_by_putgd(window):
            return True
        parent = window.get('tr')
        if parent:
            by_id = {x['id']: x for x in self.snapshot()['windows']}
            owner = by_id.get(parent)
            return bool(owner and self.owned_by_putgd(owner))
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

    def inside(self, x, y, margin=0):
        """The fence: a point put.gd owns. On KDE, with the overlay up, that is every output; otherwise (and always on GNOME and Cinnamon) the
        topmost window at the point must be put.gd's, so a Shell overview, panel or notification above the selector denies the point.
        With a margin the point must also lie that many pixels inside that window, never on its edge."""
        if self.overlays() and not self.desk.strict_input:
            return any(o['geom'][0] <= x < o['geom'][0] + o['geom'][2] and o['geom'][1] <= y < o['geom'][1] + o['geom'][3] for o in self.outputs())
        for w in reversed(self.snapshot()['windows']):  # the topmost window at the point must be put.gd's own
            fx, fy, fw, fh = w['frame']
            if w['min'] or not (fx <= x < fx + fw and fy <= y < fy + fh):
                continue
            return self.mine(w) and fx + margin <= x < fx + fw - margin and fy + margin <= y < fy + fh - margin
        return False

    def fresh_snapshot(self):
        """A snapshot taken after this call, or None when none arrives in time: an input decision never rests on an older one."""
        asked = time.time()
        deadline = asked + self.fresh_timeout
        while True:
            snapshot, stamp = self.fence.get()
            if stamp > deadline:
                return None  # stamped after the deadline: too late to count, and it is not looked at again
            if stamp > asked + self.freshness:
                return snapshot
            if time.time() >= deadline:
                return None
            time.sleep(0.005)

    def fresh_cursor(self):
        """The cursor as the fence sees it in a snapshot taken after this call (what the last input really did), or None."""
        snapshot = self.fresh_snapshot()
        return None if snapshot is None else tuple(snapshot['cursor'])

    def fresh_view(self):
        """This world as a snapshot taken after this call shows it, or None: windows, overlays and the active window all from that one snapshot."""
        snapshot = self.fresh_snapshot()
        if snapshot is None:
            return None
        view = World(Frozen(snapshot), self.desk, procs=self.procs)
        view._procs, view._procs_at = self._procs, self._procs_at
        view.freshness, view.fresh_timeout = self.freshness, self.fresh_timeout
        return view

    def on_output(self, x, y):
        return any(o['geom'][0] <= x < o['geom'][0] + o['geom'][2] and o['geom'][1] <= y < o['geom'][1] + o['geom'][3] for o in self.outputs())


def run(args, timeout=10):
    """A small helper command; never raises."""
    try:
        done = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return done.returncode, done.stdout, done.stderr
    except (OSError, subprocess.SubprocessError) as error:
        return -1, '', str(error)
