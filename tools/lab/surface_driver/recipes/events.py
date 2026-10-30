"""OS events and updates. Never replace an OS event with an application signal.

Fixture contracts are intentionally explicit. The driver owns their lifecycle;
missing fixtures block coverage rather than silently exercising the happy path.
No live destination or package repository is used by the Boat lane.
"""
from __future__ import annotations

import json
import shlex
from urllib.parse import urlsplit

from tools.lab.surface_driver.api import Context, RecipeBlocked, recipe


# Unimplemented event controllers are declarations, not simulated coverage.
BLOCKED = {
    'event.incomplete-uploads-kept': 'Needs a core-owned cleanup-eligible multipart journal and automatic cleanup trigger: the current fresh failed upload is retained for Retry, and manual cache.clear intentionally does not emit the event toast.',
    'event.autostart-login': 'Needs a guest re-login controller, masked console login, and private XDG autostart propagation; executing start.sh is not login evidence.',
    'event.session-logout': 'Needs a guest logout/re-login controller retaining the old resident PID/start identity and saved console frames; ui.quit is not logout evidence.',
    'event.resident-retired': 'Needs a resident tied to an ended real logind session and a fresh login launcher; changing XDG_SESSION_ID does not create an ended session.',
    'event.keyring-unlock': 'Needs an isolated locked OS keyring and protected console unlock input; never lock or use a personal keyring.',
}


def _result(ctx: Context, method, params=None):
    reply = ctx.call(method, params)
    # Context.call implementations may return the wire envelope or its result.
    if 'ok' in reply:
        ctx.expect(reply['ok'] is True, method + ' succeeds')
        return reply['result']
    return reply


def _shell(ctx: Context, script, what, timeout=60):
    reply = ctx.shell(script, timeout=timeout)
    ctx.expect(reply['returncode'] == 0, what)
    return reply['stdout']


def _fixture(ctx: Context, name):
    try:
        value = ctx.fixture(name)
    except (KeyError, NotImplementedError):
        ctx.blocked('Driver fixture unavailable: ' + name, issue=682)
    if not isinstance(value, dict):
        ctx.blocked('Driver fixture needs a descriptor object: ' + name, issue=682)
    return value


def _guest(ctx: Context):
    if ctx.placement not in ('guest', 'boat'):
        ctx.blocked('Disruptive events run only in an isolated disposable guest')


def _notice(ctx: Context, text):
    return ctx.wait(lambda: next((n for n in ctx.notifications()
                                 if text.casefold() in (n.get('summary', '') + ' ' + n.get('body', '')).casefold()), None),
                    'notification: ' + text)


def _window_ready(ctx: Context, window):
    """Retry an acknowledged helper launch until its native window maps."""
    try:
        return ctx.geometry(window)
    except RecipeBlocked as error:
        if '0 matches' in str(error):
            return None
        raise


def _show_window(ctx: Context, view):
    """ui.show acknowledges a launch before the helper's native window maps."""
    ctx.view('ui.show', {'view': view})
    ctx.wait(lambda: _window_ready(ctx, view), view + ' helper maps before event frames', timeout=15)


def _failure_frame(ctx: Context):
    try:
        ctx.frame('failure-before-cleanup')
    except Exception as error:
        ctx.review('Failure frame unavailable: ' + type(error).__name__)


def _operation(ctx: Context, identity):
    result = _result(ctx, 'operation.inspect', {'id': identity})
    ctx.expect(result.get('id') == identity, 'inspect retains the exact operation ID')
    return result


def _upload(ctx: Context):
    """Fixture starts a real resident upload to a throttled synthetic destination.

    events.slow-upload: operationId, destination, address (literal IP), port,
    timeoutSeconds. Automatic link copying and notifications must be enabled.
    S3 on Boat must use a literal-loopback local fixture, never a cloud endpoint.
    The core retains its payload/fixture request receipts and cleans up that exact
    operation and synthetic object after the recipe.
    """
    _guest(ctx)
    f = _fixture(ctx, 'events.slow-upload')
    ctx.expect(f['destination'] == ctx.destination, 'fixture uses the selected destination')
    if ctx.host == 'boat' and (ctx.destination not in ('local', 's3') or f['address'] != '127.0.0.1'):
        ctx.blocked('Boat events use only the loopback S3 fixture')
    op = _operation(ctx, f['operationId'])
    ctx.expect(any(i.get('uploadStatus') == 'uploading' for i in op.get('items', [])),
               'real upload is still in flight before the OS event')
    ctx.note('Slow-upload operation: ' + f['operationId'])
    return f


