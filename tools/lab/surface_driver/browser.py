"""Real default-Firefox verification, with target-side URL/token confinement.

Only stdlib is required on the target. Marionette is Firefox's shipped remote
protocol (firefox-source-docs.mozilla.org/remote/marionette/Protocol.html).
The temporary handler launches Firefox itself; it never records URL arguments.
Profile fragments may set ``browser.command`` (argv, e.g. snap run firefox),
``browser.profile_root`` (snap's ~/snap/firefox/common), real
``browser.config_home``/``browser.data_home`` when they differ from ~/.config
and ~/.local/share, and ``browser.timeouts`` (seconds, see _TIMEOUTS) for slow
software-rendered guests. The portal's real defaults and Context.shell's
isolated XDG defaults are both guarded, covering portal and launcher fallback
dispatch.

The handler reruns the browser command with the private ``--profile``; Firefox
remoting, keyed by profile, forwards the URL to the running Marionette
instance as a new tab (#693 guest trials: Mint deb 146 and Ubuntu snap 154,
also with a personal default-profile Firefox already running). Snap replaces
MOZ_APP_REMOTINGNAME with its own, so the profile is what routes the URL. A
dispatch that starts a Firefox outside that profile fails closed by name;
that process is stopped at restore, since its window may show the link.

Restoration does not depend on the controller surviving. It runs outside the
recipe deadline; the target helper ignores the SSH hangup and leaves the
session's process group, so a step in progress completes; and every setup
first restores any transaction an interrupted run left behind. Each MIME file
is restored on its own, real-profile files first; a private XDG root that has
since gone (reboot, tmpfiles) counts as already cleaned. A file that cannot be
restored keeps its backup for the next setup without blocking the others.
"""
from __future__ import annotations

import base64
from dataclasses import asdict
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time

_ACTIVE = {}

# Seconds. protocol: one Marionette command (Firefox-side script and page-load
# timeouts; the socket allows a little more so Firefox's own named error comes
# first). ready: private Firefox startup. page: the dispatched page appearing.
# helper: one helper step, still clipped by the recipe deadline. cleanup:
# restore or recovery, which runs even after the recipe deadline.
_TIMEOUTS = {'protocol': 20, 'ready': 60, 'page': 45, 'helper': 120, 'cleanup': 180}


def _timeouts(options):
    values = dict(_TIMEOUTS)
    for key, value in (options.get('timeouts') or {}).items():
        if key not in values or type(value) not in (int, float) or not 0 < value <= 600:
            raise ValueError('browser.timeouts.%s must be a known timeout of 0 to 600 seconds' % key)
        values[key] = value
    return values


def _remote(ctx, action, values, timeout=_TIMEOUTS['helper'], cleanup=False):
    # Source and arguments contain no share links. No raw browser responses leave
    # the target, even on a protocol error. Do not interpolate observed URLs here.
    payload = base64.b64encode(json.dumps(values).encode()).decode()
    script = ('python3 - ' + shlex.quote(action) + ' ' + shlex.quote(payload) + " <<'PUTGD_BROWSER_PY'\n"
              + Path(__file__).read_text() + '\nPUTGD_BROWSER_PY')
    # cleanup: Context.shell's deadline-exempt restoration path.
    result = ctx.shell(script, timeout=timeout, cleanup=True) if cleanup else ctx.shell(script, timeout=timeout)
    if result.get('returncode', 1):
        raise RuntimeError('Target browser helper failed; raw output withheld')
    try:
        value = json.loads(result['stdout'])
    except (KeyError, TypeError, ValueError):
        raise RuntimeError('Target browser helper returned an invalid receipt') from None
    if value.get('error'):
        raise RuntimeError('Target browser helper: ' + value['error'])
    return value


try:
    from .critical import armed, critical, entry
except ImportError:  # Standalone on the target: only the helper actions run there.
    def entry(function):
        return function


class default_browser:
    """Temporarily install an isolated real Firefox as the http(s) handler.

    Restores every touched mimeapps file byte for byte, verifies SHA-256, and
    leaves unrelated desktop entries (including the guarded recorder) alone.
    Restoration errors are fatal and leave the private backup for recovery.

    No recipe deadline or termination signal can skip the restore. The
    transaction is armed (critical.py) from entry to exit: a SIGALRM,
    SIGTERM, SIGHUP or SIGINT that falls due as ``__exit__`` starts, or during
    the restore, is deferred and delivered after it. It fails the recipe,
    unless the body had already failed: that failure stays the result and the
    deferred signal is reviewed. Setup recovery is deferred the same way.
    Recipes must not replace those handlers inside the ``with``: that is
    reviewed, and the exit puts back the handlers from entry. A transaction
    whose ``__exit__`` never ran is restored at fixture cleanup
    (``restore_abandoned``).
    """

    def __init__(self, ctx, browser='firefox'):
        if browser != 'firefox':
            raise ValueError('Only Firefox content verification is implemented')
        self.ctx = ctx

    def __enter__(self):
        ctx = self.ctx
        if id(ctx) in _ACTIVE:
            raise RuntimeError('Nested default_browser transactions are unsupported')
        options = dict(ctx.profile.get('browser', {}))
        options.pop('live_downloads', None)
        if (getattr(ctx, '_live_admitted', False) and ctx.placement == 'guest'
                and ctx.destination == 'proton'):
            options['live_downloads'] = True
        self.timeouts = _timeouts(options)
        self.guard = armed(ctx.review, label='the Firefox restore')
        self.guard.__enter__()
        try:
            self.state = _remote(ctx, 'prepare', options, timeout=self.timeouts['helper'] + self.timeouts['ready'])
        except BaseException:
            self._recover(options)
            raise
        try:
            _ACTIVE[id(ctx)] = {'state': self.state, 'timeouts': self.timeouts, 'manager': self}
            if self.state.get('recovered'):
                ctx.note('Restored %d unfinished Firefox handler transaction(s) before setup'
                         % self.state['recovered'])
            _note_dropped(ctx, self.state)
        except BaseException:
            self._abort()
            raise
        return self

    @entry
    def _recover(self, options):
        # The target step may have outlived its transport (deadline, dropped
        # session): restore whatever it left before reporting the failure.
        ctx, timeouts = self.ctx, self.timeouts
        with critical(ctx.review, failed=True, label='the Firefox setup recovery', release=self.guard):
            try:
                _note_dropped(ctx, _remote(ctx, 'recover', {**options, 'wait': timeouts['cleanup'] - 30},
                                           timeout=timeouts['cleanup'], cleanup=True))
            except Exception as error:
                ctx.review('Interrupted Firefox setup not yet recovered (' + str(error)[:200] +
                           '); the next default_browser entry on this account restores it')

    @entry
    def _abort(self):
        self.__exit__(*sys.exc_info())

    @entry
    def __exit__(self, kind, value, traceback):
        # Registered: a signal due as this starts is deferred, not raised here.
        ctx = self.ctx
        with critical(ctx.review, failed=kind is not None, label='the Firefox restore',
                      release=self.guard) as section:
            replaced = self.guard.replaced()
            if replaced:
                ctx.review('Recipe replaced the ' + '/'.join(signal.Signals(s).name for s in replaced) +
                           ' handler inside default_browser; deferral was off, and the handler from entry is back')
            try:
                problem = _restore_transaction(ctx, self.state, self.timeouts,
                                               failing=kind is not None or section.pending())
            finally:
                _ACTIVE.pop(id(ctx), None)
            if problem is not None and kind is None:
                raise problem  # The deferred signal, if any, is reviewed.
        return False  # The recipe's own failure stays the result.


