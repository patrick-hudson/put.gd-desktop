// Synthetic actors prove the production selector's geometry and lifecycle.
// Native input routing and rendering require a separate Shell session.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

function equal(actual, expected) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error(`Expected ${JSON.stringify(expected)}, received ${JSON.stringify(actual)}`);
}
if (ARGV.length !== 1)
    throw new Error('Supply the production GNOME extension source path');
const [, contents] = Gio.File.new_for_path(ARGV[0]).load_contents(null);
const source = new TextDecoder().decode(contents);
const selector = source.slice(source.indexOf('const RegionSelection ='), source.indexOf('// AppIndicators'))
    .replace('const RegionSelection =', 'globalThis.RegionSelection =').replace('const RegionSelector =', 'globalThis.RegionSelector =');
class Widget {
    constructor(...args) {this._init(...args);}
    _init(properties) {Object.assign(this, properties); this.children = []; this.clutter_text = {set_line_wrap() {}, set_markup(value) {this.markup = value;}};}
    add_constraint() {}
    set_cursor_type() {}
    add_child(child) {this.children.push(child);}
    connect() {}
    set_position(x, y) {Object.assign(this, {x, y});}
    set_size(width, height) {Object.assign(this, {width, height});}
    set_width(width) {this.width = width;}
    get_preferred_width() {return [0, 650];}
    get_preferred_height() {return [0, 112];}
    show() {this.visible = true;}
    set_content(content) {this.content = content;}
    ease(params) {this.eased = params;}
    remove_all_transitions() {this.eased = null;}
    hide() {this.visible = false;}
    destroy() {this.destroyed = true;}
    get_accessible() {return {set_description: value => {this.description = value;}};}
}
const GObject = {registerClass: cls => cls};
const St = {Widget, Label: Widget, ThemeContext: {get_for_stage: () => ({scale_factor: 1})}};
const Clutter = {
    BindConstraint: class {}, BindCoordinate: {ALL: 0}, CursorType: {CROSSHAIR: 0},
    EventType: {KEY_PRESS: 1, BUTTON_PRESS: 2, BUTTON_RELEASE: 3, MOTION: 4},
    ModifierType: {CONTROL_MASK: 1, SHIFT_MASK: 2}, BUTTON_PRIMARY: 1, BUTTON_SECONDARY: 3,
    EVENT_STOP: 1, EVENT_PROPAGATE: 0, AnimationMode: {EASE_OUT_QUAD: 5},
    KEY_Left: 10, KEY_Right: 11, KEY_Up: 12, KEY_Down: 13, KEY_Return: 14, KEY_KP_Enter: 15, KEY_Escape: 16, KEY_Tab: 17, KEY_ISO_Left_Tab: 18,
};
let pointer = [2100, 200], grabs = 0;
const global = {display: {get_monitor_scale: () => 1}, stage: {width: 3200, height: 1080}, get_pointer: () => pointer};
const monitors = [{x: 0, y: 0, width: 1920, height: 1080}, {x: 1920, y: -100, width: 1280, height: 720}];
const Main = {uiGroup: new Widget(), layoutManager: {monitors, primaryMonitor: monitors[0]},
    pushModal() {grabs++; return {is_revoked: () => false};}, popModal() {grabs--;}};