@recipe('event.config-changed')
def config_changed(ctx: Context):
    """Exercise the file watcher, not settings.reload or settings.patch."""
    _guest(ctx)
    _show_window(ctx, 'settings')
    before = _result(ctx, 'settings.get')
    metadata = before['metadata']
    document = before['document']
    enabled = not document.get('preferences', {}).get('launchAtLogin', False)
    old_shortcut = document['actions']['clipboard']['shortcut']
    used = {a.get('shortcut') for a in document['actions'].values()}
    shortcut = next('Ctrl+Alt+Shift+F' + str(n) for n in range(12, 0, -1)
                    if 'Ctrl+Alt+Shift+F' + str(n) not in used)
    ctx.frame('before', window='settings')
    # Do not copy a complete user document through a shell command or log. The
    # guest script reads it in memory, checks the revision, and changes one bool.
    script = r'''python3 - <<'PYCONFIG'
import hashlib, json, os, pathlib, socket, stat, tempfile
p = pathlib.Path(PATH)
root = pathlib.Path(os.environ['XDG_CONFIG_HOME']).resolve()
assert p.resolve().is_relative_to(root) and not p.is_symlink()
s = p.stat()
assert stat.S_ISREG(s.st_mode) and s.st_uid == os.getuid() and not s.st_mode & 0o077
raw = p.read_bytes()
assert hashlib.sha256(raw).hexdigest() == REVISION
# Respect the resident's QLockFile: refuse rather than break an existing lock.
lock = pathlib.Path(str(p) + '.lock')
fd = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
try:
    os.write(fd, f'{os.getpid()}\npython3\n{socket.gethostname()}\n'.encode())
    assert hashlib.sha256(p.read_bytes()).hexdigest() == REVISION
    doc = json.loads(raw)
    doc.setdefault('preferences', {})['launchAtLogin'] = ENABLED
    doc['actions']['clipboard']['shortcut'] = __SHORTCUT__
    tmpfd, name = tempfile.mkstemp(prefix='.events-', dir=p.parent)
    try:
        with os.fdopen(tmpfd, 'w') as out:
            json.dump(doc, out)
            out.flush()
            os.fsync(out.fileno())
        os.replace(name, p)
    finally:
        if os.path.exists(name): os.unlink(name)
finally:
    os.close(fd)
    lock.unlink()
PYCONFIG'''
    script = script.replace('PATH', repr(metadata['path'])).replace('REVISION', repr(metadata['observedRevision'])).replace('ENABLED', repr(enabled)).replace('__SHORTCUT__', repr(shortcut))
    try:
        _shell(ctx, script, 'external atomic config edit in private profile')
        after = ctx.wait(lambda: (s if (s := _result(ctx, 'settings.get'))['document'].get('preferences', {}).get('launchAtLogin') == enabled else None),
                         'watcher accepts external edit')
        ctx.expect(after['metadata']['validation']['ok'], 'external document validates')
        ctx.expect(after['metadata']['acceptedRevision'] != metadata['acceptedRevision'], 'watcher accepted a new revision')
        ctx.wait(lambda: _result(ctx, 'ui.snapshot')['diagnostics']['loginStartup']['enabled'] == enabled,
                 'login startup integration reapplied')
        ctx.expect(after['document']['actions']['clipboard']['shortcut'] == shortcut, 'external shortcut preference accepted')
        ctx.wait(lambda: ((b := _result(ctx, 'ui.snapshot')['diagnostics']['shortcuts']['bindings']['clipboard']).get('requested') == shortcut
                          and b.get('bound') is True), 'native clipboard shortcut rebound')
        ctx.frame('after', window='settings')
    except Exception:
        _failure_frame(ctx)
        raise
    finally:
        current = _result(ctx, 'settings.get')
        _result(ctx, 'settings.patch', {'expectedRevision': current['metadata']['observedRevision'],
                                     'patch': {'preferences': {'launchAtLogin': document.get('preferences', {}).get('launchAtLogin', False)},
                                               'actions': {'clipboard': {'shortcut': old_shortcut}}}})


@recipe('event.single-instance')
def single_instance(ctx: Context):
    _guest(ctx)
    before = _result(ctx, 'ui.snapshot')
    _show_window(ctx, 'history')
    _shell(ctx, 'putgd-launcher --background', 'second installed launcher hands off', timeout=15)
    after = _result(ctx, 'ui.snapshot')
    ctx.expect(after['residentEpoch'] == before['residentEpoch'], 'second launch preserves the original resident epoch')
    ctx.expect(after['operationsReady'], 'original resident remains available')


