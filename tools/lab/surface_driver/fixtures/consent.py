"""Guarded real-input ScreenCast consent for the private resident's own request (#791).

Profiles declare each desktop's consent behaviour under recording.consent:
``none`` (X11 capture reads the X server; no portal dialog may appear) or
``portal-dialog`` (the desktop's portal backend draws a dialog for every new
ScreenCast Start until a restore token is accepted). Nothing here pre-grants
through the permission store or replaces capture: the real pointer clicks the
backend's accept button once, after the compositor witness proves the point.

Ownership proof, all from this fixture's own dbus-monitor filter:
- exactly one impl ScreenCast/RemoteDesktop Start is pending on the bus;
- its request handle is the one xdg-desktop-portal derived from a public
  ScreenCast.Start sent by a private-family process with a putgd_record_ token;
- the impl call's destination process is a declared backend executable;
- the dialog window (or shell node) belongs to that backend process and was
  not present before recording.prepare.
"""
import fnmatch
import json
import shlex
import time
import uuid
from pathlib import Path

from ..api import RecipeBlocked, RecipeFailed
from .base import Fixture

ISSUE = 791
TOKEN_PREFIX = 'putgd_record_'
DIALOG_MEMBERS = {('ScreenCast', 'Start'), ('RemoteDesktop', 'Start')}


def policy(profile):
    """The profile's declared consent behaviour; X11 needs no declaration."""
    consent = (profile.get('recording') or {}).get('consent')
    if consent is None:
        if profile.get('session') == 'x11':
            return {'kind': 'none', 'reason': 'X11 capture reads the X server'}
        return None
    if consent.get('kind') not in ('none', 'portal-dialog'):
        raise RecipeBlocked('profile recording.consent.kind must be none or portal-dialog', issue=ISSUE)
    return consent


def records(text):
    rows = []
    for line in text.splitlines():
        try:
            row = json.loads(line)
        except ValueError:
            continue
        if isinstance(row, dict) and isinstance(row.get('kind'), str):
            rows.append(row)
    return rows


def analyse(rows):
    """Pure correlation of the filtered portal log (testable without a bus)."""
    ours = [r for r in rows if r['kind'] == 'public' and r.get('member') == 'Start' and
            isinstance(r.get('token'), str) and r['token'].startswith(TOKEN_PREFIX) and r.get('handle')]
    dialogs = [r for r in rows if r['kind'] == 'impl' and (r.get('interface'), r.get('member')) in DIALOG_MEMBERS and r.get('handle')]
    frontends = {r['sender'] for r in rows if r['kind'] == 'impl'}
    closed, responses = set(), {}
    for row in rows:
        if row['kind'] == 'close' and row.get('sender') in frontends:
            closed.add(row.get('handle'))
        # A Response counts only from the frontend that forwarded impl calls.
        if row['kind'] == 'response' and row.get('sender') in frontends:
            closed.add(row.get('handle'))
            responses[row.get('handle')] = row.get('code')
    handles = {r['handle'] for r in ours}
    pending = [r for r in dialogs if r['handle'] not in closed]
    return {'ours': ours, 'handles': handles, 'dialogs': dialogs, 'pending': pending,
            'responses': {h: c for h, c in responses.items() if h in handles},
            'foreign': [r for r in pending if r['handle'] not in handles]}


def decision(state, consent):
    """What the fixture may do now: wait, answered, dialog, or a blocked reason."""
    if state['responses']:
        return 'answered', state['responses']
    if state['foreign']:
        apps = sorted({r.get('app_id') or 'unknown app' for r in state['foreign']})
        return 'blocked', 'a ScreenCast/RemoteDesktop dialog for a different request is pending (' + ', '.join(apps) + ')'
    if len(state['pending']) > 1:
        return 'blocked', 'more than one ScreenCast dialog request is pending for this resident'
    if state['pending']:
        if consent is None:
            return 'blocked', 'profile declares no recording.consent behaviour for a pending ScreenCast dialog'
        if consent['kind'] == 'none':
            return 'blocked', 'profile declares no ScreenCast consent, but the portal opened a dialog request'
        request = state['pending'][0]
        backends = set(consent.get('backends', [])) | set(consent.get('shell_backends', []))
        if request.get('backend_exe') not in backends or not request.get('backend_pid'):
            return 'blocked', ('ScreenCast dialog request went to ' + repr(request.get('backend_exe')) +
                               ', not a declared portal backend')
        return 'dialog', request
    return 'wait', None


