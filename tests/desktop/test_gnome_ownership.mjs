// Native Shell APIs are fixtures here: this suite proves extension control and
// frozen-target behavior, never graphical acceptance or compositor permission.
// Run with GNOME's own interpreter: gjs -m this-file extension-source-path.
// No Node.js, package manager or extra JavaScript engine is required.
import FileIO from 'gi://Gio';

function equal(actual, expected) {
    if (actual !== expected)
        throw new Error(`Expected ${String(expected)}, received ${String(actual)}`);
}
function deepEqual(actual, expected) {
    equal(JSON.stringify(actual), JSON.stringify(expected));
}
async function rejects(promise, predicate) {
    try {
        await promise;
    } catch (error) {
        if (predicate(error))
            return;
        throw error;
    }
    throw new Error('Expected the operation to reject');
}
const assert = {equal, deepEqual, rejects};
if (ARGV.length !== 1)
    throw new Error('Supply the production GNOME extension source path');
const [loaded, contents] = FileIO.File.new_for_path(ARGV[0]).load_contents(null);
if (!loaded)
    throw new Error('Could not read extension source');

const source = new TextDecoder('utf-8', {fatal: true}).decode(contents)
    .replace(/^import .*;$/gm, '')
    .replace('class TrayTooltipCompanion', 'globalThis.TrayTooltipCompanion = class TrayTooltipCompanion')
    .replace('export default class PutgdExtension', 'globalThis.PutgdExtension = class PutgdExtension');
const resident = ':1.50';
const residentPid = 1000;
const helperPid = 2000;
const context = {
    Clutter: {EVENT_PROPAGATE: 0, EventType: {BUTTON_PRESS: 1}},
    Gio: {_promisify() {}, Cancellable: class {cancel() {}}, DBusCallFlags: {NONE: 0},
        DBus: {session: {call_sync(_service, _path, _iface, method) {
            return {deep_unpack: () => [method === 'GetNameOwner' ? resident : residentPid]};
        }}}},
    GLib: {Variant: class {constructor(_type, value) {this.value = value;}}, VariantType: class {},
        timeout_add: () => 1, source_remove() {}, PRIORITY_DEFAULT: 0},
    Shell: {Screenshot: class {}}, GObject: {registerClass: cls => cls}, St: {Widget: class {}},
    Extension: class {}, Main: {sessionMode: {isLocked: false}},
    global: {display: {focus_window: null}},
};
// Direct eval captures only these deliberately mocked Shell dependencies.
const {Clutter, Gio, GLib, Shell, GObject, St, Extension, Main, global} = context;
eval(source);
context.PutgdExtension = globalThis.PutgdExtension;
function fixture() {
    const extension = new context.PutgdExtension();
    Object.assign(extension, {_enabled: true, _requests: new Map(), _copyEpoch: 0, _revision: 1,
        _uiProcess: 0, _uiProcessOwner: null});
    let layouts = 0;
    extension._layout = async () => {layouts++; return [];};
    return {extension, layouts: () => layouts};
}
function focused(pid) {
    const actor = {};
    return {get_pid: () => pid, get_compositor_private: () => actor, minimized: false,
        get_frame_rect: () => ({x: 10, y: 20, width: 30, height: 40})};
}
function register(extension, pid, sender = resident) {
    return new Promise(resolve => extension.SetUiProcessAsync([pid], {
        get_sender: () => sender, get_parameters: () => ({deep_unpack: () => [pid]}),
        return_value: value => resolve(JSON.parse(value.value[0])),
    }));
}
let serial = 0;
const id = () => `00000000-0000-0000-0000-${String(++serial).padStart(12, '0')}`;

const own = fixture();
context.global.display.focus_window = focused(residentPid);
await assert.rejects(own.extension._prepare(id(), 'window', resident), e => e.bridgeCode === 'capture_target');
assert.equal(own.layouts(), 0);
print('PASS resident PID rejection before layout');

const helper = fixture();
assert.deepEqual(await register(helper.extension, helperPid), {});
context.global.display.focus_window = focused(helperPid);
await assert.rejects(helper.extension._prepare(id(), 'window', resident), e => e.bridgeCode === 'capture_target');
assert.equal(helper.layouts(), 0);
print('PASS registered helper PID rejection before layout');

assert.equal((await register(helper.extension, 3000, ':1.99')).error.code, 'bridge_unauthorized');
await assert.rejects(helper.extension._prepare(id(), 'window', resident), e => e.bridgeCode === 'capture_target');
print('PASS foreign caller cannot replace helper identity');

assert.deepEqual(await register(helper.extension, 0), {});
const afterExit = id();
await helper.extension._prepare(afterExit, 'window', resident);
assert.equal(helper.extension._requests.get(afterExit).window.get_pid(), helperPid);
print('PASS helper exit clears exclusion');