@recipe('event.ui-helper-crash', desktop_drawn=True)
def ui_helper_crash(ctx: Context):
    _guest(ctx)
    _show_window(ctx, 'history')
    before = _result(ctx, 'ui.snapshot')
    pid = before['diagnostics']['ui']['pid']
    ctx.expect(isinstance(pid, int) and pid > 1, 'inspect identifies the live UI helper PID')
    ctx.note('UI helper crash target PID: ' + str(pid) + '; resident epoch: ' + before['residentEpoch'])
    ctx.frame('before', window='history')
    # pidfd eliminates PID reuse between validation and the fatal signal. Check
    # ownership, executable and private runtime before killing exactly one PID.
    script = '''python3 - <<'PYKILL'
import os, pathlib, signal
pid = PID
fd = os.pidfd_open(pid)
try:
    p = pathlib.Path('/proc') / str(pid)
    assert p.stat().st_uid == os.getuid()
    assert pathlib.Path(os.readlink(p / 'exe')).name == 'putgd-ui'
    env = dict(x.split(b'=', 1) for x in (p / 'environ').read_bytes().split(b'\\0') if b'=' in x)
    assert env[b'XDG_RUNTIME_DIR'].decode() == os.environ['XDG_RUNTIME_DIR']
    signal.pidfd_send_signal(fd, signal.SIGKILL)
finally:
    os.close(fd)
PYKILL'''.replace('PID', str(pid))
    _shell(ctx, script, 'kill the inspected UI helper only')
    try:
        ctx.wait(lambda: _result(ctx, 'ui.snapshot')['diagnostics']['ui']['pid'] == 0, 'resident observes helper death')
        ctx.expect(_result(ctx, 'ui.snapshot')['residentEpoch'] == before['residentEpoch'], 'resident survives helper crash')
        _notice(ctx, 'window')
        ctx.frame('after-crash')
    except Exception:
        _failure_frame(ctx)
        raise
    finally:
        _show_window(ctx, 'history')
    restarted = _result(ctx, 'ui.snapshot')['diagnostics']['ui']['pid']
    ctx.expect(restarted > 1 and restarted != pid, 'next view creates a new UI helper')
    ctx.frame('recovered', window='history')


@recipe('event.clipboard-changed', needs=('events.slow-upload',), destinations=('local', 's3', 'proton'), desktop_drawn=True)
def clipboard_changed(ctx: Context):
    f = _upload(ctx)
    _show_window(ctx, 'history')
    ctx.frame('before', window='history')
    clipboard = _fixture(ctx, 'events.clipboard')
    text = clipboard['text']
    ctx.wait(lambda: ctx.clipboard_text() == text, 'new clipboard text is owned by external tool')
    op = ctx.wait(lambda: (o if (o := _operation(ctx, f['operationId'])).get('copyStatus') == 'suppressed' else None),
                  'automatic link copy suppressed', timeout=f['timeoutSeconds'])
    ctx.expect(op['items'] and all(i['uploadStatus'] == 'uploaded' for i in op['items']), 'upload succeeds despite clipboard change')
    ctx.expect(ctx.clipboard_text() == text, 'automatic copy preserves the newer clipboard')
    _notice(ctx, 'clipboard changed')
    ctx.frame('after', window='history')


@recipe('event.network-stall', needs=('events.slow-upload',), destinations=('local', 's3', 'proton'), desktop_drawn=True, timeout=180)
def network_stall(ctx: Context):
    """Drop packets to precisely the fixture port; never change a global policy."""
    f = _upload(ctx)
    controllers = _fixture(ctx, 'events.controllers')
    _show_window(ctx, 'history')
    ctx.frame('before', window='history')
    # The core stages the narrow privileged rule and registers rollback before
    # mutation. Its independent finalizer also covers setup/deadline failures.
    table = controllers['network_stall'](f['address'], f['port'])
    try:
        # The tiny request may already be written when the event occurs. On
        # loopback, also drop this server's response packets in the same owned
        # table so a delayed successful response cannot escape the stall.
        _shell(ctx, f'sudo -n /usr/sbin/nft add rule inet {table} output ip saddr 127.0.0.1 tcp sport {f["port"]} counter drop',
               'stall only the loopback fixture response direction')
        op = ctx.wait(lambda: (o if (o := _operation(ctx, f['operationId'])).get('state') in ('failed', 'partial') else None),
                      'stalled upload reaches bounded failure', timeout=f['timeoutSeconds'])
        counters = json.loads(_shell(ctx, f'sudo -n /usr/sbin/nft -j list table inet {table}', 'read real packet-drop counters'))
        count = sum(expr['counter'].get('packets', 0) for row in counters['nftables']
                    for expr in row.get('rule', {}).get('expr', []) if 'counter' in expr)
        ctx.note('Endpoint packet-drop witness: ' + str(count) + ' packets')
        ctx.expect(count > 0, 'nft actually dropped upload packets')
        ctx.expect(any(i.get('uploadStatus') in ('failed', 'unknown') and i.get('error', {}).get('code') for i in op['items']), 'inspect exposes per-item upload error or retained unknown outcome')
        if any(i.get('uploadStatus') == 'unknown' for i in op['items']):
            ctx.note('Publication outcome remains unknown for operation ' + f['operationId'] + '; reconcile its retained identity before any retry')
        _notice(ctx, 'failed')
        ctx.frame('after', window='history')
        ctx.review('Inspect the saved tray frame for the attention latch; resident tray state does not expose the latch.')
    except Exception:
        _failure_frame(ctx)
        raise
    finally:
        # Core owns idempotent teardown, including on an expired recipe deadline.
        ctx.note('Owned nft table cleanup registered with events.controllers: ' + table)