@entry
def restore_abandoned(ctx):
    """Restore a transaction whose ``__exit__`` never ran; returns cleanup errors.

    Only a handler that bypassed the deferral (a recipe replaced it) or a
    manual ``__enter__`` can leave one open. run.py calls this in its fixture
    cleanup section, after the recipe; handlers stay as run.py set them.
    """
    active = _ACTIVE.get(id(ctx))
    if not active:
        return []
    with critical(ctx.review, failed=True, label='the abandoned Firefox restore'):
        ctx.review('default_browser transaction was still open at fixture cleanup; restoring it there')
        try:
            active['manager'].guard.abandon()
            problem = _restore_transaction(ctx, active['state'], active['timeouts'], failing=True)
        finally:
            _ACTIVE.pop(id(ctx), None)
    return [] if problem is None else ['Firefox handler restore: ' + type(problem).__name__]


def _note_dropped(ctx, answer):
    if answer.get('dropped'):
        ctx.note('Skipped %d MIME backup(s) whose private XDG root no longer exists (already cleaned)'
                 % answer['dropped'])


def _restore_transaction(ctx, state, timeouts, failing):
    """Restore, retrying once; returns the error the run should fail with, if any.

    Reviews record every restore problem. A retry is idempotent: once the
    target has restored, it answers restored for the vanished transaction.
    """
    attempt = lambda: _remote(ctx, 'restore', {'directory': state['directory']},
                              timeout=timeouts['cleanup'], cleanup=True)
    try:
        restored = attempt()
    except Exception as error:
        try:
            restored = attempt()
        except Exception as again:
            ctx.review('Firefox handler restore incomplete (' + str(again)[:200] +
                       '); the next default_browser entry on this account restores it')
            return error
    try:
        if restored.get('restored') is not True:
            ctx.review('Firefox handler restore unconfirmed; the next default_browser entry on this account restores it')
            ctx.expect(False, 'browser MIME bytes and hashes restored')
        ctx.note('Firefox handler restored; MIME SHA-256 checks passed')
        _note_dropped(ctx, restored)
        if restored.get('foreign'):
            ctx.note('Stopped %d Firefox process(es) started outside the private profile' % restored['foreign'])
            if not failing:
                ctx.expect(False, 'no Firefox started outside the private profile during the transaction')
    except Exception as error:
        return error
    return None


def open_and_verify(ctx, trigger, expect):
    """Exercise an entry point and inspect the page in the real default browser.

    ``expect`` is {kind: s3|proton|local, name: filename, preview: bool}.
    Optional ``url_host`` and ``text`` tighten local fixture checks. Receipts
    retain only the hostname, a bounded title, booleans and full-desktop frames.
    A new blank tab before each trigger prevents a stale page proving a click.
    """
    if id(ctx) not in _ACTIVE:
        raise RuntimeError('open_and_verify requires default_browser')
    if (expect.get('kind') not in ('s3', 'proton', 'local') or
            not isinstance(expect.get('name'), str) or not expect['name'] or
            type(expect.get('preview')) is not bool):
        raise ValueError('Expected kind, nonempty filename and preview boolean')
    from .api import RecipeFailed  # Controller only; the target runs this file standalone.
    state, timeouts = _ACTIVE[id(ctx)]['state'], _ACTIVE[id(ctx)]['timeouts']
    frames = []
    try:
        ready = _remote(ctx, 'blank', state, timeout=timeouts['helper'])
        ctx.expect(ready.get('safe'), 'private blank Firefox kiosk owns the foreground before input')
        trigger()
        pending = []
        def observe():
            result = _remote(ctx, 'observe', {**state, 'expect': expect}, timeout=timeouts['helper'])
            if result.get('pending'):
                pending[:] = [result['pending']]  # Static, named helper reason.
                return None
            return result
        try:
            result = ctx.wait(observe, 'default Firefox loaded the expected page content',
                              timeout=max(timeouts['page'], 120) if expect['kind'] == 'proton' else timeouts['page'])
        except RecipeFailed as error:
            if pending:
                raise RecipeFailed(str(error) + '; last Firefox answer: ' + pending[0]) from None
            raise
        ctx.expect(not result.get('foreign'),
                   'external dispatch reached the private Marionette Firefox, '
                   'not a Firefox started outside its profile')
        ctx.expect(result.get('matched'), 'real Firefox URL, title and page content match')
        frames.append(asdict(ctx.frame('browser-content', window=None, desktop_safe=KIOSK_SAFE)))
        receipt = {key: result[key] for key in ('url_host', 'title', 'matched')}
        receipt['frames'] = frames
        ctx.note(json.dumps({key: receipt[key] for key in ('url_host', 'title', 'matched')}))
        return receipt
    except Exception:
        # KeyboardInterrupt and SystemExit skip the failure frame; restoration
        # still runs in default_browser.
        _failure_frame(ctx, state, timeouts, frames)
        raise


# Live-evidence declaration for these full-desktop frames: the private
# Marionette kiosk has no address bar and was proven to own the foreground
# (blanked first for failures). Notification areas, other browser chrome,
# leaking titles and owned secret fields stay masked; OCR still audits.
KIOSK_SAFE = 'private Firefox kiosk without address bar owns the verified foreground'


def live_download_digest(ctx, kind, saved_path):
    """Download original recipient bytes and compare with the saved PNG in guest RAM."""
    active = _ACTIVE[id(ctx)]
    return _remote(ctx, 'download-digest', dict(active['state'], kind=kind,
                   saved_path=saved_path, app_root=ctx.target.root), timeout=150)


def live_link_unavailable(ctx, kind):
    """Reload the existing URL without returning it or trusting a cached preview."""
    active = _ACTIVE[id(ctx)]
    result = _remote(ctx, 'unavailable', dict(active['state'], kind=kind), timeout=160)
    ctx.note('recipient_unavailable_observation='+json.dumps(result, sort_keys=True))
    ctx.focus('outcome-browser', raise_window=ctx.profile.get('session') == 'x11')
    label = 'browser-cloudfront-status-'+str(result.get('status', 0)) if kind == 's3' else (
        'browser-link-unavailable' if result.get('unavailable') else 'browser-link-unavailable-failed')
    ctx.frame(label,
              window=None, desktop_safe=KIOSK_SAFE)
    return result


