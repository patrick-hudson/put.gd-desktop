"""Critical cleanup sections: deadline and termination signals wait for them.

run.py fails a recipe with a one-shot SIGALRM and stops the run on SIGTERM or
SIGHUP; SIGINT raises KeyboardInterrupt. Their handlers raise, so a signal
landing in a release, a browser restore or a fixture finalizer would skip it.

Blocking the signals as a cleanup's first statement is too late. CPython runs
a pending handler at the next eval-breaker check, and a function's start is
one: the handler runs in the new frame before its first line (#680, #815).
Two scopes close that window:

* ``armed()`` wraps the current Python handlers for the four signals for a
  whole operation whose cleanup must run: a press-release gesture, a browser
  transaction, fixture setup, the runner's recipe loop. Outside a section the
  wrapper calls the original handler at once, so behaviour is unchanged.
* ``critical()`` blocks the four signals for one bounded cleanup step. While
  armed, a handler that falls due inside a section, in this module, or at the
  start of a function registered with ``entry`` defers its signal instead.

A registered function must open ``critical()`` as its first statement. When
the outermost section or an armed scope ends, each deferred signal's original
handler runs, in the order SIGINT, SIGTERM, SIGHUP, SIGALRM. Its exception is
raised, unless the step had already failed: then that failure stays the
error and the deferred deadline or termination becomes a review (side effects
such as run.py's ``interrupted`` still happen). KeyboardInterrupt and
SystemExit always propagate. A blocked signal at default disposition is
raised again after the section. Sections must be short: callers bound their
transport time. The controller is single-threaded; masks are per thread.
"""
import signal
import sys

# C entry points. signal.py wraps pthread_sigmask, signal and getsignal in
# Python functions (enum conversion, a generator): a handler due there would
# run in an unregistered frame.
import _signal

# Delivery order: a stop request first, the recipe deadline last.
SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGALRM)
_NAMES = {signal.SIGALRM: 'Recipe deadline', signal.SIGTERM: 'SIGTERM', signal.SIGHUP: 'SIGHUP',
          signal.SIGINT: 'SIGINT'}


class _State:
    depth = 0       # Open critical sections.
    pending = []    # (signum, original handler) deferred by a wrapper.
    closed = []     # Frames on the stack when the last outermost scope closed.


_state = _State()
_ENTRY = set()  # Code objects in which a wrapped handler defers.


def entry(function):
    """Register ``function``: a signal due as it starts is deferred while armed."""
    _ENTRY.add(function.__code__)
    return function


class _Deferring:
    """A wrapped handler; ``original`` is the handler it stands in for."""
    __slots__ = ('original',)

    def __init__(self, original):
        self.original = original

    def __call__(self, signum, frame):
        if _state.depth or (frame is not None and frame.f_code in _ENTRY
                            and not any(frame is done for done in _state.closed)):
            _state.pending.append((signum, self.original))
            return None
        return self.original(signum, frame)


def _stack():
    frames, frame = [], sys._getframe(1)
    while frame is not None:
        frames.append(frame)
        frame = frame.f_back
    return frames


def _original(handler):
    return handler.original if isinstance(handler, _Deferring) else handler


def _block():
    """Block SIGNALS and return the previous mask.

    A handler already due runs inside pthread_sigmask after the mask changed.
    In a registered frame a wrapper defers it; an unwrapped one raises, and
    then the previous mask is restored before the exception leaves.
    """
    previous = _signal.pthread_sigmask(signal.SIG_BLOCK, ())
    try:
        _signal.pthread_sigmask(signal.SIG_BLOCK, SIGNALS)
    except BaseException:
        _signal.pthread_sigmask(signal.SIG_SETMASK, previous)
        raise
    return previous


def pending():
    """Whether a deferred or blocked deadline/termination is waiting."""
    return bool(_state.pending) or bool(set(_signal.sigpending()) & set(SIGNALS))


def _record(review, text, error):
    if review is not None:
        try:
            review(text)
            return
        except Exception:
            pass
    if error is not None:
        error.add_note(text)


def _settle(raised, failing, review, label, primary, chosen=None):
    """Pick the exception to raise; record the rest."""
    for signum, error in raised:
        what = _NAMES.get(signum, 'A deferred signal')
        detail = ' (' + type(error).__name__ + ': ' + str(error)[:160] + ')'
        stops = not isinstance(error, Exception)
        if not failing and chosen is None:
            chosen = error
        elif not failing and not stops:
            _record(review, what + ' also fell due during ' + label + detail, chosen)
        elif stops and (chosen is None or isinstance(chosen, Exception)):
            if chosen is not None:
                _record(review, 'Deferred failure superseded by ' + type(error).__name__ + ' during ' + label +
                        ' (' + type(chosen).__name__ + ': ' + str(chosen)[:160] + ')', error)
            chosen = error  # Stop requests are never reviewed away.
        else:
            _record(review, what + ' fell due during ' + label + '; the earlier failure stays the result' + detail,
                    primary if primary is not None else chosen)
    return chosen


def _deliver(previous, failing, review, label, primary, chosen, defaults):
    """One round, masked on entry; returns with ``previous`` restored."""
    raised = []
    try:
        # Signals blocked by the section: take them here, not at the unmask.
        while True:
            caught = _signal.sigtimedwait(SIGNALS, 0)
            if caught is None:
                break
            _state.pending.append((caught.si_signo, _original(_signal.getsignal(caught.si_signo))))
        while _state.pending:
            batch = sorted(_state.pending, key=_order)
            _state.pending = []
            for signum, handler in batch:
                if handler in (signal.SIG_IGN, None):
                    continue
                if handler == signal.SIG_DFL:
                    defaults.append(signum)
                    continue
                try:
                    handler(signum, None)
                except BaseException as error:
                    raised.append((signum, error))
        chosen = _settle(raised, failing, review, label, primary, chosen)
    finally:
        try:
            _signal.pthread_sigmask(signal.SIG_SETMASK, previous)
        except BaseException as late:
            # Arrived after the sweep above, at the unmask: delivered all the same.
            chosen = _settle([(None, late)], failing or chosen is not None, review, label, primary, chosen)
    return chosen