function check(condition, _code, message) {if (!condition) throw new Error(message);}
eval(selector);
const scaledOutputs = [{x: -800, y: 100, width: 800, height: 600, density: 1.25}, {x: 0, y: -200, width: 800, height: 600, density: 2}];
equal(RegionSelection.outputSize({x: -799, y: 101, width: 101, height: 51}, scaledOutputs), {width: 127, height: 64});
equal(RegionSelection.outputSize({x: -100, y: 100, width: 200, height: 100}, scaledOutputs), {width: 400, height: 200});
equal(RegionSelection.instructions(null, false, 'Left', scaledOutputs[0], scaledOutputs).includes('Entire display · 1000 × 750 px'), true);
equal(RegionSelection.instructions({x: 0, y: 0, width: 50, height: 30}, false, 'Unknown', null).includes('50 × 30 logical px'), true);
const oneOutput = [{x: 0, y: 0, width: 800, height: 600, density: 1}];
const oneHint = RegionSelection.instructions({x: 0, y: 0, width: 400, height: 400}, false, 'Display 1', oneOutput[0], oneOutput, false, 'Saves locally');
equal(oneHint.split('\n').length, 3);
equal(oneHint.split('\n')[0], '400 × 400 px · Enter to capture · Esc to cancel');
equal(oneHint.includes('Display 1'), false);
print('PASS output pixel sizes on fractional, mixed-density and unknown outputs');
for (const outcome of ['Uploads right away to Team Drive · Shared link', 'Saves locally', 'Opens the image editor']) {
    const actor = new globalThis.RegionSelector(false, null, outcome);
    const result = actor.selectAsync();
    equal(actor._instructionText.endsWith(`\n${outcome}`), true);
    actor._input({type: () => Clutter.EventType.KEY_PRESS, get_key_symbol: () => Clutter.KEY_Right, get_state: () => 0});
    equal(actor._instructionText.endsWith(`\n${outcome}`), true);
    actor.cancel();
    await result;
    equal(RegionSelection.instructions(null, true, 'Display 1', null, [], false, outcome).includes(outcome), false);
}
print('PASS resolved screenshot outcomes survive keyboard selection without changing recording guidance');
function begin() {
    const actor = new globalThis.RegionSelector();
    const result = actor.selectAsync();
    return {actor, result};
}
function key(actor, symbol, state = 0) {
    equal(actor._input({type: () => Clutter.EventType.KEY_PRESS, get_key_symbol: () => Clutter[`KEY_${symbol}`],
        get_state: () => state}), Clutter.EVENT_STOP);
}
function mouse(actor, type, point, button = 1) {
    equal(actor._input({type: () => Clutter.EventType[type], get_button: () => button, get_coords: () => point}), Clutter.EVENT_STOP);
}
function rect(actor, expected) {equal(Object.values(actor._region), expected);}
function closed(actor) {equal([actor.destroyed, grabs], [true, 0]);}

{
    const {actor, result} = begin();
    equal(actor._instructionText.includes('Enter to capture'), true);
    equal(actor._instructionText.includes('Display 2 · Entire display · 1280 × 720 px'), true);
    equal([actor._rectangle.x, actor._rectangle.y, actor._rectangle.width, actor._rectangle.height], [1920, -100, 1280, 720]);
    key(actor, 'Left', 3);
    equal(actor._instructionText.includes('Enter to capture'), true);
    equal(actor._instructionText.includes('640 × 360'), true);
    rect(actor, [2240, 80, 640, 360]); // First arrow initializes, ignoring modifiers.
    key(actor, 'Right'); rect(actor, [2241, 80, 640, 360]);
    key(actor, 'Down', 1); rect(actor, [2241, 90, 640, 360]);
    key(actor, 'Right', 2); rect(actor, [2241, 90, 641, 360]);
    key(actor, 'Up', 3); rect(actor, [2241, 90, 641, 350]);
    equal(actor._rectangle.description.includes('641 × 350 at 2241, 90.'), true);
    // The size label follows the keyboard selection on its monitor.
    equal(actor._size.visible, true);
    equal(actor._size.text, '641 × 350');
    equal(actor._size.x >= 1920, true);
    // Pointer motion without dragging must not disturb a keyboard region.
    mouse(actor, 'MOTION', [100, 100]); rect(actor, [2241, 90, 641, 350]);
    key(actor, 'KP_Enter');
    equal(await result, {x: 2241, y: 90, width: 641, height: 350}); closed(actor);
}
print('PASS keyboard initialization, modifiers, accessibility and keypad accept on offset monitor');
{
    const {actor, result} = begin();
    key(actor, 'Down');
    for (let i = 0; i < 200; i++) key(actor, 'Left', 1);
    for (let i = 0; i < 200; i++) key(actor, 'Up', 1);
    rect(actor, [1920, -100, 640, 360]);
    for (let i = 0; i < 200; i++) key(actor, 'Left', 3);
    for (let i = 0; i < 200; i++) key(actor, 'Up', 3);
    rect(actor, [1920, -100, 1, 1]);
    for (let i = 0; i < 200; i++) key(actor, 'Right', 3);
    for (let i = 0; i < 200; i++) key(actor, 'Down', 3);
    rect(actor, [1920, -100, 1280, 720]);
    key(actor, 'Escape'); equal(await result, null); closed(actor);
}
print('PASS movement and resize bounds, minimum one pixel and Escape cleanup');
for (const position of [[2100, 200], [-200, -200]]) {
    pointer = position;
    const {actor, result} = begin();
    key(actor, 'Return');
    equal(await result, monitors[position[0] > 0 ? 1 : 0]); closed(actor);
}
print('PASS Enter captures pointer monitor, falling back to primary outside monitors');
{
    pointer = [100, 100];
    const {actor, result} = begin();
    mouse(actor, 'BUTTON_PRESS', [350.9, 450.8]);
    mouse(actor, 'MOTION', [100.9, 150.9]); rect(actor, [100, 150, 250, 300]);
    equal(actor._instructionText.includes('Release to capture'), true);
    equal(actor._instructionText.includes('Enter to capture'), false);
    equal(actor.description.includes('Release to capture'), true);
    mouse(actor, 'BUTTON_RELEASE', [90.1, 140.1]);
    equal(await result, {x: 90, y: 140, width: 260, height: 310}); closed(actor);
}
{
    const {actor, result} = begin();
    mouse(actor, 'BUTTON_PRESS', [10, 10]);
    mouse(actor, 'BUTTON_RELEASE', [10, 10]); equal(await result, null); closed(actor);
}
{
    const {actor, result} = begin();
    key(actor, 'Down');
    mouse(actor, 'BUTTON_PRESS', [100, 100], 3); equal(await result, null); closed(actor);
}
print('PASS reverse mouse drag, floored edges, zero-area cancellation and secondary-click cancel');
{
    const {actor, result} = begin();
    mouse(actor, 'BUTTON_PRESS', [1900, -20]);
    mouse(actor, 'MOTION', [2200, 200]);
    key(actor, 'Right'); rect(actor, [1921, -20, 280, 220]);
    mouse(actor, 'BUTTON_RELEASE', [2200, 200]);
    equal(actor.destroyed, undefined); // A key ends the drag without accepting it.
    key(actor, 'Return'); equal(await result, {x: 1921, y: -20, width: 280, height: 220}); closed(actor);
}
{
    const {actor, result} = begin();
    key(actor, 'Down');
    mouse(actor, 'BUTTON_PRESS', [10, 20]);
    mouse(actor, 'MOTION', [110, 120]);
    mouse(actor, 'BUTTON_RELEASE', [110, 120]);
    equal(await result, {x: 10, y: 20, width: 100, height: 100}); closed(actor);
}
print('PASS mouse-to-keyboard clipping and keyboard-to-mouse replacement');