class Watch:
    """Started before recording.prepare; observes only, never inputs."""
    def __init__(self, scope, consent, log, baseline, offset):
        self.scope, self.ctx, self.consent = scope, scope.ctx, consent
        self.log, self.baseline, self.offset = log, baseline, offset
        self.clicked, self.dialog = False, None

    def rows(self):
        result = self.ctx.shell('head -c 1048577 ' + shlex.quote(self.log))
        if result['returncode']:
            raise RecipeBlocked('ScreenCast consent monitor log is unavailable', issue=ISSUE)
        if len(result['stdout']) > 1024 * 1024:
            raise RecipeBlocked('ScreenCast consent monitor log exceeded 1 MiB', issue=ISSUE)
        return records(result['stdout'])[self.offset:]

    def state(self):
        return analyse(self.rows())


def compositor_windows(ctx):
    """The compositor witness's window list, or None when the profile has none."""
    compositor = ctx.profile.get('compositor')
    if ctx.profile.get('session') != 'wayland' or not compositor:
        return None
    return ctx._probe('compositor_windows', {'compositor': compositor})


def start(scope):
    """Attach the consent monitor (once per scope) and record a window baseline."""
    ctx = scope.ctx
    consent = policy(ctx.profile)
    monitor = scope.values.get('_portal_monitor')
    if monitor is None:
        source = (Path(__file__).resolve().parents[1] / 'probe.py').read_text()
        program, log = scope.root + '/portal-monitor.py', scope.root + '/portal-events.jsonl'
        scope.write(program, source.encode())
        request = json.dumps({'probe': 'portal_monitor', 'query': {'log': log}})
        scope.launch(['python3', program, request], name='portal-monitor')
        # A marker signal from this private family proves dbus-monitor is attached.
        token = uuid.uuid4().hex
        def marked():
            ctx.shell('gdbus emit --session --object-path /org/putgd/LabPortalMonitor --signal '
                      'org.putgd.LabPortalMonitor.Marker ' + shlex.quote(token))
            text = ctx.shell('head -c 1048577 ' + shlex.quote(log))['stdout']
            return any(r['kind'] == 'marker' and r.get('token') == token for r in records(text))
        try:
            ctx.wait(marked, 'ScreenCast consent monitor attached', timeout=10, interval=.5)
        except RecipeFailed as error:
            raise RecipeBlocked('ScreenCast consent monitor could not attach to the session bus (dbus-monitor)', issue=ISSUE) from error
        monitor = scope.values['_portal_monitor'] = {'log': log}
    text = ctx.shell('head -c 1048577 ' + shlex.quote(monitor['log']))['stdout']
    baseline = None
    if consent and consent['kind'] == 'portal-dialog' and ctx.profile.get('compositor'):
        baseline = {w['id'] for w in compositor_windows(ctx)['windows']}
    return Watch(scope, consent, monitor['log'], baseline, len(records(text)))


def _dialog_window(ctx, watch, request):
    dialog = (watch.consent.get('dialog') or {})
    title = dialog.get('title', '*')
    found = [w for w in compositor_windows(ctx)['windows']
             if w.get('pid') == request['backend_pid'] and not w.get('minimized') and
             fnmatch.fnmatchcase(w.get('title') or '', title) and
             (watch.baseline is None or w['id'] not in watch.baseline)]
    if len(found) != 1:
        return None, ('ScreenCast consent dialog from the request\'s backend (pid %s, title %r) is missing or ambiguous '
                      '(%d new windows)' % (request['backend_pid'], title, len(found)))
    return found[0], None


def _accept_node(ctx, watch, request, window):
    accept = watch.consent.get('accept') or {}
    query = {'pid': request['backend_pid'], 'role': accept.get('role', 'push button'),
             'name': accept.get('name', 'Share'), 'visible': True, 'showing': True}
    nodes = [n for n in ctx.accessible(query) if n.get('rect') and min(n['rect'][2:]) > 0 and
             (window is None or n.get('window_id') == window['id'])]
    if len(nodes) != 1:
        return None, 'ScreenCast consent accept button %r is not uniquely drawn in the backend dialog (%d)' % (query['name'], len(nodes))
    node = nodes[0]
    if not node.get('enabled'):
        return None, 'ScreenCast consent accept button is disabled (no source selected)'
    prompt = (watch.consent.get('dialog') or {}).get('prompt')
    if prompt:
        labels = [n for n in ctx.accessible({'pid': request['backend_pid'], 'name': prompt, 'visible': True, 'showing': True})
                  if window is None or n.get('window_id') == window['id']]
        if not labels:
            return None, 'ScreenCast consent dialog does not show the declared prompt ' + repr(prompt)
    return node, None


