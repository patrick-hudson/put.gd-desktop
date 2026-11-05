"""Aimed-signal sweep for surface-driver cleanup paths (#815, #816, #762).

usage: sweep.py TREE SCENARIO SIGNAL TRIALS [MAX_US]

A POSIX timer delivers SIGNAL 0..MAX_US microseconds after it is armed, as
close as possible before each critical cleanup entry. Fake targets, no
network. Handlers behave like run.py's: SIGALRM raises the recipe deadline,
SIGTERM/SIGHUP record `interrupted` and raise, SIGINT raises KeyboardInterrupt.

A trial is bad if the cleanup was skipped (and, for input, later input is not
refused), the handler did not run exactly once, its exception was neither
raised nor reviewed, a mask/handler/transaction leaked, or a physical release
was sent for a press that never went out (#736).
"""
import collections, ctypes, json, signal, sys, tempfile, time, types
from pathlib import Path

TREE, SCENARIO, SIGNAME, TRIALS = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
MAX_US = float(sys.argv[5]) if len(sys.argv) > 5 else 150
SIG = getattr(signal, 'SIG' + SIGNAME)
sys.path.insert(0, TREE)
libc = ctypes.CDLL(None, use_errno=True)
libc.prctl(29, 1, 0, 0, 0)  # PR_SET_TIMERSLACK: 1 ns

from tools.lab import lab
from tools.lab.surface_driver import browser, context as context_module
from tools.lab.surface_driver.api import Recipe, RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.fixtures import FACTORIES
from tools.lab.surface_driver.run import run_recipes
try:
    from tools.lab.surface_driver import critical
except ImportError:
    critical = None
lab.verify_owned_guest = lambda run: None


class SigEvent(ctypes.Structure):
    _fields_ = [('value', ctypes.c_void_p), ('signo', ctypes.c_int), ('notify', ctypes.c_int), ('pad', ctypes.c_int * 12)]
class TS(ctypes.Structure):
    _fields_ = [('s', ctypes.c_long), ('ns', ctypes.c_long)]
class ITS(ctypes.Structure):
    _fields_ = [('interval', TS), ('value', TS)]
timer = ctypes.c_void_p()
assert libc.timer_create(1, ctypes.byref(SigEvent(None, SIG, 0)), ctypes.byref(timer)) == 0


