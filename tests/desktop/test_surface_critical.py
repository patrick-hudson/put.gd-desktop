"""Signal-safe cleanup sections (critical.py) and the releases built on them (#815)."""
import ctypes
import dis
from pathlib import Path
import signal
import sys
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.lab.surface_driver import context as context_module, critical
from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.context import DriverContext

ENV = dict(desktop='d', host='boat', placement='guest', package='deb', destination='local')


def deadline(_signum, _frame):
    raise RecipeFailed('recipe deadline exceeded: sweep')


class Handlers(unittest.TestCase):
    def setUp(self):
        self.previous = {sig: signal.signal(sig, deadline) for sig in (signal.SIGALRM, signal.SIGTERM, signal.SIGHUP)}
        self.addCleanup(self.restore)

    def restore(self):
        signal.setitimer(signal.ITIMER_REAL, 0)
        for sig, handler in self.previous.items():
            signal.signal(sig, handler)
        signal.pthread_sigmask(signal.SIG_UNBLOCK, critical.SIGNALS)
        self.assertEqual((critical._state.depth, critical._state.pending), (0, []))

    def assertClean(self):
        self.assertFalse(set(signal.pthread_sigmask(signal.SIG_BLOCK, ())) & set(critical.SIGNALS))
        self.assertFalse(set(signal.sigpending()) & set(critical.SIGNALS))
        self.assertEqual((critical._state.depth, critical._state.pending), (0, []))


def timer_slack():
    try:
        ctypes.CDLL(None).prctl(29, 1, 0, 0, 0)  # PR_SET_TIMERSLACK: 1 ns, for aimed alarms.
    except (OSError, AttributeError):
        pass


class Input:
    def __init__(self, physical, offset, arm_on='press'):
        self.physical = object() if physical else None
        self.run = Path('/nonexistent')
        self.log, self.offset, self.arm_on = [], offset, arm_on

    def heartbeat(self, force=False):
        pass

    def pointer(self, x, y, w, h, down=None, button='left', head=0):
        self.log.append('press' if down else 'release' if down is False else 'move')
        if self.log[-1] == self.arm_on:
            signal.setitimer(signal.ITIMER_REAL, self.offset)

    def key_state(self, codes, down):
        self.log.append('press' if down else 'release')