const external = focused(3000);
context.global.display.focus_window = external;
let releaseLayout;
helper.extension._layout = () => new Promise(resolve => {releaseLayout = resolve;});
const frozenId = id();
const prepared = helper.extension._prepare(frozenId, 'window', resident);
assert.equal(helper.extension._requests.get(frozenId).window, external);
await register(helper.extension, helperPid);
context.global.display.focus_window = focused(helperPid);
releaseLayout([]);
await prepared;
assert.equal(helper.extension._requests.get(frozenId).window, external);
print('PASS helper registration/focus changes do not replace frozen target');

// This is a synthetic Shell/SNI contract. It never creates a real tray host or
// establishes hover rendering, desktop permission, service or memory acceptance.
assert.equal(typeof globalThis.TrayTooltipCompanion, 'function');
const watches = new Map(), timers = new Map(), subscriptions = new Map();
let signalId = 10;
class Widget {
    constructor(properties = {}) {
        Object.assign(this, {visible: true, mapped: true, reactive: true, hover: false}, properties);
        this.children = []; this.signals = new Map(); this.parent = null;
        this.clutter_text = {set_line_wrap() {}};
    }
    connect(name, callback) {const id = ++signalId; this.signals.set(id, [name, callback]); return id;}
    disconnect(id) {this.signals.delete(id);}
    emit(name, ...args) {
        let result;
        for (const [event, callback] of [...this.signals.values()]) if (event === name) result = callback(this, ...args);
        return result;
    }
    get_children() {return this.children;}
    get_parent() {return this.parent;}
    add_child(child) {this.children.push(child); child.parent = this;}
    remove_child(child) {this.children = this.children.filter(value => value !== child); child.parent = null;}
    get_transformed_position() {return [1100, 0];}
    get_transformed_size() {return [24, 30];}
    get_preferred_width() {return [40, 320];}
    get_preferred_height() {return [24, 80];}
    set_text(text) {this.text = text;}
    set_size(width, height) {this.width = width; this.height = height;}
    set_position(x, y) {this.x = x; this.y = y;}
    show() {this.visible = true;}
    hide() {this.visible = false;}
    destroy() {this.emit('destroy'); this.parent?.remove_child(this); this.signals.clear(); this.destroyed = true;}
}
context.St.Widget = Widget;
context.St.Label = class extends Widget {};
context.Main.uiGroup = new Widget();
context.Main.panel = new Widget();
context.Main.layoutManager = {monitors: [{x: 0, y: 0, width: 1200, height: 800}]};
context.Main.sessionMode = new Widget({isLocked: false});
context.Gio.BusType = {SESSION: 0};
context.Gio.BusNameWatcherFlags = {NONE: 0};
context.Gio.DBusSignalFlags = {NONE: 0};
context.Gio.Cancellable = class {cancel() {this.cancelled = true;}};
context.Gio.bus_watch_name = (_type, name, _flags, appeared, vanished) => {
    const id = ++signalId; watches.set(id, {name, appeared, vanished}); return id;
};
context.Gio.bus_unwatch_name = id => watches.delete(id);
Object.assign(context.GLib, {SOURCE_REMOVE: false, SOURCE_CONTINUE: true,
    timeout_add: (_priority, _delay, callback) => {const id = ++signalId; timers.set(id, callback); return id;},
    source_remove: id => timers.delete(id)});
