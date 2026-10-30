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
first restores any transaction an interrupted run left behind.
"""
from __future__ import annotations

import base64
from contextlib import contextmanager
from dataclasses import asdict
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import socket
import stat
import subprocess
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


@contextmanager
def default_browser(ctx, browser='firefox'):
    """Temporarily install an isolated real Firefox as the http(s) handler.

    Restores every touched mimeapps file byte for byte, verifies SHA-256, and
    leaves unrelated desktop entries (including the guarded recorder) alone.
    Restoration errors are fatal and leave the private backup for recovery.
    """
    if browser != 'firefox':
        raise ValueError('Only Firefox content verification is implemented')
    if id(ctx) in _ACTIVE:
        raise RuntimeError('Nested default_browser transactions are unsupported')
    options = ctx.profile.get('browser', {})
    timeouts = _timeouts(options)
    try:
        state = _remote(ctx, 'prepare', options, timeout=timeouts['helper'] + timeouts['ready'])
    except BaseException:
        # The target step may have outlived its transport (deadline, dropped
        # session): restore whatever it left before reporting the failure.
        try:
            _remote(ctx, 'recover', {**options, 'wait': timeouts['cleanup'] - 30},
                    timeout=timeouts['cleanup'], cleanup=True)
        except Exception as error:
            ctx.review('Interrupted Firefox setup not yet recovered (' + str(error)[:200] +
                       '); the next default_browser entry on this account restores it')
        raise
    if state.get('recovered'):
        ctx.note('Restored %d unfinished Firefox handler transaction(s) before setup' % state['recovered'])
    _ACTIVE[id(ctx)] = {'state': state, 'timeouts': timeouts}
    try:
        yield
    except BaseException:
        _restore_transaction(ctx, state, timeouts, failing=True)
        raise
    else:
        _restore_transaction(ctx, state, timeouts, failing=False)
    finally:
        _ACTIVE.pop(id(ctx), None)


def _restore_transaction(ctx, state, timeouts, failing):
    try:
        restored = _remote(ctx, 'restore', {'directory': state['directory']},
                           timeout=timeouts['cleanup'], cleanup=True)
    except Exception as error:
        ctx.review('Firefox handler restore incomplete (' + str(error)[:200] +
                   '); the next default_browser entry on this account restores it')
        raise
    ctx.expect(restored.get('restored') is True, 'browser MIME bytes and hashes restored')
    ctx.note('Firefox handler restored; MIME SHA-256 checks passed')
    if restored.get('foreign'):
        ctx.note('Stopped %d Firefox process(es) started outside the private profile' % restored['foreign'])
        if not failing:
            ctx.expect(False, 'no Firefox started outside the private profile during the transaction')


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
                              timeout=timeouts['page'])
        except RecipeFailed as error:
            if pending:
                raise RecipeFailed(str(error) + '; last Firefox answer: ' + pending[0]) from None
            raise
        ctx.expect(not result.get('foreign'),
                   'external dispatch reached the private Marionette Firefox, '
                   'not a Firefox started outside its profile')
        ctx.expect(result.get('matched'), 'real Firefox URL, title and page content match')
        frames.append(asdict(ctx.frame('browser-content', window=None)))
        receipt = {key: result[key] for key in ('url_host', 'title', 'matched')}
        receipt['frames'] = frames
        ctx.note(json.dumps({key: receipt[key] for key in ('url_host', 'title', 'matched')}))
        return receipt
    except Exception:
        # KeyboardInterrupt and SystemExit skip the failure frame; restoration
        # still runs in default_browser.
        _failure_frame(ctx, state, timeouts, frames)
        raise


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
        frames.append(asdict(ctx.frame('browser-failure-safe-desktop', window=None)))
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
// A Download button alone says nothing about decryption or completed bytes.
const status = performance.getEntriesByType('navigation')[0]?.responseStatus || 0;
const error = /^(404|403|500|AccessDenied|NoSuchKey|Error response)/i.test(text.trim()) ||
              /Problem loading page|Server Not Found|Error response|Access Denied/i.test(title);
const content = e.kind==='proton' ? text.includes(e.name) && media :
                (media || text.trim().length>0);
const hostOK = e.url_host ? u.hostname===e.url_host :
               e.kind==='proton' ? /(^|\.)proton\.(me|ch)$/.test(u.hostname) : true;
const matched = ['http:','https:'].includes(u.protocol) && hostOK && named &&
  title.length>0 && document.visibilityState==='visible' && document.hasFocus() &&
  innerWidth>=screen.width && innerHeight>=screen.height &&
  document.readyState==='complete' && status<400 && !error &&
  content && (!e.preview || media || (e.kind!=='proton' && text.trim().length>0)) &&
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


def _restore_mime(backups):
    for item in backups:
        path = Path(item['path'])
        if item['bytes'] is None:
            if path.is_symlink():
                raise _TargetError('MIME path became a symlink; backup retained')
            path.unlink(missing_ok=True)
            if path.exists():
                raise _TargetError('MIME removal failed')
        else:
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
                raise _TargetError('MIME restoration hash mismatch; backup retained')


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
    profile = state['directory'].encode()
    return [[pid, start] for pid, start, argv in _firefox_mains()
            if profile not in argv and int(start) >= state['since']]


def _terminate(pid, start):
    """Stop one exact process identity, never a name or substring match."""
    path = Path('/proc/%d/stat' % pid)
    try:
        if _start_ticks(pid) != start:
            return
        os.kill(pid, 15)
        deadline = time.monotonic() + 8
        while path.exists() and time.monotonic() < deadline:
            time.sleep(.1)
        if path.exists() and _start_ticks(pid) == start:
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
    count = 0
    for stored in sorted(root.glob('putgd-surface-*/transaction.json')):
        state = json.loads(stored.read_text())
        deadline = time.monotonic() + wait
        while _alive(state.get('helper')) and state.get('helper') != [os.getpid(), _start_ticks(os.getpid())]:
            if time.monotonic() >= deadline:
                raise _TargetError('Firefox setup still running; transaction retained for recovery')
            time.sleep(.25)
        # Recorded identities only: a fresh scan this long after the
        # transaction began could reach an unrelated, later Firefox.
        _restore({'directory': str(stored.parent)}, scan=False)
        count += 1
    return count


def _prepare(options):
    config, data, root, profiles = _locations(options)
    timeouts = _timeouts(options)
    command = options.get('command', ['firefox'])
    if not isinstance(command, list) or not command or not all(isinstance(x, str) for x in command):
        raise ValueError('browser.command must be argv')
    recovered = _recover(root, timeouts['cleanup'] - 30)
    created = []
    for directory in sorted({root, *(c for c, d in profiles), *(d / 'applications' for c, d in profiles)}):
        _make_directories(directory, created)
    try:
        backups = _snapshot(sorted({path for c, d in profiles for path in _mime_paths(c, d)}))
        directory = Path(tempfile.mkdtemp(prefix='putgd-surface-', dir=root))
    except BaseException:
        _remove_created(created)
        raise
    os.chmod(directory, 0o700)
    token = directory.name.replace('-', '')
    desktops = list(dict.fromkeys(d / 'applications' / (token + '.desktop') for c, d in profiles))
    desktop = desktops[0]
    state = {'directory': str(directory), 'desktops': [str(p) for p in desktops], 'created': created,
             'backups': backups, 'pid': None, 'since': _uptime_ticks(), 'foreign': [],
             'helper': [os.getpid(), _start_ticks(os.getpid())], 'timeouts': timeouts}
    try:
        # Recorded before anything else changes, so recovery can undo any step.
        _write_transaction(state)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            state['port'] = probe.getsockname()[1]
        # External links open as a new tab of the kiosk window, and no
        # first-run or privacy-notice tab competes with the dispatched page.
        preferences = {'marionette.port': state['port'], 'browser.shell.checkDefaultBrowser': False,
                       'browser.startup.homepage_override.mstone': 'ignore',
                       'browser.startup.page': 0, 'browser.aboutwelcome.enabled': False,
                       'browser.newtabpage.enabled': False, 'datareporting.policy.dataSubmissionEnabled': False,
                       'datareporting.policy.firstRunURL': '', 'toolkit.telemetry.reportingpolicy.firstRun': False,
                       'startup.homepage_welcome_url': '', 'startup.homepage_welcome_url.additional': '',
                       'browser.link.open_newwindow': 3, 'browser.link.open_newwindow.override.external': 3,
                       'browser.sessionstore.resume_from_crash': False,
                       'signon.rememberSignons': False, 'browser.privatebrowsing.autostart': True}
        (directory / 'user.js').write_text(''.join('user_pref(%s, %s);\n' % (json.dumps(k), json.dumps(v))
                                                  for k, v in preferences.items()))
        env = _environment(config, data)
        env['MOZ_APP_REMOTINGNAME'] = token
        # The handler forwards to the running instance through profile-keyed
        # remoting. If that instance has gone, the handler starts one on the
        # same profile and Marionette port, so observation still attaches.
        argv = command + ['--profile', str(directory), '--kiosk']
        desktop_argv = ['MOZ_APP_REMOTINGNAME=' + token, 'XDG_CONFIG_HOME=' + str(config),
                        'XDG_DATA_HOME=' + str(data)] + argv + ['--marionette', '%u']
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
        return {'directory': str(directory), 'port': state['port'], 'recovered': recovered}
    except BaseException:
        _restore(state)
        raise


def _remove_created(created):
    # Deepest first. Anything inside a directory setup created appeared during
    # the transaction: Firefox adds an empty ~/.mozilla/extensions, for one.
    # Empty directories go; a directory holding any file stays, with the file.
    for directory in reversed(created):
        for parent, _, _ in os.walk(directory, topdown=False):
            try:
                os.rmdir(parent)
            except OSError:
                pass


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
    _restore_mime(state['backups'])
    # Exact identities only: our launch, any instance on our private profile
    # (a handler-started one), and Firefox processes started outside it during
    # this transaction, whether observation recorded them or not.
    targets = [[state['pid'], state.get('pid_start')]] if state.get('pid') else []
    profile = state['directory'].encode()
    targets += [[pid, start] for pid, start, argv in _firefox_mains() if profile in argv]
    foreign = state.get('foreign', [])
    foreign += [item for item in (_foreign(state) if scan else []) if item not in foreign]
    for pid, start in targets + foreign:
        _terminate(pid, start)
    for desktop in state['desktops']:
        Path(desktop).unlink(missing_ok=True)
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
    _remove_created(state.get('created', []))
    return {'restored': True, 'foreign': len(foreign)}


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


def _target(action, values):
    if action in ('prepare', 'recover'):
        if action == 'prepare':
            return _prepare(values)
        return {'recovered': _recover(_locations(values)[2], values.get('wait', 0))}
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