{
    pointer = [2100, 200];
    const {actor, result} = begin();
    equal(actor._instructionText.includes('Drag to select'), true);
    key(actor, 'Tab');
    equal(actor._monitor(), monitors[0]);
    equal(actor._instructionText.includes('Display 1'), true);
    key(actor, 'ISO_Left_Tab');
    key(actor, 'Right');
    equal(actor._instructionText.includes('Drag to select'), true);
    key(actor, 'Tab'); rect(actor, [320, 180, 640, 360]);
    key(actor, 'Tab', 2); rect(actor, [2240, 80, 640, 360]);
    key(actor, 'Escape'); equal(await result, null); closed(actor);
    equal(RegionSelection.switchDisplay({x: 0, y: 0, width: 1920, height: 1080}, monitors, 0),
        {current: 1, region: {x: 1920, y: -100, width: 1280, height: 720}});
    equal(RegionSelection.instructions(null, true, 'Display 2').includes('Drag to select'), true);
    equal(RegionSelection.instructions(null, true, 'Display 2').includes('Enter to choose'), true);
    equal(RegionSelection.instructions({width: 2, height: 2}, true, 'Display 2').includes('Enter to choose'), true);
    equal(RegionSelection.instructions({width: 2, height: 2}, true, 'Display 2').includes('2 × 2'), true);
    equal(RegionSelection.instructions({width: 2, height: 2}, true, 'Display 2').includes('Drag to select'), true);
}
print('PASS display cycling, reverse cycling, clamping, display names and state-specific hints');