def _feed(ctx: Context, fixture_name='events.update-feed'):
    """Resident launch must inherit this guest-only feed, before any check.

    Descriptor: url (loopback base), residentPid, version, channel, root.
    A guest-local HTTP server serves root; the fixture owns/cleans its PID and
    verifies request receipts. Tagged package bytes are required, never relabelled.
    """
    _guest(ctx)
    f = _fixture(ctx, fixture_name)
    url = urlsplit(f['url'])
    ctx.expect(url.scheme == 'http' and url.hostname == '127.0.0.1' and url.port is not None
               and not url.username and not url.password and not url.query and not url.fragment, 'only a guest loopback feed')
    pid = f['residentPid']
    ctx.expect(isinstance(pid, int) and pid > 1, 'fixture identifies resident PID')
    script = '''python3 - <<'PYFEED'
import os, pathlib
p = pathlib.Path('/proc') / str(PID)
assert p.stat().st_uid == os.getuid()
env = dict(x.split(b'=', 1) for x in (p / 'environ').read_bytes().split(b'\\0') if b'=' in x)
assert env[b'PUTGD_UPDATE_FEED_URL'].decode() == __FEED_URL__
assert env[b'XDG_RUNTIME_DIR'].decode() == os.environ['XDG_RUNTIME_DIR']
PYFEED'''.replace('PID', str(pid)).replace('__FEED_URL__', repr(f['url']))
    _shell(ctx, script, 'resident is pinned to local test feed before checking')
    status = _result(ctx, 'update.status')
    if not status['build']['supported']:
        ctx.blocked('Update flow needs tagged package bytes: ' + status['build'].get('reason', 'unsupported'), issue=682)
    ctx.expect(status['channel'] == f['channel'], 'fixture feed matches active channel')
    # Keep serving separate from this lane: the core owns server startup/teardown.
    feed = {'schema': 1, 'channel': f['channel'], 'release': {'version': f['version'], 'tag': 'v' + f['version'], 'assets': {}}}
    script = '''python3 - <<'PYWRITE'
import json, os, pathlib, tempfile
root = pathlib.Path(ROOT)
assert root.resolve().is_relative_to(pathlib.Path(os.environ['XDG_STATE_HOME']).resolve())
fd, name = tempfile.mkstemp(prefix='.feed-', dir=root)
try:
    with os.fdopen(fd, 'w') as out: json.dump(FEED, out)
    os.replace(name, root / CHANNEL)
finally:
    if os.path.exists(name): os.unlink(name)
PYWRITE'''.replace('ROOT', repr(f['root'])).replace('FEED', repr(feed)).replace('CHANNEL', repr(f['channel'] + '.json'))
    _shell(ctx, script, 'publish synthetic feed inside guest')
    return f


def _available(ctx: Context, f):
    status = _result(ctx, 'update.status')
    ctx.expect(status['result']['latest'] == f['version'] and status['result']['updateAvailable'], 'resident reports the fixture update')
    return status


@recipe('event.update-found', needs=('events.update-feed',), desktop_drawn=True)
def update_found(ctx: Context):
    f = _feed(ctx)
    _show_window(ctx, 'history')
    ctx.frame('before', window='history')
    _result(ctx, 'update.check')
    _available(ctx, f)
    ctx.view('ui.show', {'view': 'updates'})
    ctx.frame('after', window='settings')
    ctx.review('Manual check proves found/settings state. A fresh stable scheduled check is required for the toast and tray announcement.')


@recipe('shell.app-menu-check-updates', needs=('events.update-feed',))
def app_menu_check_updates(ctx: Context):
    f = _feed(ctx)
    _show_window(ctx, 'history')
    before = _result(ctx, 'update.status').get('attemptedAt')
    ctx.frame('before', window='history')
    ctx.click('applicationMenu', window='history')
    ctx.click('checkForUpdates', window='history')
    ctx.wait(lambda: _result(ctx, 'update.status').get('attemptedAt') != before, 'menu triggers a new check')
    ctx.wait(lambda: not _result(ctx, 'update.status')['checking'], 'menu check finishes', timeout=60)
    _available(ctx, f)
    _result(ctx, 'ui.viewState')
    ctx.frame('after', window='settings')