class AimedRelease(Handlers):
    """#815: a recipe alarm landing as the release starts never loses it."""
    TRIALS = 200

    def setUp(self):
        super().setUp()
        timer_slack()
        fast = SimpleNamespace(**{k: getattr(time, k) for k in dir(time) if not k.startswith('_')})
        fast.sleep = lambda seconds: None  # Lets the 0-150 us offsets reach the release.
        replaced = patch.object(context_module, 'time', fast)
        replaced.start()
        self.addCleanup(replaced.stop)

    def context(self, target):
        ctx = DriverContext(target, ENV, {}, '/tmp', '/tmp')
        ctx._deadline = time.monotonic() + 600
        ctx._dimensions = lambda head: (100, 100)
        return ctx

    def released_or_refused(self, ctx, target):
        if target.log.count('release') >= target.log.count('press'):
            return True
        try:
            ctx._guard()
        except RecipeBlocked as error:
            return 'release was not confirmed' in str(error)
        return False

    def trial(self, gesture, target, ctx):
        """One gesture, then a spin that gives a late alarm somewhere to land; returns what was raised.

        Its own frame on purpose: CPython 3.13 can handle a signal at a loop's final
        back-edge, which lies outside the exception table of a try around the loop, so
        an alarm landing in the spin escapes the try in this frame. The caller's try,
        around the call, catches what leaves it.
        """
        raised = None
        try:
            try:
                gesture(ctx)
            except RecipeFailed as error:
                raised = error
            deadline_end = time.perf_counter() + 300e-6
            while time.perf_counter() < deadline_end:
                pass
            signal.setitimer(signal.ITIMER_REAL, 0)  # A call: a flagged late alarm is handled here, in the try.
            signal.sigpending()
        except RecipeFailed as error:
            raised = error
        return raised

    def judge(self, ctx, target, raised, physical, offset, strict=False):
        problems = []
        if not self.released_or_refused(ctx, target):
            problems.append('press without release, input usable')
        elif strict and target.log.count('release') < target.log.count('press'):
            problems.append('press left down: the release was never sent (input only refused)')
        if raised is None:
            problems.append('deadline swallowed')
        if physical and target.log.count('release') > target.log.count('press'):
            problems.append('release without press (#736)')
        if signal.SIGALRM in set(signal.pthread_sigmask(signal.SIG_BLOCK, ())):
            problems.append('alarm left blocked')
            signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGALRM})
        return (offset, problems, target.log) if problems else None

    def sweep(self, gesture, physical=False, arm_on='press'):
        bad = []
        for index in range(self.TRIALS):
            offset = max(1e-6, 150e-6 * index / (self.TRIALS - 1))
            target = Input(physical, offset, arm_on)
            ctx = self.context(target)
            ctx._guard = lambda *a, **k: None
            raised = None
            try:
                raised = self.trial(gesture, target, ctx)
            except RecipeFailed as error:
                raised = error
            del ctx._guard
            signal.setitimer(signal.ITIMER_REAL, 0)
            problem = self.judge(ctx, target, raised, physical, round(offset * 1e6, 1))
            if problem:
                bad.append(problem)
        self.assertEqual(bad, [])
        self.assertClean()

    def inject(self, gesture, physical=False):
        """Deliver the deadline alarm at every point where CPython can run a signal handler between calls.

        The timed sweep above depends on where the interpreter and the host put the alarm.
        This one does not, and it requires the release itself, not just a refusal of later
        input: a button left down on a live desktop is the failure. The opcode tracer calls the installed SIGALRM handler, with the
        running frame, at each RESUME and JUMP_BACKWARD (the checkpoints that are not calls)
        executed by a surface_driver frame, one injection per run, and the raise takes
        effect at that instruction's exception-table entry, as a real handler's does. A
        back-edge outside a try range (CPython 3.13) is a point where the release is skipped.
        """
        checkpoint_ops = {'RESUME', 'JUMP_BACKWARD'}
        driver = str(ROOT / 'tools/lab/surface_driver')
        bad, total, counted = [], None, []
        for at in [None] * 5 + list(range(10000)):
            target = Input(physical, 0, None)
            ctx = self.context(target)
            ctx._guard = lambda *a, **k: None
            seen, hit = [0], []

            def local(frame, event, argument):
                if event == 'opcode' and dis.opname[frame.f_code.co_code[frame.f_lasti]] in checkpoint_ops:
                    seen[0] += 1
                    if seen[0] - 1 == at:
                        hit.append((frame.f_code.co_name, frame.f_lineno))
                        if signal.SIGALRM in signal.pthread_sigmask(signal.SIG_BLOCK, ()):
                            signal.raise_signal(signal.SIGALRM)  # Blocked: pending, delivered at the unmask.
                        else:
                            signal.getsignal(signal.SIGALRM)(signal.SIGALRM, frame)
                return local

            def trace(frame, event, argument):
                if frame.f_code.co_filename.startswith(driver):
                    frame.f_trace_opcodes = True
                    return local
                return None

            raised = None
            sys.settrace(trace)
            try:
                gesture(ctx)
            except RecipeFailed as error:
                raised = error
            finally:
                sys.settrace(None)
            del ctx._guard
            if at is None:
                # The interpreter instruments a code object for opcode events as it first runs
                # under the tracer: the first pass can see fewer events. Count until stable.
                counted.append(seen[0])
                if len(counted) >= 2 and counted[-1] == counted[-2] and total is None and counted[-1] > 0:
                    total = counted[-1]
                continue
            if total is None:
                self.fail('opcode tracing did not give a stable checkpoint count: ' + repr(counted))
            if at >= total:
                break
            problem = self.judge(ctx, target, raised, physical, hit[0] if hit else None, strict=True)
            if problem:
                bad.append(problem)
        self.assertEqual(bad, [])
        self.assertClean()

    @staticmethod
    def click(ctx):
        ctx._pointer_click(5, 5, 'left', 0, guarded=True)

    @staticmethod
    def drag(ctx):
        ctx.drag((10, 10), (90, 90), steps=3)

    @staticmethod
    def held(ctx):
        with ctx.held_keys('Shift'):
            signal.setitimer(signal.ITIMER_REAL, ctx.target.offset)

    @staticmethod
    def held_still(ctx):
        with ctx.held_keys('Shift'):
            pass

    def test_click_release_physical(self):
        self.sweep(self.click, physical=True)
        self.inject(self.click, physical=True)

    def test_click_release_guest(self):
        self.sweep(self.click)
        self.inject(self.click)

    def test_drag_release(self):
        self.sweep(self.drag, arm_on='move')
        self.inject(self.drag)
        self.inject(self.drag, physical=True)

    def test_held_key_release(self):
        self.sweep(self.held, arm_on=None)
        self.inject(self.held_still)


class PressBookkeeping(unittest.TestCase):
    def test_an_unconfirmed_press_withholds_later_input(self):
        target = Input(False, 0, arm_on=None)
        ctx = DriverContext(target, ENV, {}, '/tmp', '/tmp')
        ctx._press_open = True  # Release never confirmed (e.g. a recipe replaced a handler).
        with self.assertRaisesRegex(RecipeBlocked, 'release was not confirmed'):
            ctx._guard()
        ctx.begin(None)  # The next recipe inherits the refusal.
        self.assertTrue(ctx._input_poisoned)

    def test_refused_physical_press_is_not_released_and_leaves_input_usable(self):
        class Refusing(Input):
            def pointer(self, *args, **kwargs):
                raise RecipeBlocked('lock lost')
        target = Refusing(True, 0)
        ctx = DriverContext(target, ENV, {}, '/tmp', '/tmp')
        ctx._dimensions = lambda head: (100, 100)
        with self.assertRaisesRegex(RecipeBlocked, 'lock lost'):
            ctx._pointer_click(5, 5, 'left', 0, guarded=True)
        self.assertEqual(target.log, [])
        self.assertFalse(ctx._press_open or ctx._input_poisoned)


if __name__ == '__main__':
    unittest.main()
