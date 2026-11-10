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
    .replace('export default class PutgdExtension', 'globalThis.PutgdExtension = class PutgdExtension')
    .replace('function barPlacement(', 'globalThis.barPlacement = barPlacement; function barPlacement(')
    .replace('function subtract(', 'globalThis.subtract = subtract; function subtract(')
    .replaceAll('new RegionSelector(', 'new (globalThis.RegionSelectorFixture ?? RegionSelector)(');
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
    Meta: {TabList: {NORMAL_ALL: 2}},
};
// Direct eval captures only these deliberately mocked Shell dependencies.
const {Clutter, Gio, GLib, Shell, GObject, St, Extension, Main, global, Meta} = context;
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

// The Edit & upload editor (`putgd-ui --edit-image`) never registers through
// SetUiProcess: the resident started it, so its parent PID identifies it.
const editorPid = 2500;
const parents = new Map([[helperPid, residentPid], [editorPid, residentPid], [3000, 1]]);
GLib.file_get_contents = path => {
    const pid = Number(path.split('/')[2]);
    if (!parents.has(pid))
        throw new Error('No such process');
    return [true, new TextEncoder().encode(`${pid} (putgd-ui (edit)) S ${parents.get(pid)} 1 1 0`)];
};
for (const registered of [helperPid, 0]) {
    const editor = fixture();
    assert.deepEqual(await register(editor.extension, registered), {});
    context.global.display.focus_window = focused(editorPid);
    await assert.rejects(editor.extension._prepare(id(), 'window', resident), e => e.bridgeCode === 'capture_target');
    assert.equal(editor.layouts(), 0);
}
print('PASS resident-started editor PID rejection before layout, with and without a views helper');
{
    const views = fixture();
    assert.deepEqual(await register(views.extension, helperPid), {});
    context.global.display.focus_window = focused(helperPid);
    await assert.rejects(views.extension._prepare(id(), 'window', resident), e => e.bridgeCode === 'capture_target');
    const outside = id();
    context.global.display.focus_window = focused(3000);
    await views.extension._prepare(outside, 'window', resident);
    assert.equal(views.extension._requests.get(outside).window.get_pid(), 3000);
    const vanished = id();
    context.global.display.focus_window = focused(3999);
    await views.extension._prepare(vanished, 'window', resident);
    assert.equal(views.extension._requests.get(vanished).window.get_pid(), 3999);
}
delete GLib.file_get_contents;
print('PASS views helper still refused; external and unreadable processes are captured');

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

// Synthetic bridge transfer, not native compositor acceptance.
function bridge(extension, method, parameters) {
    return new Promise(resolve => extension[method](parameters, {
        get_sender: () => resident, get_parameters: () => ({deep_unpack: () => parameters}),
        return_value: value => resolve(JSON.parse(value.value[0])),
    }));
}
GLib.Bytes = class {constructor(value) {this.value = value;}};
GLib.base64_decode = () => new Uint8Array([1]);
GLib.base64_encode = () => 'AQ==';
let owned = 0;
St.ClipboardType = {CLIPBOARD: 1};
St.Clipboard = {get_default: () => ({set_content() {owned++;}})};
for (const action of ['region', 'window', 'desktop']) {
    const {extension} = fixture();
    context.global.display.focus_window = focused(3000);
    const copyId = id();
    assert.deepEqual(await bridge(extension, 'BeginCopyAsync', [copyId, 'text/plain;charset=utf-8', 1]), {});
    assert.deepEqual(await bridge(extension, 'WriteCopyAsync', [copyId, 0, 'AQ==']), {});
    await extension._prepare(id(), action, resident);
    const result = await bridge(extension, 'CommitCopyAsync', [copyId]);
    assert.equal(result.error, undefined);
    assert.equal(result.clipboardRevision, 1);
    print(`PASS ${action} preparation preserves pending result copy`);
}
assert.equal(owned, 3);
const imports = fixture().extension;
const importCopy = id();
await bridge(imports, 'BeginCopyAsync', [importCopy, 'text/plain;charset=utf-8', 1]);
await bridge(imports, 'WriteCopyAsync', [importCopy, 0, 'AQ==']);
await imports._prepare(id(), 'clipboard', resident);
assert.equal((await bridge(imports, 'CommitCopyAsync', [importCopy])).error.code, 'copy_suppressed');
print('PASS clipboard import still invalidates unsent result copy');
const deliberate = fixture().extension;
const deliberateCopy = id();
await bridge(deliberate, 'BeginCopyAsync', [deliberateCopy, 'text/plain;charset=utf-8', 1]);
await bridge(deliberate, 'WriteCopyAsync', [deliberateCopy, 0, 'AQ==']);
await bridge(deliberate, 'InvalidateCopyAsync', []);
assert.equal((await bridge(deliberate, 'CommitCopyAsync', [deliberateCopy])).error.code, 'copy_suppressed');
print('PASS explicit invalidation still prevents unsent result copy');
const changed = fixture().extension;
const changedCopy = id();
await bridge(changed, 'BeginCopyAsync', [changedCopy, 'text/plain;charset=utf-8', 1]);
await bridge(changed, 'WriteCopyAsync', [changedCopy, 0, 'AQ==']);
changed._revision++;
assert.equal((await bridge(changed, 'CommitCopyAsync', [changedCopy])).error.code, 'copy_suppressed');
print('PASS manual clipboard revision still prevents unsent result copy');

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
    get_preferred_width() {return [40, this.width >= 0 ? this.width : Math.max(40, ...this.text.split('\n').map(line => line.length * 7))];}
    get_preferred_height() {return [24, this.height >= 0 ? this.height : this.text.split('\n').length * 24];}
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
    timeout_add: (_priority, delay, callback) => {const id = ++signalId; timers.set(id, {delay, callback}); return id;},
    source_remove: id => timers.delete(id)});