def _failure_frame(ctx, state, timeouts, frames):
    """Save a full-desktop failure frame only over a proven blank kiosk.

    Never raises: the recipe's own failure is the result, and the transaction is
    restored by default_browser either way.
    """
    # Kiosk has no address bar. Blank the page before retaining failure
    # evidence, since error pages may echo a token-bearing request URL.
    try:
        safe = _remote(ctx, 'blank', state, timeout=timeouts['helper']).get('safe')
    except Exception as error:
        ctx.review('Failure frame withheld: the private kiosk could not be blanked (' + str(error)[:200] + ')')
        return
    if not safe:
        ctx.review('Failure frame withheld: private blank kiosk did not own the foreground '
                   'or another Firefox was started; core needs a redacted full-desktop failure frame')
        return
    try:
        frames.append(asdict(ctx.frame('browser-failure-safe-desktop', window=None, desktop_safe=KIOSK_SAFE)))
    except Exception as error:
        ctx.review('Failure frame unavailable (' + str(error)[:200] + ')')


class _TargetError(RuntimeError):
    """Static, secret-free target error suitable for a receipt."""


class _Timeout(_TargetError):
    """Firefox timed out, or did not answer at all (then ``broken`` is set)."""


# WebDriver error codes are fixed lowercase names; anything else is withheld.
_ERROR_CODE = re.compile(r'[a-z][a-z ]{0,39}')


class _Marionette:
    grace = 10  # Seconds the socket waits beyond Firefox's own timeouts.

    def __init__(self, port, timeout=_TIMEOUTS['protocol']):
        self.timeout = timeout
        self.broken = False
        self.sequence = 0
        # Firefox's own timeouts expire first and answer with a named error;
        # the socket timeout only catches a Firefox that stopped answering.
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=timeout + self.grace)
        self.stream = self.sock.makefile('rb')
        try:
            hello = self._receive('connect')
            if hello.get('marionetteProtocol') != 3:
                raise _TargetError('Unsupported Firefox protocol')
            self.call('WebDriver:NewSession', {'capabilities': {}})
            milliseconds = int(timeout * 1000)
            self.call('WebDriver:SetTimeouts', {'script': milliseconds, 'pageLoad': milliseconds})
        except BaseException:
            self.stream.close()
            self.sock.close()
            raise

    def _receive(self, method):
        try:
            return self.read()
        except socket.timeout:
            # A timed-out socket file refuses further reads ("cannot read from
            # timed out object"); never read it again, not even to close.
            self.broken = True
            raise _Timeout('Firefox did not answer %s within %g s' % (method, self.timeout + self.grace)) from None
        except OSError:
            self.broken = True
            raise _TargetError('Firefox connection failed during ' + method) from None

    def read(self):
        header = bytearray()
        while True:
            char = self.stream.read(1)
            if char == b':':
                break
            if not char or not char.isdigit() or len(header) >= 9:
                raise _TargetError('Invalid Firefox frame')
            header.extend(char)
        size = int(header)
        if size > 8 * 1024 * 1024:
            raise _TargetError('Oversized Firefox frame')
        data = self.stream.read(size)
        if len(data) != size:
            raise _TargetError('Incomplete Firefox frame')
        return json.loads(data)

    def call(self, method, params=None):
        if self.broken:
            raise _TargetError('Firefox connection unusable after ' + method)
        self.sequence += 1
        data = json.dumps([0, self.sequence, method, params or {}]).encode()
        try:
            self.sock.sendall(str(len(data)).encode() + b':' + data)
        except OSError:
            self.broken = True
            raise _TargetError('Firefox connection failed during ' + method) from None
        value = self._receive(method)
        if value[:2] != [1, self.sequence] or value[2] is not None:
            # Firefox errors can contain URLs. Never include the response;
            # only a WebDriver error code such as "timeout" is named.
            error = value[2] if isinstance(value[2], dict) else {}
            code = error.get('error') if isinstance(error.get('error'), str) else ''
            if _ERROR_CODE.fullmatch(code):
                raise _Timeout('Firefox %s failed: %s' % (method, code)) if 'timeout' in code else \
                    _TargetError('Firefox %s failed: %s' % (method, code))
            raise _TargetError('Firefox protocol command failed')
        result = value[3]
        return result["value"] if isinstance(result, dict) and set(result) == {"value"} else result

    def script(self, source, args=(), asynchronous=False):
        value = self.call('WebDriver:ExecuteAsyncScript' if asynchronous else 'WebDriver:ExecuteScript', {
            'script': source, 'args': list(args), 'newSandbox': True,
            'sandbox': 'default', 'line': 0, 'filename': 'surface-browser'})
        return value.get('value', value) if isinstance(value, dict) else value

    def close(self):
        try:
            if not self.broken:
                self.call('WebDriver:DeleteSession')
        except _TargetError:
            pass  # Firefox ends the session with the connection anyway.
        finally:
            self.stream.close()
            self.sock.close()


# The raw URL and DOM stay inside Firefox. Return an allowlisted, sanitized
# observation, never document text, href, location or a protocol error message.
_OBSERVE = r'''
const e = arguments[0], u = new URL(location.href), body = document.body;
const text = body ? body.innerText : '', title = document.title || '';
let basename = '';
try { basename=decodeURIComponent(u.pathname.split('/').pop()); } catch (_) {}
const named = title.includes(e.name) || text.includes(e.name) ||
              basename===e.name;
const visible = el => {const r=el.getBoundingClientRect();
  return r.width>0 && r.height>0 && getComputedStyle(el).visibility!=='hidden';};
const painted = el => {
  try { return el.width>0 && el.height>0 &&
    el.getContext('2d').getImageData(0,0,el.width,el.height).data.some((v,i)=>i%4===3 && v>0); }
  catch (_) { return false; }
};
const media = [...document.querySelectorAll('img,video,canvas')].some(el =>
  visible(el) && (el.tagName==='IMG' ? el.complete && el.naturalWidth>0 :
  el.tagName==='VIDEO' ? el.readyState>=2 : painted(el)));
// Readiness is separate from the later mandatory original-byte download.
const status = performance.getEntriesByType('navigation')[0]?.responseStatus || 0;
const error = /^(404|403|500|AccessDenied|NoSuchKey|Error response)/i.test(text.trim()) ||
              /Problem loading page|Server Not Found|Error response|Access Denied/i.test(title);
const protonMedia=[...document.querySelectorAll('img,video,canvas')].some(el =>
 visible(el) && (el.tagName==='IMG' ? el.complete && el.naturalWidth>0 &&
 (el.currentSrc||el.src).startsWith('blob:') : el.tagName==='VIDEO' ? el.readyState>=2 : painted(el)));
const content = e.kind==='proton' ? text.includes(e.name) || protonMedia :
                (media || text.trim().length>0);
const hostOK = e.url_host ? u.hostname===e.url_host :
               e.kind==='proton' ? /(^|\.)proton\.(me|ch)$/.test(u.hostname) : true;
const matched = ['http:','https:'].includes(u.protocol) && hostOK && (named || (e.kind==='proton' && protonMedia)) &&
  title.length>0 && document.visibilityState==='visible' && document.hasFocus() &&
  innerWidth>=screen.width && innerHeight>=screen.height &&
  document.readyState==='complete' && status<400 && !error &&
  content && (!e.preview || e.kind==='proton' || media || text.trim().length>0) &&
  (!e.text || text.includes(e.text));
// Keep only a title we can prove contains no URL path, fragment, or arbitrary
// page-supplied token: the filename, when it is present in the actual title.
return {matched, url_host: matched ? u.hostname : '',
  title: matched ? (title.includes(e.name) ? e.name : '[page title verified; redacted]') : ''};
'''