@recipe('shell.update-available-link', needs=('events.update-feed',))
def update_available_link(ctx: Context):
    f = _feed(ctx)
    _result(ctx, 'update.check')
    _available(ctx, f)
    _show_window(ctx, 'history')
    ctx.frame('before', window='history')
    ctx.click('updateAvailable', window='history')
    _result(ctx, 'ui.viewState')
    ctx.frame('after', window='settings')
    ctx.review('Saved frame must show Updates scrolled into view; ui.viewState has no scroll-position assertion.')


@recipe('cli.update', needs=('events.update-feed',))
def cli_update(ctx: Context):
    f = _feed(ctx)
    reply = ctx.cli('update.check')
    ctx.expect(reply['returncode'] == 0, 'CLI update.check succeeds against local feed')
    data = json.loads(reply['stdout'])
    ctx.expect(data['ok'] and data['result']['result']['latest'] == f['version'], 'CLI reports exact fixture version')
    _available(ctx, f)
    ctx.blocked('CLI update status/check covered; signed local AppImage install/restart and direct helper variants still need fixtures')


@recipe('mcp.updates', needs=('events.update-feed',))
def mcp_updates(ctx: Context):
    f = _feed(ctx)
    ctx.mcp('check_for_updates', {})
    _available(ctx, f)
    ctx.mcp('get_update_status', {})
    ctx.blocked('MCP status/check covered; install_update/restart_after_update/open_software_updates need signed local update and handler fixtures')


@recipe('notification.update-available', needs=('events.update-notification',), desktop_drawn=True, timeout=1000)
def update_notification(ctx: Context):
    _feed(ctx, 'events.update-notification')
    status = _result(ctx, 'update.status')
    ctx.expect(status['notifies'], 'fresh stable build is eligible for notification')
    ctx.frame('before')
    # No manual check: that marks a version seen and suppresses the daily toast.
    _notice_timeout = 960
    ctx.wait(lambda: any('update' in n.get('summary', '').casefold() for n in ctx.notifications()),
             'real scheduled update notification', timeout=_notice_timeout)
    ctx.frame('after')
    ctx.blocked('Scheduled notification observed; opening its release-notes action needs the notification-action/real browser handler fixture')


@recipe('settings.update-links', needs=('events.update-feed', 'events.browser-handler'), desktop_drawn=True)
def update_links(ctx: Context):
    ctx.blocked('Needs a real disposable browser plus launched-handler URL/PID witness; do not fetch/download a synthetic nonexistent release from GitHub')


@recipe('settings.open-software-updates', needs=('events.software-updates-handler',), desktop_drawn=True)
def software_updates(ctx: Context):
    ctx.blocked('Needs an isolated enabled lab package repository and a real software-centre launch witness; the guest must never use a live upgrade feed')


@recipe('confirm.switch-older', needs=('events.older-update-feed',), desktop_drawn=True)
def switch_older(ctx: Context):
    ctx.blocked('Needs signed disposable AppImage switch fixture and confirmation controls; no install against a real release')


@recipe('event.display-changed', needs=('events.controllers',), desktop_drawn=True)
def display_changed(ctx: Context):
    """Change real guest topology while the production region selector is open."""
    _guest(ctx)
    observation = ctx.profile.get('selector_accessible', {}).get('query')
    if not isinstance(observation, dict):
        ctx.blocked('Core selector observation profile is unavailable; needs the owned-focus/Shell hook from PR #767 (tracked by #682)', issue=682)
    controllers = _fixture(ctx, 'events.controllers')
    identity = _result(ctx, 'action.invoke', {'action': 'region'})['operationId']
    terminal = False
    try:
        ctx.wait(lambda: _operation(ctx, identity)['state'] == 'acquiring', 'region selection is acquiring')
        def selector():
            nodes = ctx.accessible(dict(observation, name='Capture region'))
            if len(nodes) > 1:
                ctx.blocked('Native selector observation is ambiguous', issue=682)
            return nodes[0] if nodes else None
        node = ctx.wait(selector, 'live native selector observation', timeout=10)
        rect = node.get('rect')
        if not isinstance(rect, (list, tuple)) or len(rect) != 4 or rect[2] <= 0 or rect[3] <= 0:
            ctx.blocked('Native selector observation has no usable display bounds', issue=682)
        ctx.frame('selector-before-display-change')
        # Pick a different size from the live selector, not a fixed assumed mode.
        size = '1280x720+0+0' if rect[2] != 1280 else '1600x900+0+0'
        changed = controllers['display_change'](size)
        ctx.expect(changed['ok'], 'QMP and native output readback verify topology mutation')
        op = ctx.wait(lambda: (o if (o := _operation(ctx, identity))['state'] in ('failed', 'cancelled') else None),
                      'display change terminates selection')
        terminal = True
        ctx.expect(op['error']['code'] == 'display_changed', 'selection reports the actual topology event')
        ctx.expect(not op.get('items'), 'display change does not publish a capture')
        ctx.frame('after-display-change')
    finally:
        if not terminal:
            _result(ctx, 'operation.cancel', {'id': identity})