def _finish(guard, previous, failing, review, label, primary):
    """Close the outermost scope. Called masked, from a frame in this module."""
    if guard is not None:
        guard._uninstall(force=True)
        if guard.mask is not None:  # The four signals go back to their state at arming.
            previous = (set(previous) - set(SIGNALS)) | (set(guard.mask) & set(SIGNALS))
    chosen, defaults = None, []
    while True:
        chosen = _deliver(previous, failing, review, label, primary, chosen, defaults)
        # From here on these frames' cleanup is over: a signal that falls due
        # in them is raised, not deferred to a section that will not follow.
        _state.closed = _stack()
        if not _state.pending or _state.depth:
            break
        # An outer armed scope's wrapper deferred a signal at the unmask, in
        # this frame: deliver it now, not when that scope ends.
        previous = _block()
        failing = failing or chosen is not None
    for signum in defaults:
        _signal.raise_signal(signum)  # Default action, after the cleanup.
    if chosen is not None:
        raise chosen
    return False


def _order(item):
    return SIGNALS.index(item[0])


def _failing(kind, failed):
    if kind is not None:
        return True
    if failed is not None:
        return failed
    return sys.exc_info()[1] is not None  # Cleanup in an except or finally block.


class armed:
    """Wrap the four signal handlers for an operation whose cleanup must run.

    ``replaced()`` lists signals whose handler was replaced inside the scope;
    the exit restores the handlers this scope wrapped either way.
    """

    def __init__(self, review=None, label='an input release'):
        self.review, self.label = review, label
        self.installed, self.wrapped, self.mask = {}, {}, None

    def __enter__(self):
        while True:
            previous = self.mask = _block()
            for sig in SIGNALS:
                handler = _signal.getsignal(sig)
                if callable(handler) and not isinstance(handler, _Deferring):
                    wrapper = _Deferring(handler)
                    _signal.signal(sig, wrapper)
                    self.installed[sig] = wrapper
            self.wrapped = {}
            for sig in SIGNALS:
                handler = _signal.getsignal(sig)
                if isinstance(handler, _Deferring):
                    self.wrapped[sig] = handler
            # A signal blocked just now arrives here, wrapped, in this frame: deferred.
            _signal.pthread_sigmask(signal.SIG_SETMASK, previous)
            if not _state.pending or _state.depth:
                return self
            # Due before the operation started: deliver it now, unwrapped.
            _finish(self, _block(), False, self.review, self.label, None)
            _state.closed = []

    def replaced(self):
        return [sig for sig, wrapper in self.wrapped.items() if _signal.getsignal(sig) is not wrapper]

    def _uninstall(self, force):
        for sig, wrapper in self.installed.items():
            if force or _signal.getsignal(sig) is wrapper:
                _signal.signal(sig, wrapper.original)
        self.installed = {}

    def abandon(self):
        """Drop wrappers still installed, leaving any later handler in place."""
        previous = _block()
        try:
            self._uninstall(force=False)
        finally:
            _signal.pthread_sigmask(signal.SIG_SETMASK, previous)

    def __exit__(self, kind, value, traceback):
        previous = _block()
        if _state.depth:  # Inside a section: it delivers.
            self._uninstall(force=True)
            _signal.pthread_sigmask(signal.SIG_SETMASK, previous)
            return False
        return _finish(self, previous, _failing(kind, None), self.review, self.label, value)


class critical:
    """Defer SIGALRM, SIGTERM, SIGHUP and SIGINT for one bounded cleanup step.

    ``failed``: a primary error is already propagating outside the section
    (default: the body raised, or this runs in an except/finally block).
    ``release``: an ``armed`` scope to close with the outermost section.
    """

    def __init__(self, review=None, failed=None, label='cleanup', release=None):
        self.review, self.failed, self.label, self.release = review, failed, label, release
        self.previous = None

    def __enter__(self):
        if not _state.depth:
            self.previous = _block()
            _state.closed = []
        _state.depth += 1
        return self

    def pending(self):
        return pending()

    def __exit__(self, kind, value, traceback):
        _state.depth -= 1
        if _state.depth:
            if self.release is not None:
                self.release._uninstall(force=True)  # Still masked by the outer section.
            return False
        return _finish(self.release, self.previous, _failing(kind, self.failed), self.review, self.label, value)


def _codes(code):
    yield code
    for constant in code.co_consts:
        if hasattr(constant, 'co_consts'):
            yield from _codes(constant)


# Frames of this module's scope machinery defer: that code runs only while
# entering or leaving a scope, and each path ends in _finish or in a section
# that delivers. Read-only queries callable from anywhere are left out.
for _value in list(globals().values()):
    for _function in ([_value] if callable(_value) and hasattr(_value, '__code__') else
                      vars(_value).values() if isinstance(_value, type) else ()):
        if (getattr(_function, '__module__', None) == __name__ and hasattr(_function, '__code__')
                and _function.__name__ not in ('entry', 'pending', 'replaced', '_codes')):
            _ENTRY.update(_codes(_function.__code__))
del _value, _function