{
    const actor = new globalThis.RegionSelector(true);
    const result = actor.selectAsync();
    equal(actor.accessible_name, 'Recording region');
    equal(actor._instructionText.includes('Drag to select'), true);
    key(actor, 'Right');
    equal(actor._instructionText.includes('Drag to select'), true);
    key(actor, 'Escape'); equal(await result, null); closed(actor);
}
print('PASS recording selector purpose and live Enter hints');
{
    const monitor = {x: -800, y: 100, width: 800, height: 600};
    equal(RegionSelection.hintPosition(monitor, 400, 30, null), [-600, 116]);
    equal(RegionSelection.hintPosition(monitor, 400, 30, {x: -600, y: 100, width: 400, height: 200}), [-600, 654]);
    equal(RegionSelection.hintPosition(monitor, 400, 30, {x: -800, y: 100, width: 800, height: 600}), [-600, 116]);
    // Below the selection, above it without room, inside a full-monitor one.
    equal(RegionSelection.sizePosition(monitor, 80, 24, {x: -700, y: 200, width: 300, height: 200}), [-700, 404]);
    equal(RegionSelection.sizePosition(monitor, 80, 24, {x: -700, y: 500, width: 300, height: 190}), [-700, 472]);
    equal(RegionSelection.sizePosition(monitor, 80, 24, monitor), [-796, 104]);
    equal(RegionSelection.labelMonitor(monitors, {x: 1800, y: 0, width: 200, height: 10}), monitors[0]);
    equal(RegionSelection.labelMonitor(monitors, {x: 1800, y: 0, width: 300, height: 10}), monitors[1]);
    equal(RegionSelection.sizeLabel({x: -799, y: 101, width: 101, height: 51}, scaledOutputs), '127 × 64');
    equal(RegionSelection.sizeLabel(null, scaledOutputs), '');
    equal(RegionSelection.textureArea([1920, 0, 1280, 720], 1.5, 4800, 1620), [2880, 0, 1920, 1080]);
    equal(RegionSelection.textureArea([1920, -100, 1280, 720], 1, 3200, 1080), [1920, 0, 1280, 620]);
}
print('PASS hint pill and size label placement, label monitor, sizes and frozen texture areas');

for (const scheme of ['prefer-dark', 'prefer-light', 'default']) {
    const style = RegionSelection.pillStyle(scheme);
    equal(style.includes(scheme === 'prefer-dark' ? '#191919' : '#eeeeeb'), true);
    equal(style.includes(scheme === 'prefer-dark' ? '#f0f0ec' : '#292926'), true);
    equal(style.includes('border-radius: 999px'), true);
}
{
    const {actor, result} = begin();
    const settings = new Gio.Settings({schema_id: 'org.gnome.desktop.interface'});
    equal(actor._hint.style, RegionSelection.pillStyle(settings.get_string('color-scheme')));
    equal(actor._hint.text, 'Drag to select · Esc to cancel');
    equal(actor._hint.eased.opacity, 0);
    equal(actor._hint.eased.delay + actor._hint.eased.duration, 2000);
    equal(actor._size.visible, false);
    equal(actor._rectangle.style.includes('3px solid black'), true);
    equal(actor._contrast.style.includes('1px solid white'), true);
    // No scrim (#629): only the outline, the hint and the size label are children.
    equal(actor.children.length, 4);
    equal(actor._backdrop, undefined);
    key(actor, 'Escape'); equal(await result, null); closed(actor);
    equal(actor._hint.eased, null);
}
equal(RegionSelection.scrimAlpha, undefined);
{
    const frozen = {frozen: true};
    const actor = new globalThis.RegionSelector(false, null, '', frozen);
    const result = actor.selectAsync();
    equal(actor._backdrop.content, frozen);
    equal(actor.children[0], actor._backdrop);
    equal([actor._backdrop.width, actor._backdrop.height], [3200, 1080]);
    key(actor, 'Escape'); equal(await result, null); closed(actor);
}
print('PASS native scheme pill that fades, no scrim, frozen backdrop and black/white selection outline');

{
    const actor = new globalThis.RegionSelector(true);
    const result = actor.selectAsync();
    mouse(actor, 'BUTTON_PRESS', [100, 100]);
    mouse(actor, 'MOTION', [200, 200]);
    equal(actor._instructionText.includes('Release to choose recording area'), true);
    equal(actor.description.includes('Release to choose recording area'), true);
    mouse(actor, 'BUTTON_RELEASE', [200, 200]);
    equal(await result, {x: 100, y: 100, width: 100, height: 100}); closed(actor);
}
print('PASS release instructions match the actual mouse commit for capture and recording');
