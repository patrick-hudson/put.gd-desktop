"""Real default-Firefox verification, with target-side URL/token confinement.

Only stdlib is required on the target. Marionette is Firefox's shipped remote
protocol (firefox-source-docs.mozilla.org/remote/marionette/Protocol.html).
The temporary handler launches Firefox itself; it never records URL arguments.
Profile fragments may set ``browser.command`` (argv, e.g. snap run firefox),
``browser.profile_root`` (snap's ~/snap/firefox/common), and real
``browser.config_home``/``browser.data_home`` when they differ from ~/.config
and ~/.local/share. The portal's real defaults and Context.shell's isolated
XDG defaults are both guarded, covering portal and launcher fallback dispatch.
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


def _remote(ctx, action, values):
    # Source and arguments contain no share links. No raw browser responses leave
    # the target, even on a protocol error. Do not interpolate observed URLs here.
    payload = base64.b64encode(json.dumps(values).encode()).decode()
    script = 'python3 - ' + shlex.quote(action) + ' ' + shlex.quote(payload)
    result = ctx.shell(script + " <<'PUTGD_BROWSER_PY'\n" + Path(__file__).read_text()
                       + '\nPUTGD_BROWSER_PY', timeout=90)
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
    state = _remote(ctx, 'prepare', ctx.profile.get('browser', {}))
    _ACTIVE[id(ctx)] = state
    try:
        yield
    finally:
        try:
            restored = _remote(ctx, 'restore', state)
            ctx.expect(restored.get('restored') is True, 'browser MIME bytes and hashes restored')
            ctx.note('Firefox handler restored; MIME SHA-256 checks passed')
        finally:
            _ACTIVE.pop(id(ctx), None)


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
    state = _ACTIVE[id(ctx)]
    _remote(ctx, 'blank', state)
    frames = []
    try:
        trigger()
        result = ctx.wait(lambda: _remote(ctx, 'observe', {**state, 'expect': expect}),
                          'default Firefox loaded the expected page content', timeout=45)
        ctx.expect(result.get('matched'), 'real Firefox URL, title and page content match')
        frames.append(asdict(ctx.frame('browser-content', window=None)))
        receipt = {key: result[key] for key in ('url_host', 'title', 'matched')}
        receipt['frames'] = frames
        ctx.note(json.dumps({key: receipt[key] for key in ('url_host', 'title', 'matched')}))
        return receipt
    except BaseException:
        # Kiosk has no address bar. Blank the page before retaining failure
        # evidence, since error pages may echo a token-bearing request URL.
        safe = _remote(ctx, 'blank', state)
        if safe.get('safe'):
            frames.append(asdict(ctx.frame('browser-failure-safe-desktop', window=None)))
        else:
            ctx.review('Failure frame withheld: private blank kiosk did not own the foreground; '
                       'core needs a redacted full-desktop failure frame')
        raise


class _TargetError(RuntimeError):
    """Static, secret-free target error suitable for a receipt."""


class _Marionette:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.stream = self.sock.makefile('rb')
        self.sequence = 0
        try:
            hello = self.read()
            if hello.get('marionetteProtocol') != 3:
                raise _TargetError('Unsupported Firefox protocol')
            self.call('WebDriver:NewSession', {'capabilities': {}})
        except BaseException:
            self.stream.close()
            self.sock.close()
            raise

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
        self.sequence += 1
        data = json.dumps([0, self.sequence, method, params or {}]).encode()
        self.sock.sendall(str(len(data)).encode() + b':' + data)
        value = self.read()
        if value[:2] != [1, self.sequence] or value[2] is not None:
            # Firefox errors can contain URLs. Never include the response.
            raise _TargetError('Firefox protocol command failed')
        result = value[3]
        return result["value"] if isinstance(result, dict) and set(result) == {"value"} else result

    def script(self, source, args=()):
        value = self.call('WebDriver:ExecuteScript', {
            'script': source, 'args': list(args), 'newSandbox': True,
            'sandbox': 'default', 'line': 0, 'filename': 'surface-browser'})
        return value.get('value', value) if isinstance(value, dict) else value

    def close(self):
        try:
            self.call('WebDriver:DeleteSession')
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
const media = [...document.querySelectorAll('img,video,canvas,embed,object')].some(el =>
  visible(el) && (el.tagName==='IMG' ? el.complete && el.naturalWidth>0 :
  el.tagName==='VIDEO' ? el.readyState>=2 :
  el.tagName==='CANVAS' ? el.width>0 && el.height>0 :
  /pdf/.test(el.type || '')));
const download = [...document.querySelectorAll('button,a,[role="button"]')].some(el =>
  visible(el) && !el.disabled && /download/i.test(el.innerText || el.getAttribute('aria-label') || ''));
const status = performance.getEntriesByType('navigation')[0]?.responseStatus || 0;
const error = /^(404|403|500|AccessDenied|NoSuchKey|Error response)/i.test(text.trim()) ||
              /Problem loading page|Server Not Found|Error response|Access Denied/i.test(title);
const content = e.kind==='proton' ? text.includes(e.name) && (media || download) :
                (media || text.trim().length>0);
const hostOK = e.url_host ? u.hostname===e.url_host :
               e.kind==='proton' ? /(^|\.)proton\.(me|ch)$/.test(u.hostname) : true;
const matched = ['http:','https:'].includes(u.protocol) && hostOK && named &&
  title.length>0 && document.hasFocus() && innerWidth>=screen.width && innerHeight>=screen.height &&
  document.readyState==='complete' && status<400 && !error &&
  content && (!e.preview || media || (e.kind==='proton' ? download : text.trim().length>0)) &&
  (!e.text || text.includes(e.text));
// Keep only a title we can prove contains no URL path, fragment, or arbitrary
// page-supplied token: the filename, when it is present in the actual title.
return {matched, url_host: matched ? u.hostname : '',
  title: matched ? (title.includes(e.name) ? e.name : '[page title verified; redacted]') : ''};
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


def _prepare(options):
    home = Path(pwd.getpwuid(os.getuid()).pw_dir)
    expand = lambda value: Path(value.replace('~', str(home), 1)) if value.startswith('~/') else Path(value)
    config = expand(options.get('config_home', str(home / '.config')))
    data = expand(options.get('data_home', str(home / '.local/share')))
    root = expand(options.get('profile_root', str(home / '.mozilla/firefox')))
    # Portal dispatch uses the real profile; gio/xdg-open fallback uses the
    # app's private XDG profile. Guard both, with independent byte backups.
    private = (Path(os.environ.get('XDG_CONFIG_HOME') or config),
               Path(os.environ.get('XDG_DATA_HOME') or data))
    profiles = list(dict.fromkeys([(config, data), private]))
    for directory in {root, *(c for c, d in profiles), *(d / 'applications' for c, d in profiles)}:
        directory.mkdir(parents=True, exist_ok=True)
    command = options.get('command', ['firefox'])
    if not isinstance(command, list) or not command or not all(isinstance(x, str) for x in command):
        raise ValueError('browser.command must be argv')
    backups = _snapshot(sorted({path for c, d in profiles for path in _mime_paths(c, d)}))
    directory = Path(tempfile.mkdtemp(prefix='putgd-surface-', dir=root))
    os.chmod(directory, 0o700)
    token = directory.name.replace('-', '')
    desktops = list(dict.fromkeys(d / 'applications' / (token + '.desktop') for c, d in profiles))
    desktop = desktops[0]
    state = {'directory': str(directory), 'desktops': [str(p) for p in desktops],
             'backups': backups, 'pid': None}
    try:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            state['port'] = probe.getsockname()[1]
        preferences = {'marionette.port': state['port'], 'browser.shell.checkDefaultBrowser': False,
                       'browser.startup.homepage_override.mstone': 'ignore',
                       'browser.startup.page': 0, 'browser.aboutwelcome.enabled': False,
                       'browser.newtabpage.enabled': False, 'datareporting.policy.dataSubmissionEnabled': False,
                       'browser.sessionstore.resume_from_crash': False,
                       'signon.rememberSignons': False, 'browser.privatebrowsing.autostart': True}
        (directory / 'user.js').write_text(''.join('user_pref(%s, %s);\n' % (json.dumps(k), json.dumps(v))
                                                  for k, v in preferences.items()))
        env = _environment(config, data)
        env['MOZ_APP_REMOTINGNAME'] = token
        # Both the initial instance and the handler use a unique remoting
        # name. Page observation still fails closed if dispatch reaches a
        # different instance (the bounded guest trials caught this case).
        argv = command + ['--profile', str(directory), '--kiosk']
        desktop_argv = ['MOZ_APP_REMOTINGNAME=' + token, 'XDG_CONFIG_HOME=' + str(config),
                        'XDG_DATA_HOME=' + str(data)] + argv + ['%u']
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
        state['pid_start'] = Path('/proc/%d/stat' % process.pid).read_text().split()[21]
        (directory / 'transaction.json').write_text(json.dumps(state))
        deadline = time.monotonic() + 35
        while True:
            try:
                client = _Marionette(state['port'])
                client.close()
                break
            except (OSError, RuntimeError):
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise _TargetError('Private Firefox did not become ready') from None
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
        return {'directory': str(directory), 'port': state['port']}
    except BaseException:
        _restore(state)
        raise


def _restore(state):
    directory = Path(state['directory'])
    # A caller passes only a directory identifier, not mutable backup bytes.
    stored = directory / 'transaction.json'
    if stored.exists():
        state = json.loads(stored.read_text())
    _restore_mime(state['backups'])
    pid = state.get('pid')
    if pid:
        process_stat = Path('/proc/%d/stat' % pid)
        if process_stat.exists() and process_stat.read_text().split()[21] == state.get('pid_start'):
            # Only our exact process, never a name/substring match.
            os.kill(pid, 15)
            deadline = time.monotonic() + 8
            while process_stat.exists() and time.monotonic() < deadline:
                time.sleep(.1)
            if process_stat.exists() and process_stat.read_text().split()[21] == state.get('pid_start'):
                os.kill(pid, 9)
    for desktop in state['desktops']:
        Path(desktop).unlink(missing_ok=True)
    shutil.rmtree(directory)
    return {'restored': True}


def _target(action, values):
    if action == 'prepare':
        return _prepare(values)
    if action == 'restore':
        return _restore(values)
    client = _Marionette(values['port'])
    try:
        if action == 'blank':
            # Reuse the current tab; all tabs are examined during observation.
            for handle in client.call('WebDriver:GetWindowHandles'):
                client.call('WebDriver:SwitchToWindow', {'handle': handle})
                client.call('WebDriver:Navigate', {'url': 'about:blank'})
            safe = client.script("return location.href==='about:blank' && document.hasFocus() && "
                                 "innerWidth>=screen.width && innerHeight>=screen.height;")
            return {'blank': True, 'safe': safe is True}
        if action == 'observe':
            for handle in client.call('WebDriver:GetWindowHandles'):
                try:
                    client.call('WebDriver:SwitchToWindow', {'handle': handle})
                    result = client.script(_OBSERVE, [values['expect']])
                except _TargetError:
                    continue  # A closed/navigating tab must not hide a later match.
                if isinstance(result, dict) and result.get('matched'):
                    return result
            return {}  # falsy until ctx.wait's deadline
        raise ValueError('Unknown browser helper action')
    finally:
        client.close()


if __name__ == '__main__':
    import sys
    try:
        answer = _target(sys.argv[1], json.loads(base64.b64decode(sys.argv[2])))
    except BaseException as error:
        answer = {'error': str(error) if isinstance(error, _TargetError) else type(error).__name__}
        # Only our static allowlisted errors, never protocol/OS text or traces.
    print(json.dumps(answer))
