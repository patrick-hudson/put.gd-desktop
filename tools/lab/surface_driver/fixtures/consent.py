"""Guarded real-input ScreenCast consent for the private resident's own request (#791).

Profiles declare each desktop's consent behaviour under recording.consent:
``none`` (X11 capture reads the X server; no portal dialog may appear) or
``portal-dialog`` (the desktop's portal backend draws a dialog for every new
ScreenCast Start until a restore token is accepted). Nothing here pre-grants
through the permission store or replaces capture: the real pointer clicks the
backend's accept button once, after the compositor witness proves the point.

Ownership proof, from this fixture's own dbus-monitor filter and the compositor:
- across the whole monitor log, exactly one impl ScreenCast/RemoteDesktop
  Start is pending on the bus, and it is this capture's request: the handle
  xdg-desktop-portal derived from a public ScreenCast.Start sent by a
  private-family process with a putgd_record_ token;
- the impl call's destination process is a declared backend executable, and
  that backend had no window open before recording.prepare;
- exactly one window of the backend process is new since prepare, whatever its
  title; it carries the declared title and its prompt names the application
  xdg-desktop-portal resolved for this request (the impl Start's app_id);
- the accept button is stable, sensitive and showing, the compositor's pick at
  its centre is that window, and a fresh read of the bus and window list taken
  after that pick still shows only this request and this window.

xdg-desktop-portal-gnome passes nothing else that links a dialog to a request:
put.gd sends no parent window (so no transient-for), and the GTK4 dialog's
accessibility tree holds no request handle. The app name in the prompt is the
only per-request text it draws.

The click is made at most once per request and never on a physical desktop:
the dialog's ticked "Remember this selection" stores a ScreenCast grant in the
desktop's permission store, which is acceptable only in a disposable overlay.
"""
import fnmatch
import glob
import json
import re
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