# Two animation frames after the match, so the selected tab is painted before
# the runner's full-desktop frame (a just-switched tab paints blank first).
_PAINTED = r'''
const done = arguments[arguments.length - 1];
requestAnimationFrame(() => requestAnimationFrame(() =>
  done(document.visibilityState==='visible' && document.hasFocus())));
'''


def _mime_paths(config, data):
    roots = (config, data / 'applications')
    paths = {root / 'mimeapps.list' for root in roots}
    for root in roots:
        paths.update(root.glob('*-mimeapps.list'))
    # xdg-mime may create a desktop-specific file that didn't exist before.
    for desktop in os.environ.get('XDG_CURRENT_DESKTOP', '').lower().split(':'):
        if re.fullmatch(r'[a-z0-9_-]+', desktop):
            paths.update(root / (desktop + '-mimeapps.list') for root in roots)
    return sorted(paths)


def _snapshot(paths):
    result = []
    for path in paths:
        if path.is_symlink() or (path.exists() and not path.is_file()):
            raise _TargetError('MIME file must be a regular file')
        data = path.read_bytes() if path.exists() else None
        result.append({'path': str(path), 'bytes': base64.b64encode(data).decode() if data is not None else None,
                       'sha256': hashlib.sha256(data).hexdigest() if data is not None else None,
                       'mode': stat.S_IMODE(path.stat().st_mode) if data is not None else None})
    return result


def _backups(profiles):
    """Byte backups of every guarded MIME file, the real profile's first.

    Private (app XDG) entries name their root: when that root has gone, there
    is nothing left to restore into.
    """
    (config, data), private = profiles[0], profiles[1:]
    real = _mime_paths(config, data)
    backups = [dict(item, scope='real') for item in _snapshot(real)]
    for profile_config, profile_data in private:
        paths = [path for path in _mime_paths(profile_config, profile_data) if path not in real]
        for path, item in zip(paths, _snapshot(paths)):
            root = profile_config if path.parent == profile_config else profile_data
            backups.append(dict(item, scope='private', root=str(root)))
    return backups


def _private_scope(item):
    """(private, root) for one backup; records without a scope predate it."""
    if 'scope' in item:
        return item['scope'] == 'private', item.get('root')
    path, home = Path(item['path']), Path(pwd.getpwuid(os.getuid()).pw_dir)
    if path.is_relative_to(home) or path.is_relative_to(home.resolve()):
        return False, None
    return True, str(path.parent)


def _restore_entry(item):
    path = Path(item['path'])
    if item['bytes'] is None:
        if path.is_symlink():
            raise _TargetError('MIME path became a symlink')
        path.unlink(missing_ok=True)
        if path.exists():
            raise _TargetError('MIME removal failed')
        return
    data = base64.b64decode(item['bytes'])
    descriptor, temporary = tempfile.mkstemp(prefix='.putgd-restore-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
            os.fchmod(handle.fileno(), item['mode'])
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)
    if hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
        raise _TargetError('MIME restoration hash mismatch')


def _restore_mime(backups):
    """Restore each entry independently, real-profile files first.

    Returns (retained, dropped, reasons): the entries that could not be
    restored, kept for the next recovery; the number of private entries whose
    XDG root, or the directory holding them, no longer exists; and static
    reasons. Path order is no guide: /tmp sorts before /var/home.
    """
    retained, dropped, reasons = [], 0, set()
    for item in sorted(backups, key=lambda item: _private_scope(item)[0]):
        private, root = _private_scope(item)
        gone = item['bytes'] is not None and not Path(item['path']).parent.is_dir()
        if private and (gone or (root and not os.path.lexists(root))):
            dropped += 1  # Already cleaned with its private root or directory.
            continue
        if gone:
            # Recreating a real profile directory someone removed is not ours
            # to decide: keep refusing, by name, until it is put back.
            retained.append(item)
            reasons.add('real MIME directory missing; restore it, then rerun')
            continue
        try:
            _restore_entry(item)
        except _TargetError as error:
            retained.append(item)  # Never blocks the others.
            reasons.add(str(error))
        except Exception as error:
            retained.append(item)
            reasons.add('MIME write failed: ' + type(error).__name__)
    return retained, dropped, sorted(reasons)


def _environment(config, data):
    env = dict(os.environ, XDG_CONFIG_HOME=str(config), XDG_DATA_HOME=str(data))
    for name in ('MOZ_NO_REMOTE', 'MOZ_LOG', 'MOZ_LOG_FILE'):
        env.pop(name, None)
    return env


_FIREFOX = {'firefox', 'firefox-bin', 'firefox-esr'}


def _uptime_ticks():
    return int(float(Path('/proc/uptime').read_text().split()[0]) * os.sysconf('SC_CLK_TCK'))


def _start_ticks(pid):
    # Field 22 (starttime), counted after the parenthesized command name.
    return Path('/proc/%d/stat' % pid).read_text().rsplit(')', 1)[1].split()[19]


def _firefox_mains():
    """(pid, start ticks, argv) of this user's Firefox parent processes."""
    found = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if entry.stat().st_uid != os.getuid():
                continue
            argv = (entry / 'cmdline').read_bytes().split(b'\0')
            if (os.path.basename(argv[0]).decode(errors='replace') not in _FIREFOX
                    or b'-contentproc' in argv):
                continue
            found.append((int(entry.name), _start_ticks(int(entry.name)), argv))
        except (OSError, IndexError):
            continue  # Exited while being read.
    return found


def _foreign(state):
    """Firefox parents started during this transaction outside its private profile.

    Returns identities only; argv may hold the dispatched URL and never leaves
    the target.
    """
    if state.get('boot') not in (None, _boot_id()):
        return []  # ``since`` counts from an earlier boot: no Firefox now is ours.
    profile = state['directory'].encode()
    return [[pid, start] for pid, start, argv in _firefox_mains()
            if profile not in argv and int(start) >= state['since']]


def _boot_id():
    return Path('/proc/sys/kernel/random/boot_id').read_text().strip()


def _session_members(pid, start):
    """Exact (pid, start) identities still in the private Firefox's session.

    Popen(start_new_session=True) makes Firefox a session leader; children
    that outlive it are reparented but keep its session id. Linux does not
    reuse a PID that still names a live session, so the members are ours
    while that PID is free or still the exact leader.
    """
    if _start_ticks_or_none(pid) not in (None, start):
        return []
    found = []
    for entry in Path('/proc').iterdir():
        if entry.name.isdigit() and int(entry.name) != pid:
            try:
                if entry.stat().st_uid != os.getuid():
                    continue
                fields = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
                if int(fields[3]) == pid and int(fields[19]) >= int(start):
                    found.append([int(entry.name), fields[19]])
            except (OSError, IndexError, ValueError):
                continue
    return found


