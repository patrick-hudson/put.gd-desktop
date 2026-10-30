"""Settings-only input checks; core transports remain owned by the core lane."""
import json
import shlex

from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed


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
    if any(row.get('pid') == owner['pid'] for row in ctx.accessible(query)):
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
        def matching():
            return [row for row in ctx.accessible(query)
                    if row.get('pid') == owner['pid'] and row.get('rect') and row['rect'][2] > 0]
        try:
            matches = ctx.wait(matching, 'Settings accessibility exposes ' + control)
        except RecipeFailed:
            ctx.blocked('Settings live control is missing or ambiguous: ' + control)
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