def analyse(rows, offset=0):
    """Pure correlation of the filtered portal log (testable without a bus).

    Pending and foreign requests are judged over the whole log; only this
    capture's own Start calls (rows from ``offset``) count as ours.
    """
    ours = [r for r in rows[offset:] if r['kind'] == 'public' and r.get('member') == 'Start' and
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
        self.clicked, self.dialog, self.refusals = False, None, []
        self.stable = None

    def rows(self):
        result = self.ctx.shell('head -c 1048577 ' + shlex.quote(self.log))
        if result['returncode']:
            raise RecipeBlocked('ScreenCast consent monitor log is unavailable', issue=ISSUE)
        if len(result['stdout']) > 1024 * 1024:
            raise RecipeBlocked('ScreenCast consent monitor log exceeded 1 MiB', issue=ISSUE)
        return records(result['stdout'])

    def state(self):
        return analyse(self.rows(), self.offset)


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
        # The monitor exits with the run's probe channel (#817 review): a dropped
        # guest connection must not leave it observing the session bus.
        request = json.dumps({'probe': 'portal_monitor', 'query': {'log': log, 'parent': getattr(ctx, '_channel_identity', lambda: None)()}})
        scope.launch(['python3', program, request], name='portal-monitor')
        # A marker signal from this private family proves dbus-monitor is attached.
        token = uuid.uuid4().hex
        def marked():
            ctx.shell('gdbus emit --session --object-path /org/putgd/LabPortalMonitor --signal '
                      'org.putgd.LabPortalMonitor.Marker ' + shlex.quote(repr(token)))
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
        windows = compositor_windows(ctx)['windows']
        baseline = {w['id'] for w in windows}
        # A dialog the backend drew before this monitor could see its request
        # is never ours: refuse rather than guess which window belongs to whom.
        backend_idle(backend_pid(ctx, consent), windows)
    return Watch(scope, consent, monitor['log'], baseline, len(records(text)))


def backend_idle(pid, windows):
    """Refuse when the portal backend already shows a window before prepare."""
    if pid and any(w.get('pid') == pid and not w.get('minimized') for w in windows):
        raise RecipeBlocked('ScreenCast consent: the portal backend already shows a window before prepare', issue=ISSUE)


def backend_pid(ctx, consent):
    """PID owning the declared impl portal bus name, or None when not running."""
    name = consent.get('backend_bus')
    if not name:
        return None
    reply = ctx.shell('gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus '
                      '--method org.freedesktop.DBus.GetConnectionUnixProcessID ' + shlex.quote(name))
    found = re.search(r'uint32 (\d+)', reply['stdout']) if reply['returncode'] == 0 else None
    return int(found[1]) if found else None


def app_name(ctx, app_id):
    """The Name= an application's desktop entry gives xdg-desktop-portal for its app_id."""
    if not app_id or not re.fullmatch(r'[A-Za-z0-9._-]{1,255}', app_id):
        return None
    code = """import configparser,os,sys
from pathlib import Path
dirs=[os.environ.get('XDG_DATA_HOME') or str(Path.home()/'.local/share')]+(os.environ.get('XDG_DATA_DIRS') or '/usr/local/share:/usr/share').split(':')
for d in dirs:
 p=Path(d)/'applications'/(APP+'.desktop')
 if p.is_file():
  c=configparser.RawConfigParser(strict=False);c.read(p,encoding='utf-8')
  print(c.get('Desktop Entry','Name',fallback=''));break
""".replace('APP', repr(app_id))
    reply = ctx.shell("python3 - <<'SD_APPNAME'\n" + code + 'SD_APPNAME\n')
    name = reply['stdout'].strip() if reply['returncode'] == 0 else ''
    return name or None


def _new_backend_windows(ctx, watch, request):
    return [w for w in compositor_windows(ctx)['windows']
            if w.get('pid') == request['backend_pid'] and not w.get('minimized') and
            (watch.baseline is None or w['id'] not in watch.baseline)]


def _dialog_window(ctx, watch, request):
    title = (watch.consent.get('dialog') or {}).get('title', '*')
    # Any second new backend window, whatever its title, may be another
    # request's dialog: one per request is the only unambiguous state.
    found = _new_backend_windows(ctx, watch, request)
    if len(found) != 1:
        return None, ('ScreenCast consent needs exactly one new window from the request\'s backend (pid %s); found %d'
                      % (request['backend_pid'], len(found)))
    if not fnmatch.fnmatchcase(found[0].get('title') or '', title):
        return None, 'the backend\'s new window is not titled %r' % title
    return found[0], None


def _accept_node(ctx, watch, request, window):
    accept = watch.consent.get('accept') or {}
    query = {'pid': request['backend_pid'], 'role': accept.get('role', 'push button'),
             'name': accept.get('name', 'Share'), 'visible': True, 'showing': True}
    if accept.get('coordinates'):
        query['coordinates'] = accept['coordinates']
    nodes = [n for n in ctx.accessible(query) if n.get('rect') and min(n['rect'][2:]) > 0 and
             (window is None or n.get('window_id') == window['id'])]
    if len(nodes) != 1:
        return None, 'ScreenCast consent accept button %r is not uniquely drawn in the backend dialog (%d)' % (query['name'], len(nodes))
    node = nodes[0]
    # GTK4 exposes sensitivity, Qt enabled: either is the toolkit's "can activate".
    if not (node.get('enabled') or node.get('states', {}).get('sensitive')):
        return None, 'ScreenCast consent accept button is disabled (no source selected)'
    # A declared placement error must keep the centre inside the button.
    tolerance = accept.get('placement_tolerance', 0)
    if min(node['rect'][2:]) <= 2 * tolerance:
        return None, ('ScreenCast consent accept button (%dx%d) is too small for the declared %d px placement tolerance'
                      % (node['rect'][2], node['rect'][3], tolerance))
    prompt = (watch.consent.get('dialog') or {}).get('prompt')
    if prompt:
        # Tie the drawn dialog to this request: its prompt must name the
        # application xdg-desktop-portal resolved for our impl Start.
        if '{app}' in prompt:
            name = app_name(ctx, request.get('app_id'))
            if not name:
                return None, ('this request\'s app id %r has no desktop entry name, so its dialog cannot be told '
                              'from another request\'s' % (request.get('app_id') or ''))
            prompt = prompt.replace('{app}', glob.escape(name))
        labels = [n for n in ctx.accessible({'pid': request['backend_pid'], 'name': prompt, 'visible': True, 'showing': True,
                                             **({'coordinates': accept['coordinates']} if accept.get('coordinates') else {})})
                  if window is None or n.get('window_id') == window['id']]
        if not labels:
            return None, 'ScreenCast consent dialog does not show the declared prompt ' + repr(prompt)
    return node, None


def _unchanged(watch, request, window):
    """Fresh bus and window read: still only this request and this dialog."""
    kind, value = decision(watch.state(), watch.consent)
    if kind != 'dialog' or value.get('handle') != request.get('handle'):
        return 'the portal request state changed before the click (%s)' % (value if kind == 'blocked' else kind)
    if window is not None:
        now = _new_backend_windows(watch.ctx, watch, request)
        if [w['id'] for w in now] != [window['id']] or now[0].get('frame') != window.get('frame'):
            return 'the backend\'s windows changed before the click'
    return None


def drive(watch, request):
    """One guarded real click on the backend dialog's accept button."""
    ctx, consent = watch.ctx, watch.consent
    if watch.clicked:
        raise RecipeBlocked('ScreenCast consent was already clicked once for this request; never twice', issue=ISSUE)
    if getattr(ctx.target, 'physical', None):
        raise RecipeBlocked('ScreenCast consent stores a portal grant (Remember this selection); drive it only in '
                            'disposable lab guests', issue=ISSUE)
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
            watch.stable = None
            return reason
    node, reason = _accept_node(ctx, watch, request, window)
    if reason:
        watch.stable = None
        return reason
    # A newly drawn GTK4 dialog can ignore a press before it settles: click
    # only after two observations at least 0.25 s apart agree on the window
    # frame and the sensitive, showing button.
    observed = (window and window['id'], window and tuple(window['frame']), tuple(node['rect']), time.monotonic())
    previous, watch.stable = watch.stable, observed
    if previous is None or previous[:3] != observed[:3] or observed[3] - previous[3] < .25:
        return 'waiting for the consent dialog to settle'
    x, y, w, h = node['rect']
    point = (x + w / 2, y + h / 2)
    if not shell:
        fx, fy, fw, fh = window['frame']
        if not (fx <= point[0] < fx + fw and fy <= point[1] < fy + fh):
            return 'ScreenCast consent accept button lies outside its dialog frame'
    ctx.frame('screencast-consent-before')
    if shell:
        reason = _unchanged(watch, request, None)
        if reason:
            raise RecipeBlocked('ScreenCast consent: ' + reason, issue=ISSUE)
        # Shell-drawn dialog: the shell stage pick proves the node is on top.
        watch.clicked = True  # before dispatch: a failed press is never retried
        ctx._accessible_pointer(None, 'left', node=node)
    else:
        geometry = {'x': fx, 'y': fy, 'width': fw, 'height': fh, 'pid': request['backend_pid'],
                    'window_id': window['id'], 'coordinate_space': 'logical'}
        # The compositor's own pick at the point must be the dialog window.
        ctx._guard(window='screencast-consent', point=point, geometry=geometry)
        # Re-read the bus and windows after the pick: a request or dialog that
        # appeared since the decision refuses the click.
        reason = _unchanged(watch, request, window)
        if reason:
            raise RecipeBlocked('ScreenCast consent: ' + reason, issue=ISSUE)
        mapped = ctx._map_geometry({'x': point[0], 'y': point[1], 'coordinate_space': 'logical'})
        scale = mapped.get('scale', 1)
        px, py = (round((point[i] - mapped.get(('screen_x', 'screen_y')[i], 0)) * scale) for i in (0, 1))
        watch.clicked = True  # before dispatch: a failed press is never retried
        ctx._pointer_click(px, py, 'left', mapped['head'], guarded=True)
    watch.dialog = {'backend': request.get('backend_exe'), 'window': window and window['id']}
    ctx.note('screencast consent: clicked ' + repr((consent.get('accept') or {}).get('name', 'Share')) +
             ' in ' + str(request.get('backend_exe')) + ' for this resident\'s request (app id ' +
             repr(request.get('app_id') or '') + ')')
    # Frame the desktop once the dialog has gone (bounded; the portal
    # Response, not this frame, proves acceptance).
    until = time.monotonic() + 3
    while window is not None and time.monotonic() < until:
        if not any(w['id'] == window['id'] for w in compositor_windows(ctx)['windows']):
            break
        time.sleep(.2)
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
                if reason and reason not in watch.refusals:
                    # Evidence for each distinct pre-input refusal; no input was sent.
                    watch.refusals.append(reason)
                    ctx.note('screencast consent not yet driven: ' + reason)
                last = reason or 'accept clicked; waiting for the portal response'
        elif watch.state()['ours']:
            last = 'this resident\'s ScreenCast Start was sent; no backend dialog request observed yet'
        if time.monotonic() >= deadline:
            raise RecipeBlocked('ScreenCast consent/ready not reached in %ds: %s' % (timeout, last), issue=ISSUE)
        time.sleep(.3)


__all__ = ['Fixture', 'analyse', 'app_name', 'await_ready', 'backend_idle', 'decision', 'drive', 'policy', 'records', 'start']