@recipe('event.panel-theme-changed', desktop_drawn=True)
def panel_theme_changed(ctx: Context):
    _guest(ctx)
    spec = ctx.profile.get('events', {}).get('panelTheme')
    if not spec:
        ctx.blocked('Profile needs reversible native panelTheme commands and light/dark theme names')
    read, write = spec['read'], spec['write']
    ctx.expect(isinstance(read, list) and isinstance(write, list) and read and write, 'profile supplies theme argument vectors')
    share = _share_session_dconf(ctx, spec.get('sessionDconf'))
    try:
        _panel_theme_event(ctx, spec, read, write)
    finally:
        if share:
            _shell(ctx, share, 'unlink the session dconf database from the private profile')


def _share_session_dconf(ctx: Context, relative):
    """Let the resident read the session's dconf database; return the cleanup script.

    The resident reads the theme with its own gsettings, whose dconf user
    database is $XDG_CONFIG_HOME/dconf/user: in the private profile, a database
    nothing writes, while every writer (Themes settings, gsettings, the profile's
    write vector) goes through dconf-service into the session's (#702). Only that
    directory is shared, by a link inside the run's private configuration home.
    """
    if relative is None:
        return None
    parts = relative.split('/') if isinstance(relative, str) else []
    ctx.expect(parts and not relative.startswith('/') and all(p not in ('', '.', '..') for p in parts),
               'profile names the session dconf directory relative to the home directory')
    session = '"$HOME"/' + shlex.quote(relative)
    # Keep mutation, verification and failure rollback in one target script.
    _shell(ctx, 'set -eu\nlink="$XDG_CONFIG_HOME/dconf"\n'
                'test -d ' + session + '\n'
                'test ! -L ' + session + '\n'
                'test ! -e "$link"\n'
                'test ! -L "$link"\n'
                'created=0\n'
                'rollback() { if test "$created" = 1; then\n'
                ' if test -L "$link"; then\n'
                '  if test "$(readlink "$link")" = ' + session + '; then rm "$link"; fi\n'
                ' fi\nfi; }\n'
                'trap rollback EXIT\n'
                'ln -sT ' + session + ' "$link"\n'
                'created=1\n'
                'test "$(readlink "$link")" = ' + session + '\n'
                'test -d "$link"\n'
                'trap - EXIT\n',
           'share the session dconf database with the private profile')
    return ('set -eu\nlink="$XDG_CONFIG_HOME/dconf"\n'
            'if test -L "$link" && test "$(readlink "$link")" = ' + session + '; then rm "$link"; fi\n'
            'test ! -e "$link"\n'
            'test ! -L "$link"\n')


def _panel_theme_event(ctx: Context, spec, read, write):
    original = _shell(ctx, shlex.join(read), 'read existing native panel theme').strip()
    # gsettings prints a GVariant string; parse it as one shell word, then quote
    # it again. Do not execute arbitrary output from a desktop setting.
    values = shlex.split(original)
    ctx.expect(len(values) == 1, 'one native theme name')
    # A newly shared dconf database is observed by the resident's periodic
    # gsettings read. Do not choose the opposite tint from its stale baseline.
    if spec.get('sessionDconf'):
        expected = 'dark' if 'dark' in values[0].casefold() else 'light'
        before = ctx.wait(lambda: (t if (t := _result(ctx, 'ui.snapshot')['diagnostics']['trayTint'])['source'] == 'cinnamon-theme'
                                  and t['panel'] == expected else None),
                          'resident re-reads the shared native theme before baseline', timeout=15)
    else:
        before = _result(ctx, 'ui.snapshot')['diagnostics']['trayTint']
    target = spec['light'] if before['panel'] == 'dark' else spec['dark']
    ctx.note('Panel theme original: ' + values[0] + '; target: ' + target + '; resident tint: ' + json.dumps(before, sort_keys=True))
    _show_window(ctx, 'history')
    ctx.frame('before')
    try:
        _shell(ctx, shlex.join(write + [target]), 'change real desktop panel theme')
        changed = shlex.split(_shell(ctx, shlex.join(read), 'read actual native panel theme after event').strip())
        ctx.expect(changed == [target], 'native panel theme really changed')
        ctx.note('Native panel theme after event: ' + changed[0])
        after = ctx.wait(lambda: (t if (t := _result(ctx, 'ui.snapshot')['diagnostics']['trayTint'])['panel'] != before['panel'] else None),
                         'resident observes native panel tone change')
        ctx.expect(after['color'] != before['color'], 'resident recolours tray glyph')
        ctx.frame('after')
        ctx.review('Review the saved native panel glyph; opening the desktop tray menu still needs a profile input target.')
    except Exception:
        _failure_frame(ctx)
        raise
    finally:
        _shell(ctx, shlex.join(write + [values[0]]), 'restore original native panel theme')
        restored = shlex.split(_shell(ctx, shlex.join(read), 'verify native theme restoration').strip())
        ctx.expect(restored == values, 'native panel theme restored')
        ctx.note('Native panel theme restored: ' + restored[0])
        ctx.wait(lambda: _result(ctx, 'ui.snapshot')['diagnostics']['trayTint'] == before, 'tray tone restored')

