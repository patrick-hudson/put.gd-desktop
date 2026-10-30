// Lab-only compositor witness for the put.gd surface driver (disposable guests).
// It reports Mutter's own window geometry, focus and stacking, and the stage
// hit-test at a point. Its one action, Activate, is the driver's explicit
// focus(raise_window=True) on Wayland: the same activation a dock click does.
// It never moves, resizes or closes anything.
import Gio from 'gi://Gio';
import Clutter from 'gi://Clutter';
import Meta from 'gi://Meta';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const PATH = '/org/putgd/LabWitness';
const XML = `<node><interface name="org.putgd.LabWitness">
<method name="Windows"><arg type="s" direction="out"/></method>
<method name="Pick"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="s" direction="out"/></method>
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

export default class LabWitness extends Extension {
    enable() {
        this._dbus = Gio.DBusExportedObject.wrapJSObject(XML, this);
        this._dbus.export(Gio.DBus.session, PATH);
    }

    disable() {
        this._dbus?.unexport();
        this._dbus = null;
    }

    Windows() {
        const display = global.display;
        const monitors = [];
        for (let i = 0; i < display.get_n_monitors(); i++)
            monitors.push({rect: rect(display.get_monitor_geometry(i)), scale: display.get_monitor_scale(i)});
        const windows = windowActors().map((actor, i) => describe(actor.meta_window, i));
        return JSON.stringify({windows, monitors});
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
        for (const mode of [Clutter.PickMode.REACTIVE, Clutter.PickMode.ALL]) {
            const chain = [];
            for (let actor = global.stage.get_actor_at_pos(mode, x, y); actor; actor = actor.get_parent()) {
                if (actor instanceof Meta.WindowActor)
                    return JSON.stringify({window: describe(actor.meta_window, index.get(actor) ?? -1), chain, mode});
                chain.push({type: actor.constructor?.name ?? '', name: actor.get_name?.() ?? '',
                            style: actor.style_class ?? '', accessible: actor.get_accessible_name?.() ?? ''});
            }
            // Shell chrome (panel, banner, menu) is reactive: report it.
            if (mode === Clutter.PickMode.REACTIVE && chain.length > 1)
                return JSON.stringify({window: null, chain, mode});
        }
        return JSON.stringify({window: null, chain: [], mode: null});
    }
}
