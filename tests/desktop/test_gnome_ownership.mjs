// Native Shell APIs are fixtures here: this suite proves extension control and
// frozen-target behavior, never graphical acceptance or compositor permission.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(new URL('../../integrations/gnome/extension.js', import.meta.url), 'utf8')
    .replace(/^import .*;$/gm, '')
    .replace('export default class PutgdExtension', 'globalThis.PutgdExtension = class PutgdExtension');
const resident = ':1.50';
const residentPid = 1000;
const helperPid = 2000;
const context = vm.createContext({
    Gio: {_promisify() {}, Cancellable: class {cancel() {}}, DBusCallFlags: {NONE: 0},
        DBus: {session: {call_sync(_service, _path, _iface, method) {
            return {deep_unpack: () => [method === 'GetNameOwner' ? resident : residentPid]};
        }}}},
    GLib: {Variant: class {constructor(_type, value) {this.value = value;}}, VariantType: class {},
        timeout_add: () => 1, source_remove() {}, PRIORITY_DEFAULT: 0},
    Shell: {Screenshot: class {}}, GObject: {registerClass: cls => cls}, St: {Widget: class {}},
    Extension: class {}, Main: {sessionMode: {isLocked: false}},
    global: {display: {focus_window: null}},
});
vm.runInContext(source, context);
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
console.log('PASS resident PID rejection before layout');

const helper = fixture();
assert.deepEqual(await register(helper.extension, helperPid), {});
context.global.display.focus_window = focused(helperPid);
await assert.rejects(helper.extension._prepare(id(), 'window', resident), e => e.bridgeCode === 'capture_target');
assert.equal(helper.layouts(), 0);
console.log('PASS registered helper PID rejection before layout');

assert.equal((await register(helper.extension, 3000, ':1.99')).error.code, 'bridge_unauthorized');
await assert.rejects(helper.extension._prepare(id(), 'window', resident), e => e.bridgeCode === 'capture_target');
console.log('PASS foreign caller cannot replace helper identity');

assert.deepEqual(await register(helper.extension, 0), {});
const afterExit = id();
await helper.extension._prepare(afterExit, 'window', resident);
assert.equal(helper.extension._requests.get(afterExit).window.get_pid(), helperPid);
console.log('PASS helper exit clears exclusion');

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
console.log('PASS helper registration/focus changes do not replace frozen target');
