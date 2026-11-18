// Test-only GNOME Shell extension for the GUI fuzzer (tools/lab/gui_fuzz). Lab guests only.
// It exports one method, org.putgd.GuiFuzz.Eval(code), at /org/putgd/GuiFuzz on the Shell's own
// connection (reach it as org.gnome.Shell). The fuzzer sends gui_fuzz/witness.js through it, so
// iterating on the witness needs no new login.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const XML = `<node><interface name="org.putgd.GuiFuzz">
<method name="Eval"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
</interface></node>`;

export default class GuiFuzzWitness extends Extension {
    enable() {
        const context = {Clutter, Gio, GLib, Meta, Shell, St, Main, global, store: {}};
        this._impl = Gio.DBusExportedObject.wrapJSObject(XML, {
            EvalAsync([code], invocation) {
                let reply;
                try {
                    const value = new Function('ctx', code)(context);
                    reply = value === undefined ? '' : String(value);
                } catch (error) {
                    reply = 'ERROR ' + error + '\n' + (error.stack ?? '');
                }
                invocation.return_value(new GLib.Variant('(s)', [reply]));
            },
        });
        this._impl.export(Gio.DBus.session, '/org/putgd/GuiFuzz');
    }

    disable() {
        this._impl?.unexport();
        this._impl = null;
    }
}
