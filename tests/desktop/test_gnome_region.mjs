// Synthetic actors prove the production selector's geometry and lifecycle.
// Native input routing and rendering require a separate Shell session.
import Gio from 'gi://Gio';

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
    constructor(properties) {this._init(properties);}
    _init(properties) {Object.assign(this, properties); this.children = []; this.clutter_text = {set_line_wrap() {}};}
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
    EVENT_STOP: 1, EVENT_PROPAGATE: 0,
    KEY_Left: 10, KEY_Right: 11, KEY_Up: 12, KEY_Down: 13, KEY_Return: 14, KEY_KP_Enter: 15, KEY_Escape: 16, KEY_Tab: 17, KEY_ISO_Left_Tab: 18,
};
let pointer = [2100, 200], grabs = 0;
const global = {stage: {}, get_pointer: () => pointer};
const monitors = [{x: 0, y: 0, width: 1920, height: 1080}, {x: 1920, y: -100, width: 1280, height: 720}];
const Main = {uiGroup: new Widget(), layoutManager: {monitors, primaryMonitor: monitors[0]},
    pushModal() {grabs++; return {is_revoked: () => false};}, popModal() {grabs--;}};
function check(condition, _code, message) {if (!condition) throw new Error(message);}
eval(selector);
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
    equal(actor._instructions.text.includes('Enter captures this entire display'), true);
    equal([actor._rectangle.x, actor._rectangle.y, actor._rectangle.width, actor._rectangle.height], [1920, -100, 1280, 720]);
    key(actor, 'Left', 3);
    equal(actor._instructions.text.includes('Enter captures the selected region'), true);
    equal(actor._instructions.text.includes('640 × 360 logical pixels'), true);
    rect(actor, [2240, 80, 640, 360]); // First arrow initializes, ignoring modifiers.
    key(actor, 'Right'); rect(actor, [2241, 80, 640, 360]);
    key(actor, 'Down', 1); rect(actor, [2241, 90, 640, 360]);
    key(actor, 'Right', 2); rect(actor, [2241, 90, 641, 360]);
    key(actor, 'Up', 3); rect(actor, [2241, 90, 641, 350]);
    equal(actor._rectangle.description.includes('641 × 350 at 2241, 90.'), true);
    equal(actor._instructions.x >= 1920, true);
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
    equal(actor._instructions.text.includes('Drag and release to capture'), true);
    key(actor, 'Tab');
    equal(actor._monitor(), monitors[0]);
    equal(actor._instructions.text.includes('Display 1'), true);
    key(actor, 'ISO_Left_Tab');
    key(actor, 'Right');
    equal(actor._instructions.text.includes('Drag and release to capture'), true);
    key(actor, 'Tab'); rect(actor, [320, 180, 640, 360]);
    key(actor, 'Tab', 2); rect(actor, [2240, 80, 640, 360]);
    key(actor, 'Escape'); equal(await result, null); closed(actor);
    equal(RegionSelection.switchDisplay({x: 0, y: 0, width: 1920, height: 1080}, monitors, 0),
        {current: 1, region: {x: 1920, y: -100, width: 1280, height: 720}});
    equal(RegionSelection.instructions(null, true, 'Display 2').includes('Drag and release to use a region'), true);
    equal(RegionSelection.instructions(null, true, 'Display 2').includes('Enter chooses this entire display'), true);
    equal(RegionSelection.instructions({width: 2, height: 2}, true, 'Display 2').includes('Enter chooses this region'), true);
    equal(RegionSelection.instructions({width: 2, height: 2}, true, 'Display 2').includes('2 × 2 logical pixels'), true);
    equal(RegionSelection.instructions({width: 2, height: 2}, true, 'Display 2').includes('Drag and release to use a region'), true);
}
print('PASS display cycling, reverse cycling, clamping, display names and state-specific hints');

{
    const actor = new globalThis.RegionSelector(true);
    const result = actor.selectAsync();
    equal(actor.accessible_name, 'Recording region');
    equal(actor._instructions.text.includes('Drag and release to use a region'), true);
    key(actor, 'Right');
    equal(actor._instructions.text.includes('Drag and release to use a region'), true);
    key(actor, 'Escape'); equal(await result, null); closed(actor);
}
print('PASS recording selector purpose and live Enter hints');
{
    const monitor = {x: -800, y: 100, width: 800, height: 600};
    equal(RegionSelection.instructionPosition(monitor, 400, 100, {x: -600, y: 100, width: 400, height: 200}, null), [-600, 584]);
    equal(RegionSelection.instructionPosition(monitor, 400, 100, null, [-400, 140]), [-600, 584]);
    equal(RegionSelection.instructionPosition(monitor, 400, 100, null, [-400, 640]), [-600, 116]);
}
print('PASS instruction badge avoids selection and drag pointer on offset monitors');