def _fill_storage(ctx: Context, fixture_name):
    """Fill a core-owned, preflight-budgeted tmpfs; never the guest root.

    Descriptor: mountPath (private XDG_DATA_HOME subdirectory, an actual tmpfs
    mount used by ArtifactStore). The core mounts 1.5--2 GiB before starting the
    resident. Fill only until below its 1 GiB reserve, keeping journal headroom.
    The fixture must start with >192 MiB available above the reserve, so the
    response is caused by this real write rather than its initial capacity.
    """
    _guest(ctx)
    f = _fixture(ctx, fixture_name)
    ctx.expect(_result(ctx, 'settings.get')['effectiveLimits']['disk']['availableBytes'] > 3 * 64 * 1024 * 1024,
               'storage admission was possible before filling the filesystem')
    path = f['mountPath']
    # The marker and fill path are one recipe-owned file. Leave metadata space
    # for the resident journal; admission must fail on its normal disk reserve.
    script = r'''python3 - <<'PYFILL'
import json, os, pathlib
p = pathlib.Path(__MOUNT__)
root = pathlib.Path(os.environ['XDG_DATA_HOME']).resolve()
assert not p.is_symlink() and p.resolve().is_relative_to(root) and p.resolve() != root
assert p.stat().st_uid == os.getuid() and p.stat().st_mode & 0o077 == 0
# findmnt is read-only. Require a separate tmpfs mount of the exact path.
import subprocess
mount = json.loads(subprocess.check_output(['findmnt', '-J', '-M', str(p), '-o', 'TARGET,FSTYPE']))['filesystems']
assert len(mount) == 1 and mount[0]['target'] == str(p) and mount[0]['fstype'] == 'tmpfs'
v = os.statvfs(p)
assert 1536 * 1024**2 <= v.f_blocks * v.f_frsize <= 2 * 1024**3
fd = os.open(p / '.events-fill', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
try:
    written = 0
    block = b'\0' * (1024**2)
    while (v := os.statvfs(p)).f_bavail * v.f_frsize >= 1024**3:
        assert written < 1100 * 1024**2
        written += os.write(fd, block)
    os.fsync(fd)
    print(json.dumps({'writtenBytes': written, 'availableBytes': v.f_bavail * v.f_frsize}))
finally:
    os.close(fd)
PYFILL'''.replace('__MOUNT__', repr(path))
    # Cleanup also runs if filling/expectation fails. The fixture creates a new
    # mount per run and guarantees .events-fill did not exist before this call.
    cleanup = r'''python3 - <<'PYCLEAN'
import os, pathlib, stat
p = pathlib.Path(__MOUNT__) / '.events-fill'
if p.exists():
    s = p.lstat()
    assert stat.S_ISREG(s.st_mode) and s.st_uid == os.getuid() and s.st_mode & 0o077 == 0
    p.unlink()
PYCLEAN'''.replace('__MOUNT__', repr(path))
    return script, cleanup


def _space_error(ctx: Context, method, params):
    # call() raises on resident errors in the production driver. Keep the raw
    # CLI envelope for this intentionally failing action, including its code.
    response = ctx.cli(method, json.dumps(params))
    ctx.expect(response['returncode'] in (0, 1), 'resident returned an action response')
    reply = json.loads(response['stdout'])
    if reply.get('ok') is False:
        error = reply['error']
    else:
        result = reply['result'] if 'ok' in reply else reply
        identity = result.get('operationId', result.get('id'))
        ctx.expect(identity, 'accepted import keeps operation identity')
        ctx.note('Constrained-storage operation: ' + identity)
        op = ctx.wait(lambda: (o if (o := _operation(ctx, identity)).get('state') == 'failed' else None), 'import fails on constrained storage')
        error = op['error']
    ctx.expect(error.get('code') == 'disk_space_insufficient', 'production disk admission rejects the event')
    ctx.expect(error['details']['freeBytes'] < error['details']['reserveBytes'] + error['details']['requiredBytes'], 'disk error exposes measured shortfall')