let itemPid = residentPid, tooltip = ['', [], 'put.gd — Working', 'Uploading: 1 MiB of 4 MiB (25%)'];
let holdTooltip = false;
const held = [], tooltipCancellables = [];
const service = 'org.kde.StatusNotifierItem.putgd_contract';
const clientName = 'org.putgd.App.Gnome50', watcherName = 'org.kde.StatusNotifierWatcher';
const owners = new Map([[clientName, resident], [service, ':1.60'], [watcherName, ':1.70']]);
const bus = context.Gio.DBus.session;
bus.call = (destination, _path, _iface, method, args, _type, _flags, timeout, _cancel, callback) => {
    assert.equal(timeout > 0 && timeout <= 1000, true);
    const input = args?.value ?? [];
    let value;
    if (method === 'GetNameOwner') value = owners.get(input[0]);
    else if (method === 'GetConnectionUnixProcessID') value = input[0] === ':1.60' ? itemPid : residentPid;
    else if (method === 'Get') {
        const property = input[1];
        value = {IsStatusNotifierHostRegistered: true, RegisteredStatusNotifierItems: [service + '/StatusNotifierItem'],
            Id: 'putgd', Title: 'put.gd', ToolTip: tooltip}[property];
    } else throw new Error('Unexpected async method: ' + method);
    if (input[1] === 'ToolTip') tooltipCancellables.push(_cancel);
    const result = {deep_unpack: () => {if (_cancel?.cancelled) throw new Error('Synthetic cancelled read'); return [value];}};
    if (holdTooltip && input[1] === 'ToolTip') held.push(() => callback(bus, result));
    else Promise.resolve().then(() => callback(bus, result));
};
bus.call_finish = result => result;
bus.signal_subscribe = (sender, iface, signal, path, _arg, _flags, callback) => {
    const id = ++signalId; subscriptions.set(id, {sender, iface, signal, path, callback}); return id;
};
bus.signal_unsubscribe = id => subscriptions.delete(id);
async function flush() {for (let i = 0; i < 80; i++) await Promise.resolve();}
function tick(delay = null) {for (const [id, entry] of [...timers]) if ((delay === null || entry.delay === delay) && !entry.callback()) timers.delete(id);}
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

live.actor.emit('captured-event', {type: () => Clutter.EventType.BUTTON_PRESS});
tooltip = ['', [], 'put.gd — Working', 'x'.repeat(2049)]; emitTooltip(':1.60'); await flush();
tooltip = ['', [], 'put.gd — Working', 'Recovered transfer']; emitTooltip(':1.60'); await flush();
assert.equal(context.Main.uiGroup.get_children().filter(actor => actor.visible).length, 0);
live.actor.hover = false; live.actor.emit('notify::hover');
live.actor.hover = true; live.actor.emit('notify::hover'); tick(); await flush();
assert.equal(context.Main.uiGroup.get_children()[0].visible, true);
print('PASS transient tooltip failures preserve native menu click suppression');


const duplicate = new Widget({accessible_name: 'put.gd'}); context.Main.panel.add_child(duplicate);
emitTooltip(':1.60'); await flush();
assert.equal(context.Main.uiGroup.get_children().filter(actor => actor.visible).length, 0);
duplicate.destroy(); emitTooltip(':1.60'); await flush();
tooltip = ['', [], 'put.gd — Working', 'x'.repeat(2049)]; emitTooltip(':1.60'); await flush();
assert.equal(context.Main.uiGroup.get_children().filter(actor => actor.visible).length, 0);
print('PASS duplicate accessible actors and excessive tooltip text fail closed');

tooltip = ['', [], 'put.gd — Working', 'Uploading']; emitTooltip(':1.60'); await flush();
live.actor.hover = true; live.actor.emit('notify::hover'); tick(); await flush();
holdTooltip = true; emitTooltip(':1.60'); await flush();
const lockedRead = tooltipCancellables.at(-1);
context.Main.sessionMode.isLocked = true; context.Main.sessionMode.emit('updated'); await flush();
assert.equal(lockedRead.cancelled, true);
holdTooltip = false; for (const callback of held.splice(0)) callback(); await flush();
assert.equal(context.Main.uiGroup.get_children().length, 0);
assert.equal(live.actor.signals.size, 0);
context.Main.sessionMode.isLocked = false; context.Main.sessionMode.emit('updated'); await flush();
holdTooltip = true; emitTooltip(':1.60'); await flush();
assert.equal(held.length > 0, true);
const disabledRead = tooltipCancellables.at(-1);
live.companion.disable(); assert.equal(disabledRead.cancelled, true);
for (const callback of held.splice(0)) callback(); await flush();
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


itemPid = residentPid; tooltip = ['', [], 'put.gd — Share and local capture', ''];
const sizing = companionFixture(); await flush();
sizing.actor.hover = true; sizing.actor.emit('notify::hover'); tick(); await flush();
const idleSize = [context.Main.uiGroup.get_children()[0].width, context.Main.uiGroup.get_children()[0].height];
tooltip = ['', [], 'put.gd — Working', 'Uploading file 1/2: 8 MiB of 64 MiB (13%)\nPreparing file 2/2\nQueued'];
emitTooltip(':1.60'); await flush();
let label = context.Main.uiGroup.get_children()[0];
assert.equal(label.width > idleSize[0], true); assert.equal(label.height > idleSize[1], true);
tooltip = ['', [], 'put.gd — Share and local capture', '']; emitTooltip(':1.60'); await flush();
label = context.Main.uiGroup.get_children()[0];
assert.equal(label.width, idleSize[0]); assert.equal(label.height, idleSize[1]);
sizing.companion.disable(); sizing.actor.destroy();
print('PASS label dimensions grow for multiple rows and shrink back to idle');


for (const [name, replacement] of [[clientName, ':1.41'], [service, ':1.61'], [watcherName, ':1.71']]) {
    const original = owners.get(name);
    tooltip = ['', [], 'put.gd — Working', 'Current generation'];
    const changed = companionFixture(); await flush();
    changed.actor.hover = true; changed.actor.emit('notify::hover'); tick(); await flush();
    holdTooltip = true; emitTooltip(':1.60'); await flush();
    assert.equal(held.length > 0, true);
    owners.set(name, replacement);
    holdTooltip = false; for (const callback of held.splice(0)) callback(); await flush();
    assert.equal(context.Main.uiGroup.get_children().filter(actor => actor.visible).length, 0);
    changed.companion.disable(); changed.actor.destroy(); owners.set(name, original);
}
print('PASS item, resident and watcher owner changes reject delayed authenticated text');