def arm(us):
    ns = max(1, int(us * 1000))
    libc.timer_settime(timer, 0, ctypes.byref(ITS(TS(0, 0), TS(ns // 10**9, ns % 10**9))), None)


def disarm():
    libc.timer_settime(timer, 0, ctypes.byref(ITS(TS(0, 0), TS(0, 0))), None)


def spin(us):
    end = time.perf_counter() + us / 1e6
    while time.perf_counter() < end:
        pass


# The press-release pause and drag-step sleeps would swallow every offset.
context_module.time = types.SimpleNamespace(**{k: getattr(time, k) for k in dir(time) if not k.startswith('_')})
context_module.time.sleep = lambda seconds: None

calls = []
interrupted = []
TOKEN = {'ALRM': 'recipe deadline exceeded', 'TERM': 'runner interrupted by SIGTERM',
         'HUP': 'runner interrupted by SIGHUP', 'INT': 'KeyboardInterrupt'}[SIGNAME]


def handler(signum, _frame):
    calls.append(signum)
    if signum == signal.SIGINT:
        raise KeyboardInterrupt('sweep interrupt')
    if signum == signal.SIGALRM:
        raise RecipeFailed('recipe deadline exceeded: sweep')
    interrupted.append(signum)
    raise RecipeFailed('runner interrupted by ' + signal.Signals(signum).name)


def mentions(error, depth=0):
    if error is None or depth > 6:
        return False
    text = type(error).__name__ + ': ' + str(error) + ' '.join(getattr(error, '__notes__', []))
    return TOKEN in text or mentions(error.__context__, depth + 1) or mentions(error.__cause__, depth + 1)


ENV = dict(desktop='d', host='boat', placement='guest', package='deb', destination='local')
OUT = tempfile.mkdtemp(prefix='sigsweep-')


def make_ctx(target):
    ctx = DriverContext(target, ENV, {}, '/tmp', OUT)
    ctx._deadline = time.monotonic() + 600
    ctx._dimensions = lambda head: (100, 100)
    return ctx


def refuses(ctx):
    """Later input refused: release unconfirmed and the run withholds input."""
    try:
        ctx._guard()
    except RecipeBlocked as error:
        return 'release was not confirmed' in str(error)
    except Exception:
        return False
    return False


class Input:
    """Fake input target; ``log`` records press/release/move in order."""

    def __init__(self, physical, arm_on, offset):
        self.physical = object() if physical else None
        self.run = Path('/nonexistent')
        self.log, self.arm_on, self.offset = [], arm_on, offset

    def heartbeat(self, force=False):
        pass

    def _record(self, what, final=True):
        self.log.append(what)
        if what == self.arm_on and final:
            arm(self.offset)

    def pointer(self, x, y, w, h, down=None, button='left', head=0):
        self._record('press' if down else 'release' if down is False else 'move', final=(down is not None or x == 90))

    def key_state(self, codes, down):
        self._record('press' if down else 'release')

    def events(self, events, head=None):
        for event in events:
            if event['type'] == 'btn':
                self._record('press' if event['data']['down'] else 'release')


def input_trial(kind, offset):
    """Returns (released, ctx, target, problems). Fakes lack release_pointer:
    both trees release with pointer(..., down=False)."""
    physical = kind == 'click-physical'
    arm_on = {'click-guest': 'press', 'click-physical': 'press', 'drag': 'move', 'held': None, 'wheel': 'press'}[kind]
    target = Input(physical, arm_on, offset)
    ctx = make_ctx(target)
    holder = {'ctx': ctx, 'target': target}
    ctx._guard = lambda *a, **k: None
    try:
        if kind.startswith('click'):
            ctx._pointer_click(5, 5, 'left', 0, guarded=True)
        elif kind == 'drag':
            ctx.drag((10, 10), (90, 90), steps=4)
        elif kind == 'held':
            with ctx.held_keys('Shift'):
                arm(offset)
        elif kind == 'wheel':
            ctx.scroll(5, 5, 1)
    finally:
        del ctx._guard
        CURRENT.update(holder)
    return holder


CURRENT = {}


def browser_trial(kind, offset):
    actions = CURRENT['actions'] = []

    class T:
        physical = None
        run = '/nonexistent'

        def heartbeat(self, force=False):
            pass

        def execute(self, script, timeout=60):
            action = script.split()[2].strip("'")
            actions.append(action)
            if action == 'prepare':
                if kind == 'browser-setup':
                    arm(offset)
                    return {'returncode': 1, 'stdout': '', 'stderr': ''}
                return {'returncode': 0, 'stdout': '{"directory": "/x", "port": 1}'}
            spin(20)  # The restore takes a little while.
            actions.append(action + '-done')
            return {'returncode': 0, 'stdout': '{"restored": true}'}
    ctx = CURRENT['ctx'] = make_ctx(T())
    with browser.default_browser(ctx):
        arm(offset)


def fixture_trial(offset):
    done = CURRENT['done'] = []
    target = types.SimpleNamespace(root='/tmp/sd-sweep', physical=None, run='/nonexistent', close=lambda: [])
    ctx = CURRENT['ctx'] = make_ctx(target)

    def factory(scope):
        def finalizer():
            spin(20)
            done.append('finalizer')
        scope.defer('slow', finalizer)
        arm(offset)
        raise RecipeBlocked('setup failed')
    FACTORIES['sweep'] = factory
    ctx.fixture('sweep')


def runner_trial(offset):
    done, ran = CURRENT['done'], CURRENT['ran'] = [], []
    reviews, notes = [], []
    def cleanup():
        spin(20)
        done.append('finalizer')
        return []
    ctx = CURRENT['ctx'] = types.SimpleNamespace(begin=lambda r: None, frame=lambda *a: None, frames=[],
                                                 reviews=reviews, notes=notes, review=reviews.append,
                                                 note=notes.append, _cleanup_fixtures=cleanup)
    def first(c):
        arm(offset)
    def second(c):
        ran.append('second')
    with tempfile.TemporaryDirectory() as directory:
        CURRENT['rows'] = run_recipes(ctx, [Recipe(('editor.undo',), first), Recipe(('editor.undo',), second)],
                                      dict(ENV, build='abcdef'), directory, 'sweep', Path(directory) / 'coverage.json')


INPUT = ('click-guest', 'click-physical', 'drag', 'held', 'wheel')
tally, bad = collections.Counter(), []
for index in range(TRIALS):
    offset = MAX_US * index / max(1, TRIALS - 1)
    calls.clear(); interrupted.clear(); CURRENT.clear()
    signal.signal(SIG, handler)
    raised = late = None
    problems = []
    try:
        try:
            if SCENARIO in INPUT:
                input_trial(SCENARIO, offset)
            elif SCENARIO in ('browser-exit', 'browser-setup'):
                browser_trial(SCENARIO, offset)
            elif SCENARIO == 'fixture-setup':
                fixture_trial(offset)
            elif SCENARIO == 'runner-cleanup':
                runner_trial(offset)
            else:
                raise SystemExit('unknown scenario')
        except BaseException as error:
            raised = error
        spin(300)  # A signal still on its way lands here, outside the cleanup.
    except BaseException as error:
        late = error
    disarm()
    ctx = CURRENT.get('ctx')
    if SCENARIO in INPUT:
        target = CURRENT['target']
        presses, releases = target.log.count('press'), target.log.count('release')
        ok = releases >= presses or refuses(ctx)  # Released, or later input refuses.
        if target.physical and releases > presses:
            problems.append('release-without-press')
    elif SCENARIO.startswith('browser'):
        ok = ('restore-done' if SCENARIO == 'browser-exit' else 'recover-done') in CURRENT['actions']
        if browser._ACTIVE:
            problems.append('active-leak')
            browser._ACTIVE.clear()
    elif SCENARIO == 'fixture-setup':
        manager = getattr(ctx, '_fixture_manager', None)
        ok = CURRENT['done'] == ['finalizer'] and not (manager and manager.errors)
    else:
        # One fixture cleanup per recipe that ran; a deadline does not stop the run.
        ok = CURRENT['done'] == ['finalizer'] * (2 if CURRENT['ran'] else 1)
        if SIG in (signal.SIGTERM, signal.SIGHUP) and CURRENT['ran']:
            problems.append('run-continued-after-termination')
    reviews = list(getattr(ctx, 'reviews', [])) + list(getattr(ctx, 'notes', []))
    reviews += [(row.note or '') + (row.review_reason or '') for row in CURRENT.get('rows', [])]
    if SIGNAME == 'INT':
        surfaced = isinstance(raised, KeyboardInterrupt) or isinstance(late, KeyboardInterrupt)
    else:
        surfaced = mentions(raised) or mentions(late) or any(TOKEN in r for r in reviews)
    if not ok:
        problems.append('cleanup-skipped')
    if SCENARIO != 'runner-cleanup' and len(calls) != 1:
        problems.append('handler-ran-%d' % len(calls))  # run.py installs its own there.
    if not surfaced:
        problems.append('swallowed')
    if SIG in set(signal.pthread_sigmask(signal.SIG_BLOCK, ())):
        problems.append('left-blocked')
    if SIG in set(signal.sigpending()):
        problems.append('left-pending')
    if signal.getsignal(SIG) is not handler:
        problems.append('handler-not-restored')
    if critical is not None and (critical._state.depth or critical._state.pending):
        problems.append('critical-state-leak')
        critical._state.depth, critical._state.pending = 0, []
    key = ' '.join(problems) or 'ok'
    tally[key] += 1
    if problems and len(bad) < 6:
        bad.append((round(offset, 1), problems, type(raised).__name__, str(raised)[:80], type(late).__name__))

    signal.pthread_sigmask(signal.SIG_SETMASK, set())
    while signal.sigtimedwait({SIG}, 0) is not None:
        pass

result = {'tree': TREE, 'scenario': SCENARIO, 'signal': 'SIG' + SIGNAME, 'trials': TRIALS, 'max_us': MAX_US,
          'python': sys.version.split()[0], 'bad': sum(n for k, n in tally.items() if k != 'ok'),
          'tally': dict(tally), 'examples': bad}
print(json.dumps(result))
