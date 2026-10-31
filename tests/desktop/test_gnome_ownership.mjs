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
    selectAsync() {return new Promise(resolve => {this.resolve = resolve; pendingSelectors.push(this);});}
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
    for (const action of ['region', 'edit', 'record']) {
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
