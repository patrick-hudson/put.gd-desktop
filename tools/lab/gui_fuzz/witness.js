// Run inside gnome-shell by the test extension: new Function('ctx', <this file>)(ctx). Installs ctx.store.snap().
const {Main, Meta, St, Clutter, GLib, global} = ctx;
const T = Meta.WindowType;
const POPUPS = new Set([T.DROPDOWN_MENU, T.POPUP_MENU, T.TOOLTIP, T.COMBO, T.MENU]);
const fin = v => (Number.isFinite(v) ? Math.round(v) : 0);
const round = (x, y, w, h) => [fin(x), fin(y), fin(w), fin(h)];

function gtype(actor) {
    try { return actor.constructor.$gtype.name; } catch (e) { return actor.constructor.name; }
}

function extents(actor) {
    const [x, y] = actor.get_transformed_position();
    const [w, h] = actor.get_transformed_size();
    return round(x, y, w, h);
}

const typeName = t => Object.keys(T).find(k => T[k] === t) || String(t);

function windows() {
    const compositor = global.compositor;
    const actors = compositor && compositor.get_window_actors ? compositor.get_window_actors() : global.get_window_actors();
    let list = actors.map(a => a.meta_window).filter(w => w);
    try { list = global.display.sort_windows_by_stacking(list); } catch (e) { /* the actor order is already the stacking order */ }
    const focus = global.display.focus_window;
    return list.map((w, z) => {
        const type = w.get_window_type();
        const frame = w.get_frame_rect();
        const parent = w.get_transient_for();
        const actor = w.get_compositor_private();
        return {id: String(w.get_id()), pid: w.get_pid(), cls: w.get_wm_class() || '', app: w.get_gtk_application_id() || '', title: w.get_title() || '',
                frame: round(frame.x, frame.y, frame.width, frame.height), active: w === focus, min: !!w.minimized,
                hidden: !(actor && actor.visible), popup: POPUPS.has(type), dock: type === T.DOCK, desktop: type === T.DESKTOP,
                normal: type === T.NORMAL || type === T.DIALOG || type === T.MODAL_DIALOG, notif: type === T.NOTIFICATION,
                fullscreen: !!w.is_fullscreen(), tr: parent ? String(parent.get_id()) : null, kind: 'MetaWindow', type: typeName(type), z};
    });
}

// Direct children of the UI group: put.gd's region selector, recording frame, countdown and tooltip are St actors, not windows.
function actors() {
    const out = [];
    let z = 0;
    for (const a of Main.uiGroup.get_children()) {
        z++;
        if (!a.visible || a.opacity === 0)
            continue;
        const r = extents(a);
        if (r[2] <= 0 || r[3] <= 0)
            continue;
        const item = {t: gtype(a), name: a.name || '', cls: a.style_class || '', style: (a.style || '').slice(0, 90), reactive: !!a.reactive, frame: r, z};
        if (a instanceof St.Label)
            item.text = (a.text || '').slice(0, 24);
        if (item.t === 'Gjs_RegionSelector') {
            item.kids = [];
            for (const k of a.get_children())
                if (k.visible)
                    item.kids.push({t: gtype(k), name: k.accessible_name || k.name || '', frame: extents(k), text: k instanceof St.Label ? (k.text || '').slice(0, 24) : undefined});
        }
        out.push(item);
    }
    return out;
}

function monitors() {
    const list = Main.layoutManager.monitors;
    return list.map((m, i) => ({name: 'mon' + i, geom: round(m.x, m.y, m.width, m.height), scale: global.display.get_monitor_scale ? global.display.get_monitor_scale(i) : 1, primary: i === Main.layoutManager.primaryIndex}));
}

function chrome() {
    const out = {panel: null, overview: !!Main.overview.visible, modal: Main.modalCount | 0, banner: null, keyboard: false,
                 modal_actors: (Main.modalActorFocusStack || []).map(m => gtype(m.actor))};
    const panel = Main.layoutManager.panelBox;
    if (panel && panel.visible)
        out.panel = extents(panel);
    try {
        const banner = Main.messageTray._banner;
        if (banner && banner.visible && Main.messageTray._bannerBin && Main.messageTray._bannerBin.visible)
            out.banner = extents(Main.messageTray._bannerBin);
    } catch (e) { /* no banner */ }
    return out;
}

ctx.store.snap = function () {
    const [x, y] = global.get_pointer();
    return JSON.stringify({windows: windows(), actors: actors(), outputs: monitors(), cursor: [x, y], chrome: chrome()});
};
ctx.store.setClipboard = function (kind, base64) {
    const clipboard = St.Clipboard.get_default();
    const data = base64 ? GLib.base64_decode(base64) : new Uint8Array(0);
    if (kind === 'clear') {
        clipboard.set_text(St.ClipboardType.CLIPBOARD, '');
    } else if (kind === 'text') {
        clipboard.set_text(St.ClipboardType.CLIPBOARD, new TextDecoder().decode(data));
    } else {
        clipboard.set_content(St.ClipboardType.CLIPBOARD, kind, new GLib.Bytes(data));
    }
    return 'ok';
};
ctx.store.close = function (id) {
    for (const a of global.get_window_actors ? global.get_window_actors() : global.compositor.get_window_actors()) {
        const w = a.meta_window;
        if (w && String(w.get_id()) === String(id)) {
            w.delete(global.get_current_time());
            return 'closed';
        }
    }
    return 'gone';
};
return 'installed';
