"""Settings-only input checks; core transports remain owned by the core lane."""
import json
import shlex

from tools.lab.surface_driver.api import RecipeBlocked


# SD_SETTINGS_CONTROLS: inspect only the verified helper. The generic desktop
# query also traverses every Cinnamon menu and unrelated application.
OWNED_CONTROLS = r'''import fnmatch, json, os, pathlib, sys
import pyatspi
request = json.loads(sys.argv[1])
pid, query = request['pid'], request['query']
environment = pathlib.Path('/proc/' + str(pid) + '/environ').read_bytes().split(b'\0')
assert ('XDG_RUNTIME_DIR=' + os.environ['XDG_RUNTIME_DIR']).encode() in environment
apps = []
for app in pyatspi.Registry.getDesktop(0):
    try:
        if app.get_process_id() == pid:
            apps.append(app)
    except (RuntimeError, AttributeError):
        pass
stack, rows, visited = [(app, 0) for app in apps], [], 0
while stack and visited < 6000:
    node, depth = stack.pop()
    visited += 1
    try:
        # Hidden tabs contain thousands of descendants. They cannot receive
        # this input; keep offscreen-but-visible controls so scrolling can find them.
        if depth and not node.getState().contains(pyatspi.STATE_VISIBLE):
            continue
        name, role = node.name or '', node.getRoleName()
        if fnmatch.fnmatchcase(name, query['name']) and role == query['role']:
            rect = list(node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS))
            rows.append({'pid': pid, 'name': name, 'role': role, 'rect': rect})
        if depth < 30:
            stack.extend((child, depth + 1) for child in node if child is not None)
    except (RuntimeError, AttributeError, NotImplementedError):
        pass
assert not stack, 'private accessibility tree exceeded the bounded scan'
print(json.dumps(rows))
'''


def controls(ctx, query, pid):
    if not ctx.profile.get('at_spi', {}).get('available'):
        ctx.blocked('Settings live controls require the profile accessibility capability.')
    request = json.dumps({'pid': int(pid), 'query': query})
    reply = ctx.shell('python3 -c ' + shlex.quote(OWNED_CONTROLS) + ' ' + shlex.quote(request))
    if reply['returncode']:
        ctx.blocked('Settings private accessibility probe unavailable; input withheld.')
    return json.loads(reply['stdout'])


def observe(ctx, geometry, *, foreground=False):
    key = 'foreground_script' if foreground else 'input_state_script'
    script = ctx.profile.get('settings', {}).get(key)
    if not script:
        ctx.blocked('Settings input requires the profile ' + key + ' witness.')
    reply = ctx.shell('settings_window=' + shlex.quote(str(geometry['window_id'])) + '\n' + script)
    if reply['returncode']:
        ctx.blocked('Settings foreground/ownership witness failed; input withheld.', issue=699)
    witness = json.loads(reply['stdout'])
    ctx.note('settings input witness: ' + json.dumps(witness, sort_keys=True))
    if str(witness['active']) != str(geometry['window_id']):
        ctx.blocked('Settings foreground is obstructed; input withheld.', issue=699)
    return witness


def prepare(ctx):
    script = ctx.profile.get('settings', {}).get('prepare_script')
    geometry = ctx.geometry('settings')
    if script:
        reply = ctx.shell('settings_window=' + shlex.quote(str(geometry['window_id'])) + '\n' + script)
        ctx.expect(reply['returncode'] == 0, 'profile prepared Settings window and accessibility')
    return geometry


def ensure_accessible(ctx):
    geometry = ctx.geometry('settings')
    owner = observe(ctx, geometry, foreground=True)
    query = ctx.profile['settings']['live_controls']['settingsSection0']
    if any(row.get('pid') == owner['pid'] for row in controls(ctx, query, owner['pid'])):
        return
    # Qt 6.4 reads the AT-SPI properties at helper startup and does not
    # subscribe to their changes (qtbase/src/gui/accessible/linux/dbusconnection.cpp).
    # A fresh profile may already have opened its first-launch helper. Close
    # only this foreground, private-profile window and reopen it cooperatively.
    ctx.note('Reopening the private Settings helper after enabling accessibility.')
    ctx.frame('accessibility-before-reopen')
    ctx.keys('Alt+F4')
    pid = str(int(owner['pid']))
    ctx.wait(lambda: ctx.shell('test ! -d /proc/' + pid)['returncode'] == 0,
             'previous private Settings helper exits')
    ctx.view('ui.show', {'view': 'settings'})
    def ready():
        try:
            return ctx.geometry('settings')
        except RecipeBlocked:
            return None
    ctx.wait(ready, 'reopened private Settings window is visible')
    prepare(ctx)
    ctx.frame('accessibility-after-reopen')


def click(ctx, control):
    """Resolve each click anew, including scroll/status/disclosure layout changes."""
    options = ctx.profile.get('settings', {})
    query = options.get('live_controls', {}).get(control)
    if not query:
        ctx.blocked('Settings live control query missing: ' + control)
    geometry = ctx.geometry('settings')
    owner = observe(ctx, geometry, foreground=True)
    top, bottom = options['viewport_insets']
    for attempt in range(12):
        geometry = ctx.geometry('settings')
        rows = controls(ctx, query, owner['pid'])
        if any(row.get('pid') != owner['pid'] for row in rows):
            ctx.blocked('Settings live control is missing or ambiguous: ' + control)
        matches = [row for row in rows if row.get('rect') and row['rect'][2] > 0]
        if not matches:
            if control.startswith('settingsSection'):
                ctx.blocked('Settings live control is missing or ambiguous: ' + control)
            # Qt omits fully clipped controls from its visible accessibility
            # tree. Search the page with real wheel input, then query anew.
            ctx.note('Searching the visible Settings page for ' + control)
            ctx.scroll(geometry['x'] + geometry['width'] - 24,
                       geometry['y'] + geometry['height'] // 2,
                       40 if attempt == 0 else -3)
            continue
        if len(matches) != 1:
            ctx.blocked('Settings live control is missing or ambiguous: ' + control)
        x, y, width, height = matches[0]['rect']
        if (geometry['x'] <= x and x + width <= geometry['x'] + geometry['width']
                and geometry['y'] + top <= y and y + height <= geometry['y'] + geometry['height'] - bottom):
            break
        # Scroll the page at its right edge, never over a choice or spin box.
        ctx.scroll(geometry['x'] + geometry['width'] - 24,
                   geometry['y'] + geometry['height'] // 2,
                   3 if y < geometry['y'] + top else -3)
    else:
        ctx.blocked('Settings live control could not be brought into the viewport: ' + control)
    # The right end of a keycap contains its independent Clear button.
    point = (round(x + width / 3), round(y + height / 2))
    ctx.frame('input-before-' + control)
    ctx.note('settings live target: ' + json.dumps({'control': control, 'rect': [x, y, width, height],
                                                   'point': point, 'geometry': geometry}, sort_keys=True))
    observe(ctx, geometry)
    ctx.click_at(*point)
    witness = observe(ctx, geometry)
    ctx.expect(abs(witness['x'] - point[0]) <= 1 and abs(witness['y'] - point[1]) <= 1,
               'pointer reached the live Settings control')
    ctx.frame('input-after-' + control)