def _descendants(pid, start):
    """Exact (pid, start) identities below one process, e.g. Firefox's crash helper."""
    children = {}
    for entry in Path('/proc').iterdir():
        if entry.name.isdigit():
            try:
                fields = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
                children.setdefault(int(fields[1]), []).append([int(entry.name), fields[19]])
            except (OSError, IndexError):
                continue
    if _start_ticks_or_none(pid) != start:
        return []
    found, queue = [], [pid]
    while queue:
        for child in children.get(queue.pop(), []):
            found.append(child)
            queue.append(child[0])
    return found


def _start_ticks_or_none(pid):
    try:
        return _start_ticks(pid)
    except (OSError, IndexError):
        return None


def _terminate(pid, start):
    """Stop one exact process identity, never a name or substring match."""
    try:
        if _start_ticks(pid) != start:
            return
        os.kill(pid, 15)
        deadline = time.monotonic() + 8
        while _alive([pid, start]) and time.monotonic() < deadline:
            time.sleep(.1)
        if _alive([pid, start]):
            os.kill(pid, 9)
    except (OSError, IndexError):
        pass  # Already exited.


def _write_transaction(state):
    stored = Path(state['directory']) / 'transaction.json'
    temporary = stored.with_name('.transaction.json')
    temporary.write_text(json.dumps(state))
    os.replace(temporary, stored)


def _make_directories(path, created):
    """mkdir -p that records each directory it created, for restore to remove."""
    missing = []
    while not os.path.lexists(path):
        missing.append(path)
        path = path.parent
    for directory in reversed(missing):
        try:
            directory.mkdir()
        except FileExistsError:
            continue  # Created concurrently by someone else: not ours.
        created.append(str(directory))


def _locations(options):
    home = Path(pwd.getpwuid(os.getuid()).pw_dir)
    expand = lambda value: Path(value.replace('~', str(home), 1)) if value.startswith('~/') else Path(value)
    config = expand(options.get('config_home', str(home / '.config')))
    data = expand(options.get('data_home', str(home / '.local/share')))
    root = expand(options.get('profile_root', str(home / '.mozilla/firefox')))
    # Portal dispatch uses the real profile; gio/xdg-open fallback uses the
    # app's private XDG profile. Guard both, with independent byte backups.
    private = (Path(os.environ.get('XDG_CONFIG_HOME') or config),
               Path(os.environ.get('XDG_DATA_HOME') or data))
    return config, data, root, list(dict.fromkeys([(config, data), private]))


def _alive(identity):
    """True while this exact process identity runs (an unreaped zombie has ended)."""
    try:
        fields = Path('/proc/%d/stat' % identity[0]).read_text().rsplit(')', 1)[1].split()
        return fields[19] == identity[1] and fields[0] != 'Z'
    except (OSError, IndexError, TypeError):
        return False


def _recover(root, wait):
    """Restore every transaction an interrupted run left under this profile root.

    Default-handler transactions are exclusive per account, so a transaction
    record present at setup is abandoned, unless the helper that is still
    preparing it is alive (a controller whose transport failed mid-setup).
    """
    count, dropped, failure = 0, 0, None
    for stored in sorted(root.glob('putgd-surface-*/transaction.json')):
        state = json.loads(stored.read_text())
        deadline = time.monotonic() + wait
        while _alive(state.get('helper')) and state.get('helper') != [os.getpid(), _start_ticks(os.getpid())]:
            if time.monotonic() >= deadline:
                raise _TargetError('Firefox setup still running; transaction retained for recovery')
            time.sleep(.25)
        # Recorded identities only: a fresh scan this long after the
        # transaction began could reach an unrelated, later Firefox.
        try:
            dropped += _restore({'directory': str(stored.parent)}, scan=False).get('dropped', 0)
        except _TargetError as error:
            failure = error  # The other transactions still restore.
            continue
        count += 1
    if failure:
        raise failure
    return {'recovered': count, **({'dropped': dropped} if dropped else {})}