@recipe('event.disk-full-import', needs=('events.import-storage',), desktop_drawn=True)
def disk_full_import(ctx: Context):
    # Import synthetic native clipboard text through production disk admission.
    script, cleanup = _fill_storage(ctx, 'events.import-storage')
    _fixture(ctx, 'events.clipboard')
    _show_window(ctx, 'history')
    ctx.frame('before', window='history')
    try:
        _shell(ctx, script, 'fill isolated artifact tmpfs below reserve')
        _space_error(ctx, 'action.invoke', {'action': 'clipboard'})
        _notice(ctx, 'space')
        ctx.frame('after', window='history')
        ctx.review('Saved tray frame must prove the attention latch; no resident latch inspector exists yet.')
    except Exception:
        _failure_frame(ctx)
        raise
    finally:
        _shell(ctx, cleanup, 'release only this recipe fill file')


@recipe('event.disk-full-record', needs=('events.record-storage',), desktop_drawn=True)
def disk_full_record(ctx: Context):
    script, cleanup = _fill_storage(ctx, 'events.record-storage')
    _show_window(ctx, 'history')
    ctx.frame('before', window='history')
    try:
        _shell(ctx, script, 'fill isolated recording tmpfs below reserve')
        _space_error(ctx, 'recording.prepare', {'target': {'kind': 'region', 'rect': [0, 0, 320, 240]},
                                              'format': 'mp4', 'fps': 15})
        _result(ctx, 'ui.snapshot')
        ctx.frame('after', window='history')
    except Exception:
        _failure_frame(ctx)
        raise
    finally:
        _shell(ctx, cleanup, 'release only this recipe fill file')


@recipe('event.recording-budget-stop', needs=('events.recording-source',), desktop_drawn=True, timeout=240)
def recording_budget_stop(ctx: Context):
    """The helper must reach a real byte budget; never issue recording.stop."""
    _guest(ctx)
    f = _fixture(ctx, 'events.recording-source')
    # The fixture displays a continuously changing synthetic scene in a native
    # external window and supplies a region entirely inside it. No personal
    # desktop or microphone content may be recorded.
    ctx.expect(f['synthetic'] is True, 'record only synthetic moving content')
    before = _result(ctx, 'settings.get')
    old_limit = before['document']['limits']['maxFileBytes']
    _result(ctx, 'settings.patch', {'expectedRevision': before['metadata']['observedRevision'],
                                 'patch': {'limits': {'maxFileBytes': 64 * 1024 * 1024}}})
    identity = None
    try:
        prepare = dict(f['prepare'])
        prepare['audio'] = {'system': False, 'microphone': False}
        prepared = _result(ctx, 'recording.prepare', prepare)
        identity = prepared['recordingId']
        ctx.note('Budget-stop recording operation: ' + identity)
        ctx.wait(lambda: _result(ctx, 'recording.inspect', {'id': identity})['state'] == 'ready', 'recording is ready', timeout=45)
        ctx.frame('before')
        _result(ctx, 'recording.start', {'id': identity})
        state = ctx.wait(lambda: (s if (s := _result(ctx, 'recording.inspect', {'id': identity}))['state'] == 'reviewing' else None),
                         'real capture reaches file budget and opens review', timeout=150)
        ctx.expect(state['stopReason'] == 'budget' and state['stopDetails']['reason'] == 'max_file_bytes', 'helper reports byte-budget stop rather than user stop')
        ctx.expect(state['durationUs'] > 0, 'real clip contains recorded time')
        _result(ctx, 'recording.review', {'id': identity, 'action': 'inspect'})
        op = _operation(ctx, identity)
        ctx.expect(not any(i.get('uploadStatus') in ('uploading', 'uploaded') for i in op['items']), 'private recording was not uploaded')
        _notice(ctx, 'stopped')
        ctx.frame('after', window='review')
    except Exception:
        _failure_frame(ctx)
        raise
    finally:
        try:
            if identity:
                _result(ctx, 'recording.cancel', {'id': identity})
        finally:
            current = _result(ctx, 'settings.get')
            _result(ctx, 'settings.patch', {'expectedRevision': current['metadata']['observedRevision'], 'patch': {'limits': {'maxFileBytes': old_limit}}})


def _declare_blocked(interaction, reason):
    def declaration(ctx: Context):
        ctx.blocked(reason, issue=682)
    declaration.__name__ = interaction.replace('event.', '').replace('-', '_')
    return recipe(interaction, desktop_drawn=interaction not in ('event.session-logout', 'event.resident-retired'))(declaration)


for _interaction, _reason in BLOCKED.items():
    globals()[_interaction.replace('event.', '').replace('-', '_')] = _declare_blocked(_interaction, _reason)