def drive(watch, request):
    """One guarded real click on the backend dialog's accept button."""
    ctx, consent = watch.ctx, watch.consent
    if ctx.profile.get('session') != 'wayland':
        raise RecipeBlocked('portal-dialog consent is declared only for Wayland profiles', issue=ISSUE)
    if not ctx.profile.get('at_spi', {}).get('available'):
        raise RecipeBlocked('ScreenCast consent needs AT-SPI: ' + ctx.profile.get('at_spi', {}).get('reason', 'unavailable'), issue=ISSUE)
    shell = request.get('backend_exe') in consent.get('shell_backends', [])
    if not shell and not ctx.profile.get('compositor'):
        raise RecipeBlocked('ScreenCast consent needs the compositor witness; profile ' +
                            repr(ctx.profile.get('desktop')) + ' declares none', issue=ISSUE)
    window = None
    if not shell:
        window, reason = _dialog_window(ctx, watch, request)
        if reason:
            return reason
    node, reason = _accept_node(ctx, watch, request, window)
    if reason:
        return reason
    ctx.frame('screencast-consent-before')
    if shell:
        # Shell-drawn dialog: the shell stage pick proves the node is on top.
        ctx._accessible_pointer(None, 'left', node=node)
    else:
        x, y, w, h = node['rect']
        point = (x + w / 2, y + h / 2)
        fx, fy, fw, fh = window['frame']
        if not (fx <= point[0] < fx + fw and fy <= point[1] < fy + fh):
            return 'ScreenCast consent accept button lies outside its dialog frame'
        geometry = {'x': fx, 'y': fy, 'width': fw, 'height': fh, 'pid': request['backend_pid'],
                    'window_id': window['id'], 'coordinate_space': 'logical'}
        # The compositor's own pick at the point must be the dialog window.
        ctx._guard(window='screencast-consent', point=point, geometry=geometry)
        mapped = ctx._map_geometry({'x': point[0], 'y': point[1], 'coordinate_space': 'logical'})
        scale = mapped.get('scale', 1)
        px, py = (round((point[i] - mapped.get(('screen_x', 'screen_y')[i], 0)) * scale) for i in (0, 1))
        ctx._pointer_click(px, py, 'left', mapped['head'], guarded=True)
    watch.clicked, watch.dialog = True, {'backend': request.get('backend_exe'), 'window': window and window['id']}
    ctx.note('screencast consent: clicked ' + repr((consent.get('accept') or {}).get('name', 'Share')) +
             ' in ' + str(request.get('backend_exe')) + ' for this resident\'s request')
    time.sleep(.5)
    ctx.frame('screencast-consent-after')
    return None


def await_ready(scope, identity, watch, *, timeout=40):
    """Wait for ready; drive a proven consent dialog once; never a bare timeout."""
    ctx = scope.ctx
    deadline = time.monotonic() + timeout
    last = 'no ScreenCast portal request observed'
    while True:
        state = ctx.call('recording.inspect', {'id': identity})
        if state.get('state') == 'ready':
            if watch.clicked:
                answered = watch.state()['responses']
                if not answered or any(code != 0 for code in answered.values()):
                    raise RecipeBlocked('recording ready without the portal accepting this request\'s consent', issue=ISSUE)
            return state
        if state.get('state') in ('failed', 'cancelled'):
            code = (state.get('error') or {}).get('code') or state['state']
            raise RecipeFailed('recording prepare ended ' + state['state'] + ' (' + code + ')' +
                               (' after ScreenCast consent' if watch.clicked else ''))
        kind, value = decision(watch.state(), watch.consent)
        if kind == 'blocked':
            raise RecipeBlocked('ScreenCast consent: ' + value, issue=ISSUE)
        if kind == 'answered':
            codes = set(value.values())
            if codes != {0}:
                raise RecipeBlocked('ScreenCast portal answered this request with response ' +
                                    ','.join(str(c) for c in sorted(codes, key=str)), issue=ISSUE)
            last = 'portal accepted this request; waiting for the resident'
        elif kind == 'dialog':
            if watch.clicked:
                last = 'accept was clicked once but the backend dialog stayed pending'
            else:
                reason = drive(watch, value)
                last = reason or 'accept clicked; waiting for the portal response'
        elif watch.state()['ours']:
            last = 'this resident\'s ScreenCast Start was sent; no backend dialog request observed yet'
        if time.monotonic() >= deadline:
            raise RecipeBlocked('ScreenCast consent/ready not reached in %ds: %s' % (timeout, last), issue=ISSUE)
        time.sleep(.3)


__all__ = ['Fixture', 'analyse', 'await_ready', 'decision', 'drive', 'policy', 'records', 'start']