for (const [name, replacement] of [[clientName, ':1.41'], [service, ':1.61'], [watcherName, ':1.71']]) {
    const original = owners.get(name);
    tooltip = ['', [], 'put.gd — Working', 'Old generation'];
    const replaced = companionFixture(); await flush();
    replaced.actor.hover = true; replaced.actor.emit('notify::hover'); tick(); await flush();
    holdTooltip = true; emitTooltip(':1.60'); await flush();
    const delayed = held.splice(0), oldSubscriptions = new Set(subscriptions.keys());
    assert.equal(delayed.length > 0, true);
    owners.set(name, replacement); holdTooltip = false;
    tooltip = ['', [], 'put.gd — Working', 'New generation'];
    if (name === service) {
        for (const subscription of [...subscriptions.values()])
            if (subscription.signal === 'NameOwnerChanged') subscription.callback();
    } else appear(name, replacement);
    await flush();
    for (const callback of delayed) callback(); await flush();
    assert.equal(context.Main.uiGroup.get_children().filter(actor => actor.visible).length, 1);
    assert.equal(context.Main.uiGroup.get_children()[0].text, 'put.gd — Working\nNew generation');
    assert.equal([...oldSubscriptions].some(id => subscriptions.has(id)), false);
    replaced.companion.disable(); replaced.actor.destroy(); owners.set(name, original);
}
print('PASS replacement generations survive old reads with only current subscriptions');


tooltip = ['', [], 'put.gd — Working', 'Before deadline'];
const deadline = companionFixture(); await flush();
deadline.actor.hover = true; deadline.actor.emit('notify::hover'); tick(300); await flush();
holdTooltip = true; emitTooltip(':1.60'); await flush();
const deadlineRead = tooltipCancellables.at(-1); assert.equal(deadlineRead.cancelled, undefined);
tick(4000); assert.equal(deadlineRead.cancelled, true);
holdTooltip = false; for (const callback of held.splice(0)) callback(); await flush();
assert.equal(context.Main.uiGroup.get_children().filter(actor => actor.visible).length, 0);
tooltip = ['', [], 'put.gd — Working', 'After deadline']; emitTooltip(':1.60'); await flush();
assert.equal(context.Main.uiGroup.get_children()[0].text, 'put.gd — Working\nAfter deadline');
deadline.companion.disable(); deadline.actor.destroy();
print('PASS read cancellation reaches lock, disable and the four-second deadline with later recovery');

tooltip = ['', [], 'put.gd — Working', 'Retry baseline'];
const retry = companionFixture(); await flush();
retry.actor.hover = true; retry.actor.emit('notify::hover'); tick(300); await flush();
tooltip = ['', [], 'put.gd — Working', 'x'.repeat(2049)]; emitTooltip(':1.60'); await flush();
for (let i = 0; i < 15; i++) {tick(1000); await flush();}
tooltip = ['', [], 'put.gd — Working', 'First recovered episode']; emitTooltip(':1.60'); await flush();
assert.equal(context.Main.uiGroup.get_children()[0].visible, true);
tooltip = ['', [], 'put.gd — Working', 'x'.repeat(2049)]; emitTooltip(':1.60'); await flush();
tooltip = ['', [], 'put.gd — Working', 'Second recovered episode']; tick(1000); await flush();
assert.equal(context.Main.uiGroup.get_children()[0].visible, true);
assert.equal(context.Main.uiGroup.get_children()[0].text, 'put.gd — Working\nSecond recovered episode');
retry.companion.disable(); retry.actor.destroy();
assert.equal(timers.size, 0); assert.equal(subscriptions.size, 0); assert.equal(watches.size, 0);
print('PASS each recoverable failure episode receives a fresh bounded retry budget');

