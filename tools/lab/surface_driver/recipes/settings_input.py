"""Settings-only input checks; core transports remain owned by the core lane."""
import json
import shlex

from tools.lab.surface_driver.api import RecipeFailed


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