def _prepare(options):
    config, data, root, profiles = _locations(options)
    timeouts = _timeouts(options)
    command = options.get('command', ['firefox'])
    if not isinstance(command, list) or not command or not all(isinstance(x, str) for x in command):
        raise ValueError('browser.command must be argv')
    if options.get('live_downloads') and any(arg.startswith(('-private', '--private')) for arg in command):
        raise _TargetError('Streaming recipient Firefox must not use a private-browsing command flag')
    recovered = _recover(root, timeouts['cleanup'] - 30)
    created = []
    _make_directories(root, created)
    # Only the private Firefox (and snap, for its own data) writes below a
    # profile root setup created: restore removes such a tree whole.
    browser_created = list(created)
    for directory in sorted({*(c for c, d in profiles), *(d / 'applications' for c, d in profiles)}):
        _make_directories(directory, created)
    try:
        backups = _backups(profiles)
        directory = Path(tempfile.mkdtemp(prefix='putgd-surface-', dir=root))
    except BaseException:
        _remove_created(created, browser_created)
        raise
    os.chmod(directory, 0o700)
    token = directory.name.replace('-', '')
    desktops = list(dict.fromkeys(d / 'applications' / (token + '.desktop') for c, d in profiles))
    desktop = desktops[0]
    state = {'directory': str(directory), 'desktops': [str(p) for p in desktops], 'created': created,
             'browser_created': browser_created,
             'backups': backups, 'pid': None, 'since': _uptime_ticks(), 'boot': _boot_id(), 'foreign': [],
             'helper': [os.getpid(), _start_ticks(os.getpid())], 'timeouts': timeouts}
    try:
        # Recorded before anything else changes, so recovery can undo any step.
        _write_transaction(state)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            state['port'] = probe.getsockname()[1]
        # External links open as a new tab of the kiosk window, and no
        # first-run or privacy-notice tab competes with the dispatched page.
        # The disposable profile must publish its real input target even when
        # the desktop's automatic accessibility setting is off. This changes
        # neither the desktop setting nor the runtime/PID ownership checks.
        preferences = {'marionette.port': state['port'], 'accessibility.force_disabled': -1,
                       'browser.shell.checkDefaultBrowser': False,
                       'browser.startup.homepage_override.mstone': 'ignore',
                       'browser.startup.page': 0, 'browser.aboutwelcome.enabled': False,
                       'browser.newtabpage.enabled': False, 'datareporting.policy.dataSubmissionEnabled': False,
                       'datareporting.policy.firstRunURL': '', 'toolkit.telemetry.reportingpolicy.firstRun': False,
                       'startup.homepage_welcome_url': '', 'startup.homepage_welcome_url.additional': '',
                       'browser.link.open_newwindow': 3, 'browser.link.open_newwindow.override.external': 3,
                       'browser.sessionstore.resume_from_crash': False,
                       'signon.rememberSignons': False, 'browser.privatebrowsing.autostart': True}
        downloads = directory/'downloads'
        downloads.mkdir(mode=0o700)
        preferences.update({'browser.download.folderList': 2, 'browser.download.dir': str(downloads),
                            'browser.download.useDownloadDir': True, 'browser.download.always_ask_before_handling_new_types': False,
                            'browser.helperApps.neverAsk.saveToDisk': 'image/png,application/octet-stream,application/zip',
                            'browser.cache.disk.enable': False, 'browser.cache.memory.enable': False,
                            'network.http.use-cache': False})
        if options.get('live_downloads'):
            fs = subprocess.run(['stat', '-f', '-c', '%T', str(directory)], capture_output=True, text=True, check=True)
            if fs.stdout.strip() != 'tmpfs':
                raise _TargetError('Live download popup policy requires the admitted tmpfs profile')
            # A public recipient's streamed download can open a helper after
            # asynchronous decryption, beyond Firefox's transient activation.
            # Keep this permission inside the disposable guest RAM profile.
            preferences['dom.disable_open_during_load'] = False
            preferences['browser.privatebrowsing.autostart'] = False
            preferences['dom.serviceWorkers.enabled'] = True
            preferences['dom.indexedDB.enabled'] = True
        (directory / 'user.js').write_text(''.join('user_pref(%s, %s);\n' % (json.dumps(k), json.dumps(v))
                                                  for k, v in preferences.items()))
        # Cache and state are the transaction's own, removed with it: the
        # run's private XDG root may be gone before this Firefox exits. Pin
        # runtime for the desktop handler too: GNOME launches it from its own
        # activation environment, and a new parent must retain our ownership.
        scoped = {'XDG_CACHE_HOME': str(directory / 'xdg-cache'), 'XDG_STATE_HOME': str(directory / 'xdg-state'),
                  'XDG_RUNTIME_DIR': os.environ['XDG_RUNTIME_DIR']}
        if display := os.environ.get('WAYLAND_DISPLAY'):
            # Target already resolved this socket before isolating runtime.
            # A relative native-session name would resolve inside our private
            # runtime when GNOME activates the handler instead of remoting.
            scoped['WAYLAND_DISPLAY'] = display
        env = dict(_environment(config, data), **scoped)
        env['MOZ_APP_REMOTINGNAME'] = token
        # The handler forwards to the running instance through profile-keyed
        # remoting. If that instance has gone, the handler starts one on the
        # same profile and Marionette port, so observation still attaches.
        argv = command + ['--profile', str(directory), '--kiosk']
        desktop_argv = (['MOZ_APP_REMOTINGNAME=' + token, 'XDG_CONFIG_HOME=' + str(config),
                         'XDG_DATA_HOME=' + str(data)] + [k + '=' + v for k, v in scoped.items()]
                        + argv + ['--marionette', '%u'])
        def desktop_quote(value):
            return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$') + '"'
        # Keep the executable token unquoted. GLib ignores handlers whose
        # program token is quoted, although xdg-mime default reports success.
        desktop.write_text('[Desktop Entry]\nType=Application\nName=put.gd isolated Firefox\n'
                           'Exec=/usr/bin/env ' + ' '.join(desktop_quote(x) if x != '%u' else x for x in desktop_argv) +
                           '\nTerminal=false\nMimeType=x-scheme-handler/http;x-scheme-handler/https;\n')
        for alternate in desktops[1:]:
            alternate.write_bytes(desktop.read_bytes())
        process = subprocess.Popen(argv + ['--marionette', 'about:blank'], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        state['pid'] = process.pid
        state['pid_start'] = _start_ticks(process.pid)
        _write_transaction(state)
        deadline = time.monotonic() + timeouts['ready']
        while True:
            try:
                client = _Marionette(state['port'], timeouts['protocol'])
                client.close()
                break
            except (OSError, RuntimeError):
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise _TargetError('Private Firefox did not become ready within %g s' % timeouts['ready']) from None
                time.sleep(.25)
        for profile_config, profile_data in profiles:
            mime_env = _environment(profile_config, profile_data)
            for scheme in ('http', 'https'):
                subprocess.run(['xdg-mime', 'default', desktop.name, 'x-scheme-handler/' + scheme],
                               env=mime_env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
                check = subprocess.run(['xdg-mime', 'query', 'default', 'x-scheme-handler/' + scheme],
                                       env=mime_env, capture_output=True, text=True, check=True, timeout=10)
                if check.stdout.strip() != desktop.name:
                    raise _TargetError('Real or isolated MIME handler verification failed')
        return {'directory': str(directory), 'port': state['port'], **recovered}
    except BaseException:
        _restore(state)
        raise


# What a first private Firefox start writes beside its --profile: crash and
# ping state in the profile root, an empty ~/.mozilla/extensions above it.
_FIREFOX_STATE = {'Crash Reports', 'Pending Pings', 'Profile Groups', 'installs.ini', 'profiles.ini'}


def _firefox_state(path):
    if path.is_symlink():
        return False
    if path.name == 'extensions':
        return path.is_dir() and not any(path.iterdir())
    return path.name in _FIREFOX_STATE


def _remove_created(created, browser_created=()):
    """Remove exactly the directories setup recorded, deepest first.

    A directory goes only when empty, or, if setup made it for the browser,
    when it holds only Firefox's own state named above. Anything else
    (another snap's data, a file or empty directory the app or desktop wrote)
    keeps that directory and every ancestor.
    """
    for directory in sorted(map(Path, created), key=lambda path: len(path.parts), reverse=True):
        try:
            if directory.is_symlink() or not directory.is_dir():
                continue
            entries = list(directory.iterdir())
            if str(directory) in browser_created and all(_firefox_state(entry) for entry in entries):
                for entry in entries:
                    if entry.is_dir():
                        shutil.rmtree(entry)
                    else:
                        entry.unlink()
            os.rmdir(directory)
        except OSError:
            pass  # Holds something setup did not record: it stays.


def _restore(state, scan=True):
    directory = Path(state['directory'])
    # A caller passes only a directory identifier, not mutable backup bytes.
    stored = directory / 'transaction.json'
    if stored.exists():
        state = json.loads(stored.read_text())
    elif 'backups' not in state:
        if not os.path.lexists(directory):
            return {'restored': True, 'foreign': 0}  # Already restored, e.g. by recovery.
        raise _TargetError('Firefox transaction record missing; profile retained')
    retained, dropped, reasons = _restore_mime(state['backups'])
    # After a reboot every recorded identity (and session id) is stale.
    current = state.get('boot') in (None, _boot_id())
    # Exact identities only: our launch, any instance on our private profile
    # (a handler-started one), and Firefox processes started outside it during
    # this transaction, whether observation recorded them or not.
    launch = [state['pid'], state.get('pid_start')] if state.get('pid') and current else None
    targets = [launch] if launch else []
    profile = state['directory'].encode()
    targets += [[pid, start] for pid, start, argv in _firefox_mains() if profile in argv]
    foreign = state.get('foreign', []) if current else []
    foreign += [item for item in (_foreign(state) if scan else []) if item not in foreign]
    # Children are recorded first: once a parent exits they are reparented,
    # and Firefox's crash helper still writes into the profile root then.
    # Those of our launch stay in its session even after it has exited.
    descendants = [child for pid, start in targets + foreign for child in _descendants(pid, start)]
    if launch and launch[1]:
        descendants += [item for item in _session_members(*launch) if item not in descendants]
    for pid, start in targets + foreign + descendants:
        _terminate(pid, start)
    for desktop in state['desktops']:
        try:
            Path(desktop).unlink(missing_ok=True)
        except OSError:
            pass  # A vanished private root has taken its handler with it.
    if retained:
        state['backups'] = retained
        _write_transaction(state)
        raise _TargetError('MIME restoration failed for %d file(s) (%s); backup retained'
                           % (len(retained), '; '.join(reasons)))
    # The record goes last, so an interrupted restore is still recoverable.
    if directory.exists():
        for child in directory.iterdir():
            if child.name == 'transaction.json':
                continue
            if child.is_dir() and not child.is_symlink():
                shutil.rmtree(child)
            else:
                child.unlink()
        stored.unlink(missing_ok=True)
        directory.rmdir()
    _remove_created(state.get('created', []), state.get('browser_created', []))
    return {'restored': True, 'foreign': len(foreign), **({'dropped': dropped} if dropped else {})}


def _blank(client, timeout):
    """Leave one tab, on about:blank, selected; True when that is proven."""
    handles = client.call('WebDriver:GetWindowHandles')
    if not handles:
        raise _TargetError('Private Firefox has no window')
    # Fewer tabs keep later observation short; the kiosk keeps its window.
    for handle in handles[1:]:
        try:
            client.call('WebDriver:SwitchToWindow', {'handle': handle, 'focus': False})
            client.call('WebDriver:CloseWindow')
        except _TargetError:
            if client.broken:
                raise  # Otherwise the tab closed itself meanwhile.
    client.call('WebDriver:SwitchToWindow', {'handle': handles[0]})
    try:
        client.call('WebDriver:Navigate', {'url': 'about:blank'})
    except _Timeout:
        if client.broken:
            raise
        # A slow (software-rendered) load; the page itself is checked below.
    deadline = time.monotonic() + timeout
    while True:
        safe = client.script("return location.href==='about:blank' && document.hasFocus() && "
                             "innerWidth>=screen.width && innerHeight>=screen.height;")
        if safe is True or time.monotonic() >= deadline:
            return safe is True
        time.sleep(.25)


def _observe(client, expect):
    """The matching, painted page; {} when none yet; {'pending': reason} on a timeout."""
    # Every top-level browsing context (tab) of the private instance.
    # blank navigated the earlier one, so only a dispatched page matches.
    # focus=False: inspecting a tab must not select it, so visibility
    # and focus prove the dispatched page itself is in the foreground.
    try:
        handles = client.call('WebDriver:GetWindowHandles')
    except _TargetError as error:
        return {'pending': str(error)}
    reason = None
    for handle in handles:
        try:
            client.call('WebDriver:SwitchToWindow', {'handle': handle, 'focus': False})
            result = client.script(_OBSERVE, [expect])
            if not (isinstance(result, dict) and result.get('matched')):
                continue
            if client.script(_PAINTED, asynchronous=True) is not True:
                continue
        except _TargetError as error:
            if client.broken:
                return {'pending': str(error)}  # The next poll reconnects.
            if isinstance(error, _Timeout):
                reason = str(error)
            continue  # A closed/navigating/slow tab must not hide a later match.
        time.sleep(.5)  # Compositor presentation after the content paint.
        return result
    return {'pending': reason} if reason else {}


def _live_saved_capture(values):
    saved = Path(values['saved_path'])
    root = Path(values['app_root'])
    if saved.is_symlink() or not saved.resolve(strict=True).is_relative_to(root.resolve(strict=True)):
        raise _TargetError('Saved capture is outside the owned app root')
    with os.fdopen(os.open(saved, os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK), 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid() or not 0<info.st_size<=32*1024*1024:
            raise _TargetError('Saved capture is not an owned bounded regular file')
        original=stream.read(32*1024*1024+1)
    return original


def _target(action, values):
    if action in ('prepare', 'recover'):
        if action == 'prepare':
            return _prepare(values)
        return _recover(_locations(values)[2], values.get('wait', 0))
    if action == 'restore':
        return _restore(values)
    stored = Path(values['directory']) / 'transaction.json'
    transaction = json.loads(stored.read_text())
    timeout = transaction.get('timeouts', _TIMEOUTS)['protocol']
    try:
        client = _Marionette(values['port'], timeout)
    except _TargetError as error:
        if action != 'observe':
            raise
        client, result = None, {'pending': str(error)}
    try:
        if action == 'blank':
            safe = _blank(client, timeout)
            # Another Firefox's window may show a token-bearing URL.
            return {'blank': True, 'safe': safe and not _foreign(transaction)}
        if action == 'observe':
            if client is not None:
                result = _observe(client, values['expect'])
            # A stray Firefox fails the run even beside a matching private
            # tab: its window may show the link, so no frame is taken.
            foreign = _foreign(transaction)
            if foreign:
                # Recorded so restore stops exactly these processes.
                transaction['foreign'] = [item for item in transaction.get('foreign', []) if item not in foreign] + foreign
                _write_transaction(transaction)
                return {'matched': False, 'foreign': len(foreign)}
            return result  # falsy until ctx.wait's deadline
        if action == 'download-digest':
            directory = Path(transaction['directory'])/'downloads'
            # The live fixture insists on a runtime profile. Verify the actual
            # mount as well before letting Firefox write any recipient bytes.
            fs = subprocess.run(['stat', '-f', '-c', '%T', str(directory)], capture_output=True, text=True, check=True)
            if fs.stdout.strip() != 'tmpfs' or any(directory.iterdir()):
                raise _TargetError('Live recipient download requires a new empty tmpfs directory')
            original = _live_saved_capture(values)
            if values['kind'] == 'proton':
                deadline = time.monotonic()+30
                while True:
                    node = client.script("""
// Filename readiness can precede decryption. This lane uploads PNGs: wait
// for their decrypted preview before activating the recipient download.
const preview=[...document.images].some(e=>(e.currentSrc||e.src).startsWith('blob:')&&e.complete&&e.naturalWidth>0);
if(!preview)return null;
const nodes=[...document.querySelectorAll('button,a,[role=button]')].filter(el=>{
 const r=el.getBoundingClientRect();const label=(el.getAttribute('aria-label')||el.textContent||'').trim();
 return r.width>0&&r.height>0&&!el.disabled&&el.getAttribute('aria-disabled')!=='true'&&
  getComputedStyle(el).pointerEvents!=='none'&&/^Download(?: file)?$/i.test(label);
});
return nodes.length===1?nodes[0]:null;
""")
                    if node or time.monotonic()>=deadline:
                        break
                    time.sleep(.25)
                element = node.get('element-6066-11e4-a52e-4f735466cecf') if isinstance(node, dict) else None
                clicked = bool(element)
                if clicked:
                    # WebDriver's trusted pointer action supplies user activation
                    # for Proton's download/popup flow; DOM click() does not.
                    client.call('WebDriver:ElementClick', {'id': element})
            elif values['kind'] == 's3':
                # Fetch the original image in the recipient Firefox origin;
                # canvas serialization would not prove the uploaded bytes.
                received = client.script("""
const done=arguments[arguments.length-1];
const images=[...document.images].filter(e=>e.complete&&e.naturalWidth>0);
if(images.length!==1){done(null);return;}
fetch(images[0].currentSrc||images[0].src,{cache:'no-store',credentials:'omit'}).then(async r=>{
 if(!r.ok){done(null);return;}const data=await r.arrayBuffer();
 if(!data.byteLength||data.byteLength>32*1024*1024){done(null);return;}
 const hash=await crypto.subtle.digest('SHA-256',data);
 done({bytes:data.byteLength,sha256:[...new Uint8Array(hash)].map(v=>v.toString(16).padStart(2,'0')).join('')});
}).catch(()=>done(null));
""", asynchronous=True)
                if not isinstance(received, dict) or set(received)!={'bytes','sha256'}:
                    raise _TargetError('Recipient Firefox could not hash the original S3 image')
                return {**received, 'matches':received['bytes']==len(original) and
                        received['sha256']==hashlib.sha256(original).hexdigest()}
            else:
                raise _TargetError('Unsupported live recipient provider')
            if not clicked:
                raise _TargetError('Recipient page has no unique enabled Download control')
            deadline = time.monotonic()+120
            while time.monotonic()<deadline:
                files = list(directory.iterdir())
                complete = [p for p in files if not p.name.endswith(('.part', '.tmp'))]
                if len(complete)==1 and len(files)==1:
                    path = complete[0]
                    info = path.lstat()
                    if (not stat.S_ISREG(info.st_mode) or path.is_symlink() or info.st_uid!=os.getuid()
                            or not 0<info.st_size<=32*1024*1024):
                        raise _TargetError('Recipient download is not an owned bounded regular file')
                    with os.fdopen(os.open(path, os.O_RDONLY|os.O_NOFOLLOW), 'rb') as stream:
                        current=os.fstat(stream.fileno())
                        if (not stat.S_ISREG(current.st_mode) or current.st_uid!=os.getuid()
                                or current.st_ino!=info.st_ino or current.st_dev!=info.st_dev):
                            raise _TargetError('Recipient download changed identity before reading')
                        data=stream.read(32*1024*1024+1)
                    if len(data)>32*1024*1024:
                        raise _TargetError('Recipient download exceeded the bounded in-memory limit')
                    digest=hashlib.sha256(data).hexdigest()
                    return {'bytes':len(data),'sha256':digest,
                            'matches':len(original)<=32*1024*1024 and data==original and
                                      digest==hashlib.sha256(original).hexdigest()}
                time.sleep(.5)
            diagnostic = client.script("""
const text=document.body?.innerText||'';
return {serviceWorkerAPI:typeof navigator.serviceWorker==='object',
 downloading:/downloading|decrypting/i.test(text),downloadError:/download failed|unable to download|could not download/i.test(text),
 blobPreview:[...document.images].some(e=>(e.currentSrc||e.src).startsWith('blob:')&&e.complete&&e.naturalWidth>0),
 focused:document.hasFocus()};
""")
            # Fixed boolean fields only, never page text, hrefs or filenames.
            raise _TargetError('Recipient download deadline: '+json.dumps(diagnostic, sort_keys=True))
        if action == 'unavailable':
            # URL lives only in this target process and Firefox, never in
            # arguments, transaction metadata or a browser receipt.
            url = client.script('return location.href;')
            handle = client.call('WebDriver:GetWindowHandle')
            client.call('WebDriver:SwitchToWindow', {'handle':handle, 'focus':True})
            if not client.script('return document.hasFocus() && innerWidth>=screen.width && innerHeight>=screen.height;'):
                raise _TargetError('Recipient kiosk did not regain foreground for fresh navigation')
            client.call('WebDriver:Navigate', {'url':'about:blank'})
            # All HTTP memory/disk caches are disabled in this private profile.
            # Start from blank so the former preview cannot prove absence.
            try:
                client.call('WebDriver:Navigate', {'url':url})
            except _Timeout:
                if client.broken:
                    raise
                # Page-load timeout does not cancel navigation. Observe its
                # final rendered state over the provider-specific deadline.
            deadline=time.monotonic()+(120 if values['kind']=='proton' else 45)
            while True:
                result=client.script("""
const text=(document.body?.innerText||'').trim();
const status=performance.getEntriesByType('navigation')[0]?.responseStatus||0;
const media=[...document.querySelectorAll('img,video,canvas')].some(e=>
 e.tagName==='IMG'?e.complete&&e.naturalWidth>0&&
  (arguments[0]!=='proton'||(e.currentSrc||e.src).startsWith('blob:')):
 e.tagName==='VIDEO'?e.readyState>=2:false);
const hostOK=location.hostname===new URL(arguments[1]).hostname;
const missing=arguments[0]==='proton'?
 /not found|unavailable|has expired|has been revoked|has been deleted|has been disabled|no longer (?:available|exists)|cannot be accessed|could not be found|does not exist|doesn't exist/i.test(text):
 ([403,404,410].includes(status)||status===0)&&/NoSuchKey|AccessDenied|not found|404|403|410/i.test(text);
return {unavailable:hostOK&&document.readyState==='complete'&&missing&&!media,
        missing,media,hostOK,complete:document.readyState==='complete',status,
        httpUnavailable:[403,404,410].includes(status)};
""", [values['kind'], url])
                if (result['unavailable'] or (values['kind']=='s3' and result['complete'] and result['hostOK']
                        and result['status'] in (200, 403, 404, 410)) or time.monotonic()>=deadline):
                    return result
                time.sleep(.5)
        raise ValueError('Unknown browser helper action')
    finally:
        if client is not None:
            client.close()


if __name__ == '__main__':
    import signal
    import sys
    # A dropped SSH session must not cut a step short: ignore the hangup and
    # leave the session's process group. A step in progress completes; a
    # transaction it leaves is restored by the next setup or recover step.
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    try:
        os.setsid()
    except OSError:
        pass  # Already a process group leader.
    try:
        answer = _target(sys.argv[1], json.loads(base64.b64decode(sys.argv[2])))
    except BaseException as error:
        answer = {'error': str(error) if isinstance(error, _TargetError) else type(error).__name__}
        # Only our static allowlisted errors, never protocol/OS text or traces.
    try:
        print(json.dumps(answer), flush=True)
    except OSError:
        # The controller has gone; the step itself completed.
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