// Screen recording on GNOME. Synthetic Shell objects prove the selector request,
// the bar's ownership check and placement and the frame actors; this is not
// graphical acceptance and proves nothing about the Mutter stream's pixels.
const pendingSelectors = [];
globalThis.RegionSelectorFixture = class {
    selectAsync() {return new Promise((resolve, reject) => {this.resolve = resolve; this.reject = reject; pendingSelectors.push(this);});}
    cancel() {this.resolve(null);}
};
{
    const {extension} = fixture();
    const selectId = id();
    const chosen = bridge(extension, 'SelectAreaAsync', [selectId]); await flush();
    assert.equal(pendingSelectors.length, 1);
    pendingSelectors.shift().resolve({x: 40, y: 50, width: 640, height: 360});
    assert.deepEqual(await chosen, {version: 1, requestId: selectId, rect: [40, 50, 640, 360]});
    assert.equal(extension._requests.size, 0); assert.equal(extension._selector, null);
    const cancelId = id();
    const cancelled = bridge(extension, 'SelectAreaAsync', [cancelId]); await flush();
    assert.equal((await bridge(extension, 'SelectAreaAsync', [id()])).error.code, 'busy_selecting');
    assert.deepEqual(await bridge(extension, 'ReleaseAsync', [cancelId]), {});
    assert.equal((await cancelled).error.code, 'cancelled');
    assert.equal(extension._requests.size, 0); assert.equal(extension._selector, null); assert.equal(timers.size, 0);
    print('PASS recording selection answers the rectangle and Release cancels it');
    const failedId = id();
    const failed = bridge(extension, 'SelectAreaAsync', [JSON.stringify({id: failedId, appearance: {}})]); await flush();
    const failedSelector = pendingSelectors.pop();
    failedSelector.reject(Object.assign(new Error('Synthetic revoked grab'), {bridgeCode: 'cancelled'}));
    assert.equal((await failed).error.code, 'cancelled');
    assert.equal(extension._requests.size, 0); assert.equal(extension._selector, null); assert.equal(timers.size, 0);
    const recoveredId = id();
    const recovered = bridge(extension, 'SelectAreaAsync', [JSON.stringify({id: recoveredId, appearance: {}})]); await flush();
    pendingSelectors.pop().resolve({x: 40, y: 50, width: 640, height: 360});
    assert.deepEqual(await recovered, {version: 1, requestId: recoveredId, rect: [40, 50, 640, 360]});
    assert.equal(extension._requests.size, 0); assert.equal(extension._selector, null); assert.equal(timers.size, 0);
    print('PASS failed JSON selection releases its decoded request and allows immediate recovery');
}
const work = [[0, 32, 1920, 1048]];
assert.deepEqual(globalThis.barPlacement([100, 100, 640, 360], [400, 40], work), {rect: [100, 470, 400, 40], outside: true});
assert.deepEqual(globalThis.barPlacement([100, 600, 640, 470], [400, 40], work), {rect: [100, 550, 400, 40], outside: true});
assert.deepEqual(globalThis.barPlacement([1700, 100, 200, 200], [400, 40], work), {rect: [1520, 310, 400, 40], outside: true});
assert.deepEqual(globalThis.barPlacement([0, 0, 1920, 1080], [400, 40], [...work, [1920, 0, 1280, 1024]]), {rect: [2360, 10, 400, 40], outside: true});
assert.deepEqual(globalThis.barPlacement([0, 0, 1920, 1080], [400, 40], work), {rect: [1510, 1030, 400, 40], outside: false});
// Issue #108: a monitor stacked below takes the bar, and an inside decision keeps the corner when the bar shrinks.
assert.deepEqual(globalThis.barPlacement([0, 0, 1920, 1080], [400, 40], [[0, 0, 1920, 1080], [0, 1080, 1920, 1080]]), {rect: [0, 1090, 400, 40], outside: true});
assert.deepEqual(globalThis.barPlacement([0, 0, 1920, 1040], [90, 30], work, false), {rect: [1820, 1000, 90, 30], outside: false});
assert.deepEqual(globalThis.barPlacement([0, 32, 1920, 1048], [90, 30], work), {rect: [1820, 1040, 90, 30], outside: false});
assert.deepEqual(globalThis.subtract([[0, 0, 100, 100]], [20, 30, 50, 40]), [[0, 0, 100, 30], [0, 70, 100, 30], [0, 30, 20, 40], [70, 30, 30, 40]]);
print('PASS recording bar placement prefers below, above, another monitor, then inside');
{
    const {extension} = fixture();
    const overlap = (a, b) => a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
    let moved = null, above = false, parent = residentPid;
    // The first move is undone once, as Mutter's initial placement does; the extension re-applies it.
    let hidden = 2, undo = 1;
    let size = [400, 40], resized = null, disconnected = 0;
    const bar = {get_title: () => 'put.gd recording controls', get_pid: () => helperPid,
        get_frame_rect: () => ({x: moved ? moved[0] : 700, y: moved ? moved[1] : 400, width: size[0], height: size[1]}),
        is_hidden: () => hidden-- > 0, is_above: () => above, make_above() {above = true;},
        move_frame(_user, x, y) {moved = undo-- > 0 ? [700, 400] : [x, y];},
        connect(signal, handler) {assert.equal(signal, 'size-changed'); resized = handler; return 7;},
        disconnect(id) {disconnected = id;}};
    const settle = async promise => {for (let i = 0; i < 20; i++) {await flush(); tick(50); tick(100);} return promise;};
    context.GLib.file_get_contents = () => [true, new TextEncoder().encode(`${helperPid} (putgd record) S ${parent} 1 1 0`)];
    context.global.get_window_actors = () => [{meta_window: bar}];
    context.global.stage = {width: 1920, height: 1080};
    context.Main.layoutManager = {monitors: [{x: 0, y: 0, width: 1920, height: 1080}], getWorkAreaForMonitor: () => ({x: 0, y: 32, width: 1920, height: 1048})};
    const before = context.Main.uiGroup.get_children().length;
    const area = [600, 300, 640, 360];
    const frame = state => JSON.stringify({rect: area, kind: 'region', state, pid: helperPid});
    assert.deepEqual(await settle(bridge(extension, 'ShowRecordingFrameAsync', [frame('ready')])), {version: 1, bar: {placed: true, outside: true, rect: [600, 670, 400, 40]}});
    assert.equal(above, true);
    assert.equal(context.Main.uiGroup.get_children().length - before, 8);
    await settle(bridge(extension, 'ShowRecordingFrameAsync', [frame('recording')]));
    const actors = context.Main.uiGroup.get_children().slice(before);
    // Ring (8) and dim strips (7: the bar is cut out of the bottom strip).
    assert.equal(actors.length, 15);
    assert.equal(actors.every(a => a.reactive === false && !overlap([a.x, a.y, a.width, a.height], area) &&
        !overlap([a.x, a.y, a.width, a.height], [600, 670, 400, 40])), true);
    // Record widens the bar: the Shell places it again from its right edge's monitor, still below the area.
    size = [700, 40]; resized();
    assert.deepEqual(moved, [600, 670]);
    size = [1400, 40]; resized();
    assert.deepEqual(moved, [520, 670]);
    assert.equal(context.Main.uiGroup.get_children().slice(before).every(a => !overlap([a.x, a.y, a.width, a.height], [520, 670, 1400, 40])), true);
    size = [400, 40];
    // A whole monitor: the bar stays in the corner, and its handle goes to the corner too.
    const whole = [0, 32, 1920, 1048];
    const full = state => JSON.stringify({rect: whole, kind: 'monitor', state, pid: helperPid});
    assert.deepEqual((await settle(bridge(extension, 'ShowRecordingFrameAsync', [full('ready')]))).bar, {placed: true, outside: false, rect: [1510, 1030, 400, 40]});
    size = [90, 30]; resized();
    assert.deepEqual(moved, [1820, 1040]);
    assert.deepEqual((await settle(bridge(extension, 'ShowRecordingFrameAsync', [full('recording')]))).bar, {placed: true, outside: false, rect: [1820, 1040, 90, 30]});
    size = [400, 40];
    // A bar whose process is not the resident's child is never moved.
    parent = 4321; moved = null;
    const foreign = bridge(extension, 'ShowRecordingFrameAsync', [frame('ready')]);
    for (let i = 0; i < 41; i++) {await flush(); tick(50);}
    assert.deepEqual((await foreign).bar, {placed: false, outside: false}); assert.equal(moved, null);
    assert.equal((await bridge(extension, 'ShowRecordingFrameAsync', [JSON.stringify({rect: [0, 0, 0, 10], kind: 'region', state: 'ready'})])).error.code, 'bridge_protocol');
    assert.equal((await bridge(extension, 'ShowRecordingFrameAsync', [JSON.stringify({rect: area, kind: 'region', state: 'exporting'})])).error.code, 'bridge_protocol');
    assert.deepEqual(await bridge(extension, 'HideRecordingFrameAsync', []), {});
    assert.equal(context.Main.uiGroup.get_children().length, before); assert.equal(timers.size, 0);
    assert.equal(disconnected, 7); assert.equal(extension._barFollow, null);
    print('PASS recording frame places only the resident child\'s bar, follows its size and keeps every actor outside the area');
}
{
    // #924: a selection hotkey closes put.gd's own open tray menu first; desktop
    // and window captures keep it, and another application's menu stays open.
    const session = context.Gio.DBus.session, original = session.call_sync;
    const pids = new Map([[resident, residentPid], [':1.60', residentPid], [':1.70', 4242]]);
    session.call_sync = (_service, _path, _iface, method, parameters) => {
        if (method === 'GetNameOwner')
            return {deep_unpack: () => [resident]};
        const name = parameters.value[0];
        if (!pids.has(name))
            throw new Error('No such name');
        return {deep_unpack: () => [pids.get(name)]};
    };
    const menu = busName => ({isOpen: true, closed: [], sourceActor: {_indicator: {busName}},
        close(animation) {this.closed.push(animation); this.isOpen = false;}});
    const {extension} = fixture();
    extension._dbus = {emit_signal() {}};
    let uuid = 0;
    GLib.uuid_string_random ??= () => `00000000-0000-0000-0001-${String(++uuid).padStart(12, '0')}`;
    for (const action of ['region', 'edit', 'record', 'scroll']) {
        const own = menu(':1.60');
        context.Main.panel.menuManager = {activeMenu: own};
        await extension._hotkey(action);
        assert.deepEqual(own.closed, [0]);
    }
    for (const [action, busName] of [['desktop', ':1.60'], ['region', ':1.70'], ['region', ':1.80'], ['region', undefined]]) {
        const other = menu(busName);
        context.Main.panel.menuManager = {activeMenu: other};
        await extension._hotkey(action);
        assert.deepEqual(other.closed, []);
    }
    const shut = menu(':1.60'); shut.isOpen = false;
    context.Main.panel.menuManager = {activeMenu: shut};
    assert.equal(extension._closeTrayMenu(resident), false); assert.deepEqual(shut.closed, []);
    context.Main.panel.menuManager = {activeMenu: null};
    assert.equal(extension._closeTrayMenu(resident), false);
    delete context.Main.panel.menuManager; session.call_sync = original;
    for (const id of extension._requests.keys()) extension._drop(id);
    print('PASS selection hotkeys close only the resident\'s own open tray menu');
}
// A live region selection can show at the press (Preview) and be taken up by
// Acquire once the journal is durable. A resident that predates Preview only
// calls Acquire, which selects as it always did. Synthetic selector and streams.
{
    const layout = [{logical: [0, 0, 640, 480], density: 1}];
    const bounds = JSON.stringify({imageMimes: ['image/png'], textBytes: 1, imageBytes: 8388608, imagePixels: 2097152, maxFiles: 1});
    let pixelReads = 0;
    context.Gio.MemoryOutputStream = {new_resizable() {pixelReads++; throw Object.assign(new Error('Synthetic stop'), {bridgeCode: 'capture_failed'});}};
    const prepared = async () => {
        const {extension} = fixture();
        extension._layout = async () => layout;
        const request = id();
        await extension._prepare(request, 'region', resident);
        return {extension, request};
    };
    pendingSelectors.length = 0;
    {
        const {extension, request} = await prepared();
        globalThis.Config = {PACKAGE_VERSION: '50.0'};
        extension.metadata = {version: 18};
        assert.equal((await bridge(extension, 'HelloAsync', [])).previewVersion, 1);
        assert.deepEqual(await bridge(extension, 'PreviewAsync', [request, JSON.stringify({outcome: 'Saves locally', appearance: {}})]), {version: 1, requestId: request, shown: true});
        assert.equal(pendingSelectors.length, 1);
        // The user finishes before the journal does: geometry waits, no pixels are read.
        pendingSelectors.shift().resolve({x: 40, y: 50, width: 64, height: 36});
        await flush();
        assert.equal(pixelReads, 0); assert.equal(extension._selector, null);
        assert.equal((await bridge(extension, 'AcquireAsync', [request, bounds])).error.code, 'capture_failed');
        assert.equal(pixelReads, 1); assert.equal(pendingSelectors.length, 0);
        print('PASS Preview shows the selector and only Acquire reads pixels');
    }
    {
        pixelReads = 0;
        const {extension, request} = await prepared();
        const acquired = bridge(extension, 'AcquireAsync', [request, bounds]); await flush();
        assert.equal(pendingSelectors.length, 1);
        pendingSelectors.shift().resolve({x: 40, y: 50, width: 64, height: 36});
        assert.equal((await acquired).error.code, 'capture_failed');
        assert.equal(pixelReads, 1);
        print('PASS a resident without Preview still selects through Acquire');
    }
    {
        pixelReads = 0;
        const {extension, request} = await prepared();
        await bridge(extension, 'PreviewAsync', [request, '{}']);
        pendingSelectors.shift().cancel();
        await flush();
        assert.equal((await bridge(extension, 'AcquireAsync', [request, bounds])).error.code, 'cancelled');
        assert.equal(pixelReads, 0);
        print('PASS cancelling a previewed selection cancels the acquisition');
    }
    {
        const {extension, request} = await prepared();
        extension._selector = {};
        assert.equal((await bridge(extension, 'PreviewAsync', [request, '{}'])).shown, false);
        assert.equal((await bridge(extension, 'AcquireAsync', [request, bounds])).error.code, 'busy_selecting');
        print('PASS a busy selector leaves Preview unshown and Acquire reports it');
    }
    {
        const {extension, request} = await prepared();
        extension._requests.get(request).error = Object.assign(new Error('Synthetic layout failure'), {bridgeCode: 'capture_geometry'});
        assert.equal((await bridge(extension, 'PreviewAsync', [request, '{}'])).shown, false);
        assert.equal(pendingSelectors.length, 0);
        assert.equal((await bridge(extension, 'AcquireAsync', [request, bounds])).error.code, 'capture_geometry');
        print('PASS Preview leaves a failed request for Acquire to report');
    }
    delete context.Gio.MemoryOutputStream;
}
{
    // Recording protocol 2: the windows a window recording can follow (Mutter's
    // window ids), the focused one active, put.gd's own never listed.
    const {extension} = fixture();
    const window = (pid, id, title, minimized = false) => ({get_pid: () => pid, get_id: () => id, get_title: () => title,
        get_wm_class: () => title.toLowerCase(), minimized, get_frame_rect: () => ({x: id, y: 2, width: 300, height: 200})});
    const notes = window(3000, 42, 'Notes'), video = window(3000, 43, 'Video', true), dashboard = window(residentPid, 7, 'put.gd');
    context.global.display.get_tab_list = type => type === context.Meta.TabList.NORMAL_ALL ? [dashboard, notes, video] : [];
    context.Shell.WindowTracker = {get_default: () => ({get_window_app: w => w === notes ? {get_id: () => 'org.example.Notes.desktop'} : null})};
    context.global.display.focus_window = notes;
    assert.deepEqual(await bridge(extension, 'RecordingWindowsAsync', []), {version: 1, windows: [
        {identity: '42', title: 'Notes', appId: 'org.example.Notes.desktop', rect: [42, 2, 300, 200], active: true, minimized: false},
        {identity: '43', title: 'Video', appId: 'video', rect: [43, 2, 300, 200], active: false, minimized: true}], activeExcluded: false});
    context.global.display.focus_window = dashboard;
    const own = await bridge(extension, 'RecordingWindowsAsync', []);
    assert.equal(own.activeExcluded, true);
    assert.deepEqual(own.windows.map(w => [w.identity, w.active]), [['42', false], ['43', false]]);
    globalThis.Config = {PACKAGE_VERSION: '50.0'};
    extension.metadata = {version: 22};
    assert.equal((await bridge(extension, 'HelloAsync', [])).recordingVersion, 3);
    print('PASS RecordingWindows lists external windows, marks the focused one and leaves out put.gd\'s own');
}
{
    // Recording protocol 3 (#1364): presses for click highlights. A press over a
    // client window is read from Clutter's current event when Mutter stamps the
    // window's user time; a key press stamps it too and is not a click. Presses
    // on the Shell's own actors come from the stage. The signal goes to the
    // resident's own bus name, and nothing stays connected after Unwatch.
    const {extension} = fixture();
    const sent = [];
    context.Gio.DBus.session.emit_signal = (destination, path, iface, name, variant) => sent.push({destination, path, iface, name, click: JSON.parse(variant.value[0])});
    const connectable = extra => {
        const signals = new Map();
        let next = 1;
        return Object.assign(extra, {
            connect(name, handler) { signals.set(next, [name, handler]); return next++; },
            disconnect(id) { signals.delete(id); },
            emit(name, ...args) { let result; for (const [signal, handler] of [...signals.values()]) if (signal === name) result = handler(...args); return result; },
            connected: () => signals.size,
        });
    };
    const window = (id, x) => connectable({get_id: () => id, get_frame_rect: () => ({x, y: 30, width: 800, height: 600})});
    const editor = window(42, 100), late = window(43, 500);
    connectable(context.global.display);
    context.global.stage = connectable({});
    context.global.get_window_actors = () => [{meta_window: editor}];
    let current = null;
    context.Clutter.get_current_event = () => current;
    context.Clutter.EventType.KEY_PRESS = 2;
    const press = (button, x, y, time) => ({type: () => context.Clutter.EventType.BUTTON_PRESS, get_button: () => button, get_coords: () => [x, y], get_time: () => time});
    assert.deepEqual(await bridge(extension, 'WatchClicksAsync', []), {version: 1});
    assert.equal(editor.connected(), 2);
    assert.equal(context.global.display.connected(), 1);
    assert.equal(context.global.stage.connected(), 1);
    current = press(1, 640.4, 360.6, 1000);
    editor.emit('notify::user-time');
    assert.deepEqual(sent, [{destination: resident, path: '/org/gnome/Shell/Extensions/Putgd', iface: 'org.putgd.Gnome50', name: 'Click',
        click: {x: 640, y: 361, button: 1, window: {identity: '42', rect: [100, 30, 800, 600]}}}]);
    // A key press stamps the user time as well; so does a stamp outside any event.
    current = {type: () => context.Clutter.EventType.KEY_PRESS};
    editor.emit('notify::user-time');
    current = null;
    editor.emit('notify::user-time');
    assert.equal(sent.length, 1);
    // A window opened during the recording is watched from then on.
    context.global.display.emit('window-created', context.global.display, late);
    current = press(3, 700, 200, 1100);
    late.emit('notify::user-time');
    assert.deepEqual(sent[1].click, {x: 700, y: 200, button: 3, window: {identity: '43', rect: [500, 30, 800, 600]}});
    // The Shell's own actors (the panel, the overview): no window, and the event still propagates.
    current = null;
    assert.equal(context.global.stage.emit('captured-event', context.global.stage, press(1, 30, 12, 1200)), context.Clutter.EVENT_PROPAGATE);
    assert.deepEqual(sent[2].click, {x: 30, y: 12, button: 1});
    // One press seen by both hooks is one click; the wheel and other buttons are none.
    current = press(2, 300, 300, 1300);
    context.global.stage.emit('captured-event', context.global.stage, current);
    editor.emit('notify::user-time');
    assert.equal(sent.length, 4);
    assert.equal(sent[3].click.button, 2);
    current = press(4, 300, 300, 1400);
    editor.emit('notify::user-time');
    current = press(0, 300, 300, 1500);
    editor.emit('notify::user-time');
    assert.equal(sent.length, 4);
    // A closed window lets go of its handlers; Unwatch lets go of the rest.
    editor.emit('unmanaged');
    assert.equal(editor.connected(), 0);
    assert.deepEqual(await bridge(extension, 'UnwatchClicksAsync', []), {});
    assert.equal(late.connected(), 0);
    assert.equal(context.global.display.connected(), 0);
    assert.equal(context.global.stage.connected(), 0);
    assert.equal(extension._clicks, null);
    // Watching again replaces the earlier watch, and the lock and disable paths end it.
    await bridge(extension, 'WatchClicksAsync', []);
    await bridge(extension, 'WatchClicksAsync', []);
    assert.equal(context.global.stage.connected(), 1);
    extension._unwatchClicks();
    assert.equal(context.global.stage.connected(), 0);
    extension._unwatchClicks();
    delete context.Gio.DBus.session.emit_signal;
    print('PASS WatchClicks reports presses over windows and Shell actors to the resident only, and Unwatch disconnects everything');
}
// Capture version 2 (#1363): Capture monitor, Repeat last region and the capture
// countdown. Synthetic Shell: which monitors are read, what is refused and when
// the pill is on stage. Not graphical acceptance.
{
    const layout = [{identity: 'DP-1', logical: [0, 0, 1920, 1080], density: 1, rotation: 0},
        {identity: 'DP-2', logical: [1920, 0, 2560, 1440], density: 1, rotation: 0}];
    const tokens = {surfaceRaised: '#2a2a2a', text: '#f0f0ec', borderStrong: '#737373'};
    const bounds = (extra = {}) => JSON.stringify({imageMimes: ['image/png'], textBytes: 1, imageBytes: 67108864, imagePixels: 16777216, maxFiles: 1, appearance: {tokens}, ...extra});
    const areas = [], chrome = [];
    const originalScreenshot = context.Shell.Screenshot, originalLayout = context.Main.layoutManager;
    context.Gio.MemoryOutputStream = {new_resizable: () => ({})};
    context.Shell.Screenshot = class {screenshot_area(x, y, width, height) {areas.push([x, y, width, height]); throw Object.assign(new Error('Synthetic stop'), {bridgeCode: 'capture_failed'});}};
    context.Main.layoutManager = {monitors: [{index: 0, x: 0, y: 0, width: 1920, height: 1080}, {index: 1, x: 1920, y: 0, width: 2560, height: 1440}], primaryIndex: 0,
        // As GNOME Shell 50's Params.parse: an unknown chrome parameter throws.
        addTopChrome: (actor, params = {}) => {
            for (const name of Object.keys(params)) {
                if (!['trackFullscreen', 'affectsStruts'].includes(name))
                    throw new Error(`Unrecognized parameter "${name}"`);
            }
            chrome.push(actor);
        },
        removeChrome: actor => chrome.splice(chrome.indexOf(actor), 1)};
    context.Main.panel.height = 32;
    globalThis.Config = {PACKAGE_VERSION: '50.0'};
    const prepared = async action => {
        const {extension} = fixture();
        extension._layout = async () => layout;
        const request = id();
        await extension._prepare(request, action, resident);
        return {extension, request};
    };
    {
        const {extension} = fixture();
        extension.metadata = {version: 22};
        assert.equal((await bridge(extension, 'HelloAsync', [])).captureVersion, 2);
    }
    {
        context.global.display.get_current_monitor = () => 1;
        const {extension, request} = await prepared('monitor');
        // The pointer moves on afterwards; the monitor was frozen at Prepare.
        context.global.display.get_current_monitor = () => 0;
        assert.equal((await bridge(extension, 'AcquireAsync', [request, bounds()])).error.code, 'capture_failed');
        assert.deepEqual(areas, [[1920, 0, 2560, 1440]]);
        context.global.display.get_current_monitor = () => 7;
        await assert.rejects(prepared('monitor'), error => error.bridgeCode === 'capture_target');
        print('PASS Capture monitor reads only the monitor the pointer was on at Prepare');
    }
    {
        areas.length = 0;
        const key = fixture().extension._layoutKey(layout);
        const area = [2000, 100, 300, 150];
        const {extension, request} = await prepared('repeat');
        assert.equal((await bridge(extension, 'AcquireAsync', [request, bounds({area, layoutKey: key})])).error.code, 'capture_failed');
        assert.deepEqual(areas, [[1920, 0, 2560, 1440]]);
        const movedKey = fixture().extension._layoutKey([layout[0], {...layout[1], logical: [1920, 200, 2560, 1440]}]);
        const moved = await prepared('repeat');
        assert.equal((await bridge(moved.extension, 'AcquireAsync', [moved.request, bounds({area, layoutKey: movedKey})])).error.code, 'repeat_layout_changed');
        const rescaled = await prepared('repeat');
        assert.equal((await bridge(rescaled.extension, 'AcquireAsync', [rescaled.request, bounds({area, layoutKey: fixture().extension._layoutKey([layout[0], {...layout[1], density: 1.5}])})])).error.code, 'repeat_layout_changed');
        const none = await prepared('repeat');
        assert.equal((await bridge(none.extension, 'AcquireAsync', [none.request, bounds()])).error.code, 'repeat_unavailable');
        const outside = await prepared('repeat');
        assert.equal((await bridge(outside.extension, 'AcquireAsync', [outside.request, bounds({area: [5000, 0, 10, 10], layoutKey: key})])).error.code, 'capture_geometry');
        const malformed = await prepared('repeat');
        assert.equal((await bridge(malformed.extension, 'AcquireAsync', [malformed.request, bounds({area: [0, 0, -5, 10], layoutKey: key})])).error.code, 'bridge_protocol');
        assert.equal(areas.length, 1);
        print('PASS Repeat last region reads the monitors its rectangle lies on and refuses a changed layout');
    }
    {
        context.global.display.get_current_monitor = () => 1;
        const {extension} = fixture();
        const first = id();
        const counting = bridge(extension, 'CountdownAsync', [first, JSON.stringify({seconds: 3, appearance: {tokens}})]); await flush();
        assert.equal(chrome.length, 1);
        assert.equal(chrome[0].text, 'Capturing in 3 · Click to cancel');
        // Top centre of the pointer's monitor, which has no top bar.
        assert.equal(chrome[0].y, 24);
        assert.equal(chrome[0].x, Math.round(1920 + (2560 - chrome[0].get_preferred_width()[1]) / 2));
        tick(1000);
        assert.equal(chrome[0].text, 'Capturing in 2 · Click to cancel');
        tick(1000); tick(1000);
        assert.deepEqual(await counting, {version: 1, requestId: first});
        assert.equal(chrome.length, 0); assert.equal(extension._requests.has(first), false);
        print('PASS the countdown shows on the pointer\'s monitor and its actor is gone before it answers');
        // Below the top bar on the monitor that has it.
        context.global.display.get_current_monitor = () => 0;
        const barred = id();
        const underBar = bridge(extension, 'CountdownAsync', [barred, JSON.stringify({seconds: 3, appearance: {tokens}})]); await flush();
        assert.equal(chrome[0].y, 56);
        chrome[0].emit('button-release-event');
        assert.equal((await underBar).error.code, 'cancelled');
        assert.equal(chrome.length, 0); assert.equal(extension._requests.has(barred), false);
        print('PASS a click on the countdown cancels it');
        const released = id();
        const waiting = bridge(extension, 'CountdownAsync', [released, JSON.stringify({seconds: 10, appearance: {tokens}})]); await flush();
        assert.deepEqual(await bridge(extension, 'ReleaseAsync', [released]), {});
        assert.equal((await waiting).error.code, 'cancelled'); assert.equal(chrome.length, 0);
        print('PASS Release removes a running countdown');
        // A delay that ends on a locked screen captures nothing.
        const locked = id();
        const ending = bridge(extension, 'CountdownAsync', [locked, JSON.stringify({seconds: 3, appearance: {tokens}})]); await flush();
        tick(1000); tick(1000);
        context.Main.sessionMode.isLocked = true;
        tick(1000);
        assert.equal((await ending).error.code, 'session_locked');
        context.Main.sessionMode.isLocked = false;
        assert.equal(chrome.length, 0);
        assert.equal((await bridge(extension, 'CountdownAsync', [id(), JSON.stringify({seconds: 4})])).error.code, 'bridge_protocol');
        print('PASS a countdown that ends on a locked screen answers session_locked');
        // The Shell disables extensions when it locks, and may do so before the
        // extension's own session handler runs: still the lock, not a broken helper.
        const disabledByLock = id();
        const lockedOut = bridge(extension, 'CountdownAsync', [disabledByLock, JSON.stringify({seconds: 10, appearance: {tokens}})]); await flush();
        assert.equal(chrome.length, 1);
        // What disable() releases besides the requests.
        Object.assign(extension, {_bindings: new Map(), _dbus: {emit_signal() {}, unexport() {}}});
        context.Main.sessionMode.isLocked = true;
        extension.disable();
        assert.equal((await lockedOut).error.code, 'session_locked'); assert.equal(chrome.length, 0);
        context.Main.sessionMode.isLocked = false;
        print('PASS a lock that disables the extension mid-countdown answers session_locked');
    }
    {
        // A live region: area first, then the countdown, then the pixels.
        pendingSelectors.length = 0; areas.length = 0;
        const {extension, request} = await prepared('region');
        const acquired = bridge(extension, 'AcquireAsync', [request, bounds({delaySeconds: 3})]); await flush();
        assert.equal(pendingSelectors.length, 1); assert.equal(chrome.length, 0);
        pendingSelectors.shift().resolve({x: 2000, y: 100, width: 64, height: 36}); await flush();
        assert.equal(areas.length, 0); assert.equal(chrome.length, 1);
        // On the monitor the selection is on.
        assert.equal(chrome[0].x, Math.round(1920 + (2560 - chrome[0].get_preferred_width()[1]) / 2));
        tick(1000); tick(1000); await flush();
        assert.equal(areas.length, 0);
        tick(1000); await flush();
        assert.equal((await acquired).error.code, 'capture_failed');
        assert.equal(chrome.length, 0); assert.deepEqual(areas, [[0, 0, 1920, 1080]]);
        const cancelled = await prepared('region');
        const dropped = bridge(cancelled.extension, 'AcquireAsync', [cancelled.request, bounds({delaySeconds: 5})]); await flush();
        pendingSelectors.shift().resolve({x: 40, y: 50, width: 64, height: 36}); await flush();
        chrome[0].emit('button-release-event');
        assert.equal((await dropped).error.code, 'cancelled');
        assert.equal(areas.length, 1); assert.equal(chrome.length, 0);
        assert.equal((await bridge(extension, 'AcquireAsync', [id(), bounds({delaySeconds: 4})])).error.code, 'bridge_protocol');
        print('PASS a delayed live region counts down after the selection and before any pixel');
    }
    context.Shell.Screenshot = originalScreenshot; context.Main.layoutManager = originalLayout;
    delete context.Gio.MemoryOutputStream;
}
