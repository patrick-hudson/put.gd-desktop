"""Signal-safe cleanup sections (critical.py) and the releases built on them (#815)."""
import ctypes
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


class Sections(Handlers):
    def test_signal_in_a_section_is_delivered_after_it(self):
        done = []
        with self.assertRaisesRegex(RecipeFailed, 'deadline'):
            with critical.critical():
                signal.raise_signal(signal.SIGTERM)
                done.append('cleanup')
        self.assertEqual(done, ['cleanup'])
        self.assertClean()

    def test_body_failure_stays_primary_and_the_deferred_signal_is_reviewed(self):
        reviews = []
        with self.assertRaisesRegex(RuntimeError, 'release failed'):
            with critical.critical(reviews.append, label='a release'):
                signal.raise_signal(signal.SIGALRM)
                raise RuntimeError('release failed')
        self.assertEqual(len(reviews), 1)
        self.assertIn('Recipe deadline fell due during a release; the earlier failure stays the result', reviews[0])
        self.assertClean()

    def test_keyboard_interrupt_is_never_reviewed_away(self):
        signal.signal(signal.SIGINT, signal.default_int_handler)
        reviews = []
        with self.assertRaises(KeyboardInterrupt) as raised:
            with critical.critical(reviews.append, failed=True):
                signal.raise_signal(signal.SIGINT)
        self.assertEqual(reviews, [])
        self.assertClean()
        del raised

    def test_several_deferred_signals_all_run_their_handlers(self):
        calls = []
        def record(signum, _frame):
            calls.append(signum)
            raise RecipeFailed('runner interrupted by ' + signal.Signals(signum).name)
        for sig in (signal.SIGALRM, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, record)
        reviews = []
        with self.assertRaisesRegex(RecipeFailed, 'SIGTERM'):
            with critical.critical(reviews.append):
                for sig in (signal.SIGALRM, signal.SIGHUP, signal.SIGTERM):
                    signal.raise_signal(sig)
        self.assertEqual(calls, [signal.SIGTERM, signal.SIGHUP, signal.SIGALRM])
        self.assertEqual(len(reviews), 2)
        self.assertClean()

    def test_nested_sections_deliver_once_at_the_outermost_exit(self):
        order = []
        with self.assertRaisesRegex(RecipeFailed, 'deadline'):
            with critical.critical():
                with critical.critical():
                    signal.raise_signal(signal.SIGALRM)
                order.append('inner closed')
        self.assertEqual(order, ['inner closed'])
        self.assertClean()

    def test_unarmed_section_raises_a_due_signal_before_any_work(self):
        # Unwrapped handler: what was already due fails before the step starts.
        real = critical._signal.pthread_sigmask
        def due(how, mask):
            previous = real(how, mask)
            if how == signal.SIG_BLOCK and signal.SIGTERM in set(mask):
                raise RecipeFailed('runner interrupted by SIGTERM')
            return previous
        with patch.object(critical._signal, 'pthread_sigmask', side_effect=due):
            with self.assertRaisesRegex(RecipeFailed, 'SIGTERM'):
                with critical.critical():
                    self.fail('no work after a due signal')
        self.assertClean()


class Armed(Handlers):
    def test_wrapper_defers_only_in_registered_frames_and_sections(self):
        @critical.entry
        def release():
            pass
        with critical.armed():
            wrapper = signal.getsignal(signal.SIGTERM)
            self.assertIs(wrapper.original, deadline)
            with self.assertRaisesRegex(RecipeFailed, 'deadline'):
                wrapper(signal.SIGTERM, sys._getframe())
            self.assertIsNone(wrapper(signal.SIGTERM, SimpleNamespace(f_code=release.__code__)))
            self.assertEqual(len(critical._state.pending), 1)
            with self.assertRaisesRegex(RecipeFailed, 'deadline'):
                with critical.critical():
                    pass  # Delivers the signal deferred at the registered entry.
        self.assertIs(signal.getsignal(signal.SIGTERM), deadline)
        self.assertClean()

    def test_replaced_handler_is_reported_and_the_original_put_back(self):
        guard = critical.armed()
        with guard:
            signal.signal(signal.SIGALRM, signal.SIG_IGN)
            self.assertEqual(guard.replaced(), [signal.SIGALRM])
        self.assertIs(signal.getsignal(signal.SIGALRM), deadline)
        self.assertClean()

    def test_signal_due_as_the_scope_closes_is_not_stranded(self):
        # A registered frame whose section has ended forwards, not defers.
        @critical.entry
        def cleanup():
            with critical.critical():
                pass
            signal.raise_signal(signal.SIGTERM)  # After the section, same frame.
        with critical.armed():
            with self.assertRaisesRegex(RecipeFailed, 'deadline'):
                cleanup()
            self.assertEqual(critical._state.pending, [])
        self.assertClean()


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

    def sweep(self, gesture, physical=False, arm_on='press'):
        bad = []
        for index in range(self.TRIALS):
            offset = max(1e-6, 150e-6 * index / (self.TRIALS - 1))
            target = Input(physical, offset, arm_on)
            ctx = self.context(target)
            ctx._guard = lambda *a, **k: None
            raised = None
            try:
                try:
                    gesture(ctx)
                except RecipeFailed as error:
                    raised = error
                deadline_end = time.perf_counter() + 300e-6
                while time.perf_counter() < deadline_end:
                    pass
            except RecipeFailed as error:
                raised = error
            del ctx._guard
            signal.setitimer(signal.ITIMER_REAL, 0)
            problems = []
            if not self.released_or_refused(ctx, target):
                problems.append('press without release, input usable')
            if raised is None:
                problems.append('deadline swallowed')
            if physical and target.log.count('release') > target.log.count('press'):
                problems.append('release without press (#736)')
            if signal.SIGALRM in set(signal.pthread_sigmask(signal.SIG_BLOCK, ())):
                problems.append('alarm left blocked')
                signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGALRM})
            if problems:
                bad.append((round(offset * 1e6, 1), problems, target.log))
        self.assertEqual(bad, [])
        self.assertClean()

    def test_click_release_physical(self):
        self.sweep(lambda ctx: ctx._pointer_click(5, 5, 'left', 0, guarded=True), physical=True)

    def test_click_release_guest(self):
        self.sweep(lambda ctx: ctx._pointer_click(5, 5, 'left', 0, guarded=True))

    def test_drag_release(self):
        def drag(ctx):
            ctx.drag((10, 10), (90, 90), steps=1)
        self.sweep(drag, arm_on='move')

    def test_held_key_release(self):
        def held(ctx):
            with ctx.held_keys('Shift'):
                signal.setitimer(signal.ITIMER_REAL, ctx.target.offset)
        self.sweep(held, arm_on=None)


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
