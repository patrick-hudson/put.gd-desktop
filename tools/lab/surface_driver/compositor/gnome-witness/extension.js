// Lab-only compositor witness for the put.gd surface driver (disposable guests).
// It reports Mutter's own window geometry, focus and stacking, and the stage
// hit-test at a point. Activate is the driver's explicit
// focus(raise_window=True) on Wayland: the same activation a dock click does.
// HideOverview dismisses the login overview during disposable guest setup.
// It never moves, resizes or closes a window.
import Gio from 'gi://Gio';
import Atk from 'gi://Atk';
import GLib from 'gi://GLib';
import Clutter from 'gi://Clutter';
import Meta from 'gi://Meta';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const PATH = '/org/putgd/LabWitness';
const XML = `<node><interface name="org.putgd.LabWitness">
<method name="Grabs"><arg type="s" direction="out"/></method>
<method name="Windows"><arg type="s" direction="out"/></method>
<method name="Desktop"><arg type="s" direction="out"/></method>
<method name="HideOverview"><arg type="b" direction="out"/></method>
<method name="Pick"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="s" direction="out"/></method>
<method name="Selector"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
<method name="Activate"><arg type="t" direction="in"/><arg type="b" direction="out"/></method>
</interface></node>`;

const rect = r => [r.x, r.y, r.width, r.height];
// GNOME 48+ moved the stacking-ordered (bottom to top) actor list to the compositor.
const windowActors = () => global.compositor?.get_window_actors?.() ?? global.get_window_actors();

function describe(window, stack) {
    const transient = window.get_transient_for();
    return {
        id: window.get_id(), title: window.get_title() ?? '', pid: window.get_pid(),
        wm_class: window.get_wm_class() ?? '', type: window.get_window_type(),
        frame: rect(window.get_frame_rect()), buffer: rect(window.get_buffer_rect()),
        focus: window.has_focus(), minimized: window.minimized, stack,
        client: window.get_client_type() === Meta.WindowClientType.WAYLAND ? 'wayland' : 'x11',
        monitor: window.get_monitor(), transient_for: transient ? transient.get_id() : null,
    };
}

function actorDescription(actor) {
    const [x, y] = actor.get_transformed_position();
    const [w, h] = actor.get_transformed_size();
    // Actor properties are overrides. StLabel/StButton supply their default
    // semantic role and name on the actual ATK object exported to AT-SPI.
    const accessible = actor.get_accessible?.();
    return {type: actor.constructor?.name ?? '', name: actor.get_name?.() ?? '',
        // Atk.Action also has get_name(index). GJS can resolve that interface
        // method on buttons, so call the Object methods explicitly.
        style: actor.style_class ?? '', accessible: accessible ? Atk.Object.prototype.get_name.call(accessible) ?? '' : '',
        text: actor.get_text?.() ?? '', role: accessible ? Number(Atk.Object.prototype.get_role.call(accessible)) : -1,
        rect: [x, y, w, h].map(Math.round)};
}

function pickedShellNodes(picked, x, y) {
    // Only the actual input actor's descendants qualify. Walking the whole
    // stage could find an old menu behind the actor currently covering it.
    if (!picked || picked === global.stage)
        return [];
    const pending = [picked], nodes = [];
    for (let count = 0; pending.length && count < 512; count++) {
        const actor = pending.pop();
        if (!actor.visible || !actor.mapped)
            continue;
        const description = actorDescription(actor);
        const [left, top, w, h] = description.rect;
        if (!(left <= x && x < left + w && top <= y && y < top + h))
            continue;
        nodes.push(description);
        pending.push(...actor.get_children());
    }
    return nodes;
}

export default class LabWitness extends Extension {
    enable() {
        this._epoch = GLib.uuid_string_random();
        this._serial = 0;
        this._grabs = [];
        this._signals = ['grab-op-begin', 'grab-op-end'].map(signal =>
            global.display.connect(signal, (_display, window, operation) => {
                const name = Object.keys(Meta.GrabOp).find(key =>
                    /^(RESIZING|MOVING)/.test(key) && Meta.GrabOp[key] === operation) ?? String(operation);
                this._grabs.push({serial: ++this._serial, window_id: window?.get_id() ?? null,
                    pid: window?.get_pid() ?? null, operation: name,
                    kind: name.startsWith('RESIZING_') ? 'resize' : name.startsWith('MOVING') ? 'move' : 'other',
                    phase: signal === 'grab-op-begin' ? 'begin' : 'end',
                    monotonic_us: GLib.get_monotonic_time()});
                this._grabs = this._grabs.slice(-256);
            }));
        this._dbus = Gio.DBusExportedObject.wrapJSObject(XML, this);
        this._dbus.export(Gio.DBus.session, PATH);
    }