let itemPid = residentPid, tooltip = ['', [], 'put.gd — Working', 'Uploading: 1 MiB of 4 MiB (25%)'];
let holdTooltip = false;
const held = [];
const service = 'org.kde.StatusNotifierItem.putgd_contract';
const bus = context.Gio.DBus.session;
bus.call = (destination, _path, _iface, method, args, _type, _flags, timeout, _cancel, callback) => {
    assert.equal(timeout > 0 && timeout <= 1000, true);
    const input = args?.value ?? [];
    let value;
    if (method === 'GetNameOwner') value = input[0] === 'org.putgd.App.Gnome50' ? resident : input[0] === service ? ':1.60' : ':1.70';
    else if (method === 'GetConnectionUnixProcessID') value = input[0] === ':1.60' ? itemPid : residentPid;
    else if (method === 'Get') {
        const property = input[1];
        value = {IsStatusNotifierHostRegistered: true, RegisteredStatusNotifierItems: [service + '/StatusNotifierItem'],
            Id: 'putgd', Title: 'put.gd', ToolTip: tooltip}[property];
    } else throw new Error('Unexpected async method: ' + method);
    const result = {deep_unpack: () => [value]};
    if (holdTooltip && input[1] === 'ToolTip') held.push(() => callback(bus, result));
    else Promise.resolve().then(() => callback(bus, result));
};
bus.call_finish = result => result;
bus.signal_subscribe = (sender, iface, signal, path, _arg, _flags, callback) => {
    const id = ++signalId; subscriptions.set(id, {sender, iface, signal, path, callback}); return id;
};
bus.signal_unsubscribe = id => subscriptions.delete(id);
async function flush() {for (let i = 0; i < 80; i++) await Promise.resolve();}
function tick() {for (const [id, callback] of [...timers]) if (!callback()) timers.delete(id);}
function appear(name, owner) {for (const watch of watches.values()) if (watch.name === name) watch.appeared(bus, name, owner);}
function emitTooltip(sender) {
    for (const subscription of subscriptions.values())
        if (subscription.sender === sender && subscription.signal === 'NewToolTip')
            subscription.callback(bus, sender, '/StatusNotifierItem', 'org.kde.StatusNotifierItem', 'NewToolTip');
}
function companionFixture() {
    const actor = new Widget({accessible_name: 'put.gd'});
    context.Main.panel.add_child(actor);
    const companion = new globalThis.TrayTooltipCompanion(); companion.enable();
    appear('org.putgd.App.Gnome50', resident); appear('org.kde.StatusNotifierWatcher', ':1.70');
    return {actor, companion};
}
const live = companionFixture();
await flush(); live.actor.hover = true; live.actor.emit('notify::hover'); tick(); await flush();
assert.equal(timers.size, 0); // No permanent idle discovery/read loop once bound.
assert.equal(context.Main.uiGroup.get_children().filter(actor => actor.visible).length, 1);
assert.equal(context.Main.uiGroup.get_children()[0].text, 'put.gd — Working\nUploading: 1 MiB of 4 MiB (25%)');
assert.equal(context.Main.panel.get_children()[0], live.actor);
tooltip = ['', [], 'put.gd — Working', 'Creating share link']; emitTooltip(':1.99'); await flush();
assert.equal(context.Main.uiGroup.get_children()[0].text.includes('Uploading'), true);
emitTooltip(':1.60'); await flush();
assert.equal(context.Main.uiGroup.get_children()[0].text, 'put.gd — Working\nCreating share link');
print('PASS authenticated SNI counters update a companion on the existing public panel actor');

assert.equal(live.actor.emit('captured-event', {type: () => Clutter.EventType.BUTTON_PRESS}), Clutter.EVENT_PROPAGATE);
assert.equal(context.Main.uiGroup.get_children()[0].visible, false);
tooltip = ['', [], 'put.gd — Working', 'Finalizing']; emitTooltip(':1.60'); await flush();
assert.equal(context.Main.uiGroup.get_children()[0].visible, false);
live.actor.hover = false; live.actor.emit('notify::hover');
live.actor.hover = true; live.actor.emit('notify::hover'); tick(); await flush();
assert.equal(context.Main.uiGroup.get_children()[0].visible, true);
print('PASS native button input propagates and suppresses the label until hover leaves and returns');

const duplicate = new Widget({accessible_name: 'put.gd'}); context.Main.panel.add_child(duplicate);
emitTooltip(':1.60'); await flush();
assert.equal(context.Main.uiGroup.get_children().filter(actor => actor.visible).length, 0);
duplicate.destroy(); emitTooltip(':1.60'); await flush();
tooltip = ['', [], 'put.gd — Working', 'x'.repeat(2049)]; emitTooltip(':1.60'); await flush();
assert.equal(context.Main.uiGroup.get_children().filter(actor => actor.visible).length, 0);
print('PASS duplicate accessible actors and excessive tooltip text fail closed');

tooltip = ['', [], 'put.gd — Working', 'Uploading']; emitTooltip(':1.60'); await flush();
live.actor.hover = true; live.actor.emit('notify::hover'); tick(); await flush();
context.Main.sessionMode.isLocked = true; context.Main.sessionMode.emit('updated'); await flush();
assert.equal(context.Main.uiGroup.get_children().length, 0);
assert.equal(live.actor.signals.size, 0);
context.Main.sessionMode.isLocked = false; context.Main.sessionMode.emit('updated'); await flush();
holdTooltip = true; emitTooltip(':1.60'); await flush();
assert.equal(held.length > 0, true);
live.companion.disable(); for (const callback of held.splice(0)) callback(); await flush();
assert.equal(context.Main.uiGroup.get_children().length, 0);
assert.equal(live.actor.signals.size, 0); assert.equal(watches.size, 0);
assert.equal(timers.size, 0); assert.equal(subscriptions.size, 0);
live.actor.destroy(); holdTooltip = false;
print('PASS lock/disable cancel pending reads and remove only companion-owned actors/signals');

itemPid = residentPid + 1;
const foreign = companionFixture(); await flush(); foreign.actor.hover = true; foreign.actor.emit('notify::hover'); tick(); await flush();
assert.equal(context.Main.uiGroup.get_children().length, 0);
assert.equal(foreign.actor.signals.size, 0);
foreign.companion.disable(); foreign.actor.destroy();
print('PASS same-name SNI from another Unix PID cannot supply the tooltip');