    disable() {
        for (const id of this._signals ?? [])
            global.display.disconnect(id);
        this._signals = [];
        this._grabs = [];
        this._dbus?.unexport();
        this._dbus = null;
    }

    Grabs() {
        return JSON.stringify({epoch: this._epoch, serial: this._serial, events: this._grabs});
    }

    Windows() {
        const display = global.display;
        const monitors = [];
        for (let i = 0; i < display.get_n_monitors(); i++)
            monitors.push({rect: rect(display.get_monitor_geometry(i)), scale: display.get_monitor_scale(i)});
        const windows = windowActors().map((actor, i) => describe(actor.meta_window, i));
        return JSON.stringify({windows, monitors});
    }

    Desktop() {
        return JSON.stringify({overviewVisible: Main.overview.visible,
            overviewAnimating: Boolean(Main.overview.animationInProgress),
            locked: Main.sessionMode.isLocked, witnessVersion: this.metadata.version,
            nativeHelperEnabled: Boolean(Main.extensionManager.lookup('putgd@put.gd')?.stateObj?._enabled)});
    }

    HideOverview() {
        if (Main.sessionMode.isLocked)
            return false;
        Main.overview.hide();
        return true;
    }

    Selector(query) {
        // Shell selectors are actors, not MetaWindows. Bind the exact actor
        // reference held by the production extension to its live request and
        // authenticated bridge sender; a same-named shell actor cannot pass.
        const extension = Main.extensionManager.lookup('putgd@put.gd')?.stateObj;
        const selector = extension?._selector;
        if (!extension?._enabled || Main.sessionMode.isLocked || !selector?.mapped ||
            !selector.visible || selector._closed || !selector._grab)
            return '{}';
        const requests = [...extension._requests.values()].filter(r =>
            r.selector === selector && r.stage === 'region-select' && r.action === 'region');
        if (requests.length !== 1)
            return '{}';
        const call = (method, signature, args, reply) => Gio.DBus.session.call_sync(
            'org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', method,
            new GLib.Variant(signature, args), new GLib.VariantType(reply), Gio.DBusCallFlags.NONE, 1000, null).deep_unpack()[0];
        const request = requests[0];
        if (request.sender !== call('GetNameOwner', '(s)', ['org.putgd.App.Gnome50'], '(s)'))
            return '{}';
        const point = JSON.parse(query).point;
        const picked = point ? global.stage.get_actor_at_pos(Clutter.PickMode.REACTIVE, ...point) : null;
        const inside = actor => actor === selector || (actor && selector.contains(actor));
        return JSON.stringify({request_id: request.id, client_pid: call('GetConnectionUnixProcessID', '(s)', [request.sender], '(u)'),
            name: actorDescription(selector).accessible, rect: actorDescription(selector).rect,
            pointer: !!picked && inside(picked), keyboard: !!inside(global.stage.get_key_focus())});
    }

    Activate(id) {
        const actor = windowActors().find(a => a.meta_window.get_id() === Number(id));
        if (!actor)
            return false;
        actor.meta_window.activate(global.get_current_time());
        return true;
    }

    Pick(x, y) {
        const index = new Map(windowActors().map((actor, i) => [actor, i]));
        const pid = Number(GLib.file_read_link('/proc/self'));
        for (const mode of [Clutter.PickMode.REACTIVE, Clutter.PickMode.ALL]) {
            const chain = [];
            const picked = global.stage.get_actor_at_pos(mode, x, y);
            for (let actor = picked; actor; actor = actor.get_parent()) {
                if (actor instanceof Meta.WindowActor)
                    return JSON.stringify({pid, window: describe(actor.meta_window, index.get(actor) ?? -1), chain, mode});
                chain.push(actorDescription(actor));
            }
            // Shell chrome (panel, banner, menu) is reactive: report it.
            if (mode === Clutter.PickMode.REACTIVE && chain.length > 1)
                return JSON.stringify({pid, window: null, chain, mode, shell_nodes: [...pickedShellNodes(picked, x, y), ...chain]});
        }
        return JSON.stringify({pid, window: null, chain: [], mode: null, shell_nodes: []});
    }
}
