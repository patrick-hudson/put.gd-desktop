import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Config from 'resource:///org/gnome/shell/misc/config.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';


Gio._promisify(Shell.Screenshot.prototype, 'screenshot_area');
Gio._promisify(Shell.Screenshot.prototype, 'screenshot_stage_to_content');
Gio._promisify(Shell.Screenshot, 'composite_to_stream');
const PATH = '/org/gnome/Shell/Extensions/Putgd';
const IFACE = 'org.putgd.Gnome50';
const CLIENT = 'org.putgd.App.Gnome50';
const SNI_ITEM = 'org.kde.StatusNotifierItem';
const SNI_WATCHER = 'org.kde.StatusNotifierWatcher';
const SNI_ITEM_PATH = '/StatusNotifierItem';
const SNI_WATCHER_PATH = '/StatusNotifierWatcher';
const CHUNK = 65536;
// Capture sources the Shell can prepare; Edit & upload binds its own key and
// prepares a native region selection.
const SOURCES = ['region', 'window', 'desktop', 'clipboard'];
const ACTIONS = [...SOURCES, 'edit', 'record'];
const XML = `<node><interface name="${IFACE}">
<method name="Hello"><arg type="s" direction="out"/></method>
<method name="SetUiProcess"><arg type="u" direction="in"/><arg type="s" direction="out"/></method>
<method name="InvalidateCopy"><arg type="s" direction="out"/></method>
<method name="Revision"><arg type="s" direction="out"/></method>
<method name="Prepare"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
<method name="Acquire"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
<method name="Read"><arg type="s" direction="in"/><arg type="t" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="out"/></method>
<method name="Release"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
<method name="BeginCopy"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="t" direction="in"/><arg type="s" direction="out"/></method>
<method name="WriteCopy"><arg type="s" direction="in"/><arg type="t" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
<method name="CommitCopy"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
<method name="Bind"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
<method name="SelectArea"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
<method name="ShowRecordingFrame"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
<method name="HideRecordingFrame"><arg type="s" direction="out"/></method>
<signal name="Action"><arg type="s"/><arg type="s"/></signal>
<signal name="ClipboardChanged"><arg type="t"/></signal>
<signal name="CapabilitiesChanged"/>
</interface></node>`;

function failure(code, message) {
    const error = new Error(message);
    error.bridgeCode = code;
    return error;
}
function check(condition, code, message) {
    if (!condition)
        throw failure(code, message);
}
function rectangle(rect) {
    return [rect.x, rect.y, rect.width, rect.height];
}
function limitsFrom(json) {
    check(json.length <= 4096, 'bridge_protocol', 'Resource limits exceed the control-message limit');
    const value = JSON.parse(json);
    for (const name of ['textBytes', 'imageBytes', 'imagePixels', 'maxFiles'])
        check(Number.isSafeInteger(value[name]) && value[name] > 0,
            'bridge_protocol', 'Invalid resource limits');
    // Match the shared configuration's maximum exactly; chunk offsets are uint64.
    check(value.textBytes <= 1099511627776 && value.imageBytes <= 1099511627776,
        'input_excessive', 'Configured transfer exceeds the bridge byte range');
    check(Array.isArray(value.imageMimes) && value.imageMimes.length <= 128 &&
        value.imageMimes.every(m => typeof m === 'string' && m.length <= 128 && m.startsWith('image/')),
    'bridge_protocol', 'Invalid supported image MIME list');
    check(value.outcome === undefined || (typeof value.outcome === 'string' && value.outcome.length <= 256),
        'bridge_protocol', 'Invalid capture outcome');
    check(value.regionMode === undefined || value.regionMode === 'screenshot_first',
        'bridge_protocol', 'Invalid region selection mode');
    return value;
}
// The recording helper's control bar, a frameless Qt window the Shell places.
const RECORDING_BAR_TITLE = 'put.gd recording controls';
const RECORDING_STATES = ['ready', 'recording', 'paused'];
const RECORDING_KINDS = ['region', 'monitor', 'desktop'];
function overlaps(a, b) {
    return a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
}
function contains(area, r) {
    return r[0] >= area[0] && r[1] >= area[1] && r[0] + r[2] <= area[0] + area[2] && r[1] + r[3] <= area[1] + area[3];
}
// Rectangles covering `rects` minus `hole`, all [x, y, width, height].
function subtract(rects, hole) {
    const result = [];
    for (const r of rects) {
        if (!overlaps(r, hole)) {
            result.push(r);
            continue;
        }
        const top = Math.max(r[1], hole[1]), bottom = Math.min(r[1] + r[3], hole[1] + hole[3]);
        result.push([r[0], r[1], r[2], top - r[1]], [r[0], bottom, r[2], r[1] + r[3] - bottom],
            [r[0], top, Math.max(r[0], hole[0]) - r[0], bottom - top],
            [Math.min(r[0] + r[2], hole[0] + hole[2]), top, r[0] + r[2] - Math.min(r[0] + r[2], hole[0] + hole[2]), bottom - top]);
    }
    return result.filter(r => r[2] > 0 && r[3] > 0);
}
// Where the control bar goes (the helper's BarPlacement rules): below the
// recorded rectangle, else above it, in the work area holding its centre; else
// at the top or bottom of another monitor's work area; else inside the
// rectangle's bottom-right corner (it is then recorded), kept on that work
// area. allowOutside false keeps an earlier inside decision for a resized bar.
function barPlacement(rect, size, areas, allowOutside = true, gap = 10) {
    const [x, y, w, h] = rect, [bw, bh] = size;
    const centre = [x + w / 2, y + h / 2];
    const home = areas.find(a => centre[0] >= a[0] && centre[0] < a[0] + a[2] && centre[1] >= a[1] && centre[1] < a[1] + a[3]) ?? areas[0];
    const candidates = [];
    if (home) {
        const left = Math.max(home[0], Math.min(x, home[0] + home[2] - bw));
        candidates.push([left, y + h + gap, bw, bh], [left, y - gap - bh, bw, bh]);
    }
    for (const area of areas) {
        if (area !== home) {
            const left = area[0] + Math.round((area[2] - bw) / 2);
            candidates.push([left, area[1] + gap, bw, bh], [left, area[1] + area[3] - gap - bh, bw, bh]);
        }
    }
    const chosen = allowOutside && candidates.find(c => areas.some(a => contains(a, c)) && !overlaps(c, rect));
    if (chosen)
        return {rect: chosen, outside: true};
    const inside = [x + w - bw - gap, y + h - bh - gap, bw, bh];
    if (home) {
        inside[0] = Math.max(home[0], Math.min(inside[0], home[0] + home[2] - bw));
        inside[1] = Math.max(home[1], Math.min(inside[1], home[1] + home[3] - bh));
    }
    return {rect: inside, outside: !overlaps(inside, rect)};
}

// The ready ring just outside the recorded rectangle and, while recording, dim
// strips over everything else except the bar. Plain non-reactive Shell actors:
// input passes through them and none overlaps the recorded pixels, so the area
// stream never contains them.
class RecordingFrame {
    constructor() {
        this._actors = [];
    }
    show(rect, kind, dimmed, bar) {
        this.destroy();
        this._shown = [rect, kind, dimmed];
        this._bar = bar;
        const add = (r, color) => {
            if (r[2] <= 0 || r[3] <= 0)
                return;
            const actor = new St.Widget({reactive: false, can_focus: false, track_hover: false,
                x: r[0], y: r[1], width: r[2], height: r[3], style: `background-color: ${color};`});
            Main.uiGroup.add_child(actor);
            this._actors.push(actor);
        };
        const [x, y, w, h] = rect;
        if (dimmed) {
            let strips = subtract([[0, 0, global.stage.width, global.stage.height]], rect);
            if (bar)
                strips = subtract(strips, bar);
            for (const strip of strips)
                add(strip, 'rgba(0, 0, 0, 0.43)');
        }
        if (kind === 'desktop')
            return;
        // A 3 px white ring with a dark hairline next to the rectangle, as on X11.
        const t = 3;
        for (const r of [[x - t, y - t, w + 2 * t, t], [x - t, y + h, w + 2 * t, t], [x - t, y, t, h], [x + w, y, t, h]])
            add(r, 'rgba(255, 255, 255, 0.92)');
        for (const r of [[x - 1, y - 1, w + 2, 1], [x - 1, y + h, w + 2, 1], [x - 1, y, 1, h], [x + w, y, 1, h]])
            add(r, 'rgba(0, 0, 0, 0.78)');
    }
    // The bar moved: cut its new spot out of the dim strips.
    moveBar(bar) {
        if (this._shown && this._bar && this._shown[2] && (bar[0] !== this._bar[0] || bar[1] !== this._bar[1] || bar[2] !== this._bar[2] || bar[3] !== this._bar[3]))
            this.show(...this._shown, bar);
        else if (this._bar)
            this._bar = bar;
    }
    destroy() {
        for (const actor of this._actors)
            actor.destroy();
        this._actors = [];
    }
}

function bytesOf(stream) {
    stream.close(null);
    return stream.steal_as_bytes().toArray();
}

// Own only this selector's modal grab and actors; cleanup does not reach into
// screenshot UI internals or leave an upstream idle destruction callback alive.
// Pure geometry module, mirrored by RegionSelection.h and exercised without Shell.
const RegionSelection = {
    // No scrim on any monitor (#629): a one-line hint pill that fades after
    // about 2 s, then only a compact size label next to the selection.
    hint: 'Drag to select · Esc to cancel',
    hintHoldMs: 1700,
    hintFadeMs: 300,
    // Native fallback for older clients; keep this contracted to Theme.cpp.
    pillTokens(scheme) {
        return scheme === 'prefer-dark'
            ? {surfaceRaised: '#2a2a2a', text: '#f0f0ec', borderStrong: '#737373'}
            : {surfaceRaised: '#ffffff', text: '#292926', borderStrong: '#8e8e87'};
    },
    pillStyle(tokens) {
        for (const key of ['surfaceRaised', 'text', 'borderStrong'])
            check(/^#[0-9a-f]{6}$/i.test(tokens[key]), 'bridge_protocol', 'Invalid region theme token');
        return `background-color: ${tokens.surfaceRaised}; color: ${tokens.text}; border: 1px solid ${tokens.borderStrong}; border-radius: 999px; padding: 4px 12px; font-size: 10.5pt;`;
    },
    switchDisplay(region, monitors, current, backwards = false) {
        const next = (current + (backwards ? -1 : 1) + monitors.length) % monitors.length;
        if (!region || region.width < 1 || region.height < 1)
            return {current: next, region: null};
        const from = monitors[current], to = monitors[next];
        const width = Math.min(region.width, to.width), height = Math.min(region.height, to.height);
        return {current: next, region: {
            x: Math.max(to.x, Math.min(region.x + to.x - from.x, to.x + to.width - width)),
            y: Math.max(to.y, Math.min(region.y + to.y - from.y, to.y + to.height - height)), width, height}};
    },
    overlap(a, b) {
        return b ? Math.max(0, Math.min(a.x + a.width, b.x + b.width) - Math.max(a.x, b.x)) *
            Math.max(0, Math.min(a.y + a.height, b.y + b.height) - Math.max(a.y, b.y)) : 0;
    },
    // Top centre of the monitor, or its bottom when the selection covers the top.
    hintPosition(monitor, width, height, region) {
        const x = monitor.x + Math.floor((monitor.width - width) / 2);
        const top = {x, y: monitor.y + 16, width, height};
        const bottom = {x, y: monitor.y + monitor.height - 16 - height, width, height};
        return this.overlap(top, region) > 0 && this.overlap(bottom, region) === 0 ? [x, bottom.y] : [x, top.y];
    },
    // Just outside the bottom-left corner; above the top-left without room
    // below, inside the top-left corner when neither fits.
    sizePosition(monitor, width, height, region) {
        const gap = 4;
        let x = region.x, y = region.y + region.height + gap;
        if (y + height > monitor.y + monitor.height) {
            y = region.y - gap - height;
            if (y < monitor.y) {
                x += gap;
                y = region.y + gap;
            }
        }
        return [Math.max(monitor.x, Math.min(x, monitor.x + monitor.width - width)),
            Math.max(monitor.y, Math.min(y, monitor.y + monitor.height - height))];
    },
    // The monitor that holds most of the selection shows its size.
    labelMonitor(monitors, region) {
        let best = null, area = 0;
        for (const monitor of monitors) {
            const part = this.overlap(monitor, region);
            if (part > area) {
                area = part;
                best = monitor;
            }
        }
        return best;
    },
    // A monitor's logical rectangle in stage-texture pixels, clipped to the texture.
    textureArea([x, y, width, height], scale, textureWidth, textureHeight) {
        const left = Math.max(0, Math.floor(x * scale)), top = Math.max(0, Math.floor(y * scale));
        const right = Math.min(textureWidth, Math.ceil((x + width) * scale));
        const bottom = Math.min(textureHeight, Math.ceil((y + height) * scale));
        return [left, top, Math.max(0, right - left), Math.max(0, bottom - top)];
    },
    // A window's logical rectangle on its head's output pixel grid (#1049): the
    // pixels a desktop capture of that head holds, and their logical extent.
    outputGrid([x, y, width, height], {logical: [mx, my], density}) {
        const left = Math.floor((x - mx) * density), top = Math.floor((y - my) * density);
        const right = Math.ceil((x + width - mx) * density), bottom = Math.ceil((y + height - my) * density);
        return {pixels: [right - left, bottom - top],
            logical: [mx + left / density, my + top / density, (right - left) / density, (bottom - top) / density]};
    },
    sizeLabel(region, outputs = []) {
        if (!(region?.width > 0 && region?.height > 0))
            return '';
        const size = this.outputSize(region, outputs) || region;
        return `${Math.round(size.width)} × ${Math.round(size.height)}`;
    },
    outputSize(region, outputs) {
        const frames = outputs.filter(f => region.x < f.x + f.width && region.x + region.width > f.x && region.y < f.y + f.height && region.y + region.height > f.y);
        if (!frames.length || frames.some(f => !Number.isFinite(f.density) || f.density <= 0))
            return null;
        const single = frames[0];
        if (frames.length === 1 && region.x >= single.x && region.y >= single.y && region.x + region.width <= single.x + single.width && region.y + region.height <= single.y + single.height) {
            const x = region.x - single.x, y = region.y - single.y;
            return {width: Math.ceil((x + region.width) * single.density) - Math.floor(x * single.density),
                height: Math.ceil((y + region.height) * single.density) - Math.floor(y * single.density)};
        }
        const density = Math.max(...frames.map(f => f.density));
        return {width: Math.ceil((region.x + region.width) * density) - Math.floor(region.x * density),
            height: Math.ceil((region.y + region.height) * density) - Math.floor(region.y * density)};
    },
    instructions(region, recording, display, bounds, outputs = [], dragging = false, outcome = '') {
        const selected = region?.width > 0 && region?.height > 0;
        const logical = selected ? region : bounds;
        const output = logical ? this.outputSize(logical, outputs) : null;
        const size = output || logical;
        const context = `${outputs.length > 1 ? `${display || 'Display 1'} · ` : ''}${selected ? '' : 'Entire display · '}${Math.round(size?.width ?? 0)} × ${Math.round(size?.height ?? 0)} ${output ? 'px' : 'logical px'} · `;
        const accept = dragging ? (recording ? 'Release to choose recording area' : 'Release to capture') : recording ? 'Enter to choose'
            : 'Enter to capture';
        return context + accept + ' · Esc to cancel\n' + (dragging ? '' : 'Drag to select · ') +
            'Arrows move · Shift+arrows resize' + (outputs.length > 1 ? ' · Tab next display' : '') +
            (recording ? '\nStart recording comes next' : outcome ? `\n${outcome}` : '');
    },
};

const RegionSelector = GObject.registerClass(class RegionSelector extends St.Widget {
    // frozen (#629): the stage screenshot taken before selection, drawn as the
    // backdrop; without it the selector is transparent over the live desktop.
    _init(recording = false, outputs = null, outcome = '', frozen = null, appearance = {}) {
        super._init({reactive: true, visible: false, accessible_name: recording ? 'Recording region' : 'Capture region', x: 0, y: 0});
        this._outputs = outputs;
        this._outcome = outcome;
        this.add_constraint(new Clutter.BindConstraint({source: global.stage, coordinate: Clutter.BindCoordinate.ALL}));
        this.set_cursor_type(Clutter.CursorType.CROSSHAIR);
        if (frozen) {
            this._backdrop = new St.Widget({reactive: false, x: 0, y: 0, width: global.stage.width, height: global.stage.height});
            this._backdrop.set_content(frozen);
            this.add_child(this._backdrop);
        }
        this._rectangle = new St.Widget({visible: false, accessible_name: recording ? 'Recording region' : 'Capture region',
            clip_to_allocation: true, style: 'border: 3px solid black; background-color: transparent;'});
        this.add_child(this._rectangle);
        this._contrast = new St.Widget({visible: false, clip_to_allocation: true, style: 'border: 1px solid white; background-color: transparent;'});
        this.add_child(this._contrast);
        this._recording = recording;
        this._keyboardMonitor = this._monitor();
        this._hintMonitor = this._keyboardMonitor;
        // The resident resolves System/Dark/Light and supplies the app tokens.
        const settings = new Gio.Settings({schema_id: 'org.gnome.desktop.interface'});
        const style = RegionSelection.pillStyle(appearance.tokens ?? RegionSelection.pillTokens(settings.get_string('color-scheme')));
        this._hint = new St.Label({reactive: false, text: RegionSelection.hint, style});
        this.add_child(this._hint);
        this._size = new St.Label({reactive: false, visible: false, text: '', style});
        this.add_child(this._size);
        this._instructionText = '';
        this._region = null;
        this._start = null;
        this._finish = null;
        this._grab = null;
        this._closed = false;
        this.connect('captured-event', (_actor, event) => this._input(event));
        Main.uiGroup.add_child(this);
        this._describe();
        this._placeInstructions();
    }
    _input(event) {
        const type = event.type();
        if (type === Clutter.EventType.KEY_PRESS) {
            const key = event.get_key_symbol();
            if (key === Clutter.KEY_Escape) {
                this.cancel();
            } else if (key === Clutter.KEY_Return || key === Clutter.KEY_KP_Enter) {
                this._settle(this._region?.width > 0 && this._region?.height > 0 ? this._region : this._monitor());
            } else if (key === Clutter.KEY_Tab || key === Clutter.KEY_ISO_Left_Tab) {
                const monitors = Main.layoutManager.monitors;
                const result = RegionSelection.switchDisplay(this._region, monitors, monitors.indexOf(this._monitor()),
                    key === Clutter.KEY_ISO_Left_Tab || !!(event.get_state() & Clutter.ModifierType.SHIFT_MASK));
                this._keyboardMonitor = monitors[result.current];
                this._start = null;
                if (result.region)
                    this._showRegion(result.region);
                else {
                    this._describe();
                    this._placeInstructions();
                }
            } else {
                const step = new Map([[Clutter.KEY_Left, [-1, 0]], [Clutter.KEY_Right, [1, 0]],
                    [Clutter.KEY_Up, [0, -1]], [Clutter.KEY_Down, [0, 1]]]).get(key);
                if (step) {
                    const monitor = this._monitor();
                    if (!monitor)
                        return Clutter.EVENT_STOP;
                    let rect = this._region;
                    // Intersect a mouse-drawn region before constraining keyboard edits.
                    if (rect) {
                        const x = Math.max(rect.x, monitor.x), y = Math.max(rect.y, monitor.y);
                        rect = {x, y, width: Math.min(rect.x + rect.width, monitor.x + monitor.width) - x,
                            height: Math.min(rect.y + rect.height, monitor.y + monitor.height) - y};
                    }
                    if (!rect || rect.width < 1 || rect.height < 1) {
                        const width = Math.max(1, Math.floor(monitor.width / 2));
                        const height = Math.max(1, Math.floor(monitor.height / 2));
                        rect = {x: monitor.x + Math.floor((monitor.width - width) / 2),
                            y: monitor.y + Math.floor((monitor.height - height) / 2), width, height};
                    } else {
                        const state = event.get_state();
                        const amount = state & Clutter.ModifierType.CONTROL_MASK ? 10 : 1;
                        const [dx, dy] = step.map(value => value * amount);
                        if (state & Clutter.ModifierType.SHIFT_MASK) {
                            rect.width = Math.max(1, Math.min(rect.width + dx, monitor.x + monitor.width - rect.x));
                            rect.height = Math.max(1, Math.min(rect.height + dy, monitor.y + monitor.height - rect.y));
                        } else {
                            rect.x = Math.max(monitor.x, Math.min(rect.x + dx, monitor.x + monitor.width - rect.width));
                            rect.y = Math.max(monitor.y, Math.min(rect.y + dy, monitor.y + monitor.height - rect.height));
                        }
                    }
                    this._start = null;
                    this._showRegion(rect);
                }
            }
            return Clutter.EVENT_STOP;
        }
        if (type === Clutter.EventType.BUTTON_PRESS) {
            if (event.get_button() === Clutter.BUTTON_SECONDARY) {
                this.cancel();
                return Clutter.EVENT_STOP;
            }
            if (event.get_button() === Clutter.BUTTON_PRIMARY) {
                this._keyboardMonitor = null;
                this._start = event.get_coords().map(Math.floor);
                this._update(this._start);
            }
            return Clutter.EVENT_STOP;
        }
        if (type === Clutter.EventType.MOTION) {
            if (this._start)
                this._update(event.get_coords().map(Math.floor));
            return Clutter.EVENT_STOP;
        }
        if (type === Clutter.EventType.BUTTON_RELEASE) {
            if (event.get_button() === Clutter.BUTTON_PRIMARY && this._start) {
                const rect = this._geometry(event.get_coords().map(Math.floor));
                this._settle(rect.width > 0 && rect.height > 0 ? rect : null);
            }
            return Clutter.EVENT_STOP;
        }
        return Clutter.EVENT_PROPAGATE;
    }
    _geometry([x, y]) {
        // Pointer positions are rectangle edges, matching the shared Qt selector.
        return {x: Math.min(this._start[0], x), y: Math.min(this._start[1], y),
            width: Math.abs(x - this._start[0]), height: Math.abs(y - this._start[1])};
    }
    _update(point) {
        this._pointer = point;
        this._showRegion(this._geometry(point));
    }
    _monitor() {
        if (this._keyboardMonitor)
            return this._keyboardMonitor;
        const [x, y] = this._region ? [this._region.x + this._region.width / 2,
            this._region.y + this._region.height / 2] : global.get_pointer();
        return Main.layoutManager.monitors.find(m => x >= m.x && x < m.x + m.width && y >= m.y && y < m.y + m.height)
            ?? Main.layoutManager.primaryMonitor;
    }
    _outputList() {
        return this._outputs || Main.layoutManager.monitors.map((m, i) => ({...m, density: global.display.get_monitor_scale(i)}));
    }
    _placeInstructions() {
        const monitor = this._monitor();
        if (!monitor)
            return;
        const selected = this._region?.width > 0 && this._region?.height > 0;
        if (!selected) {
            this._rectangle.hide();
            this._contrast.hide();
        }
        const hintMonitor = this._hintMonitor || monitor;
        this._hint.set_position(...RegionSelection.hintPosition(hintMonitor, this._hint.get_preferred_width(-1)[1],
            this._hint.get_preferred_height(-1)[1], selected ? this._region : null));
        const labelMonitor = selected ? RegionSelection.labelMonitor(Main.layoutManager.monitors, this._region) : null;
        if (!labelMonitor) {
            this._size.hide();
            return;
        }
        this._size.text = RegionSelection.sizeLabel(this._region, this._outputList());
        this._size.set_position(...RegionSelection.sizePosition(labelMonitor, this._size.get_preferred_width(-1)[1],
            this._size.get_preferred_height(-1)[1], this._region));
        this._size.show();
    }
    _describe() {
        const rect = this._region;
        const monitor = this._monitor();
        const index = Main.layoutManager.monitors.indexOf(monitor);
        const display = `Display ${index + 1}`;
        // The full keyboard help and outcome stay in the accessible description.
        const instructions = RegionSelection.instructions(rect, this._recording, display, monitor, this._outputList(), !!this._start, this._outcome);
        this._instructionText = instructions;
        const description = (rect && rect.width > 0 && rect.height > 0
            ? `Region ${rect.width} × ${rect.height} at ${rect.x}, ${rect.y}.`
            : 'No region selected.') + ` ${instructions}`;
        this._rectangle.get_accessible().set_description(description);
        this.get_accessible().set_description(description);
    }
    _showRegion(rect) {
        this._region = rect;
        this._outline(rect);
        this._describe();
        this._placeInstructions();
    }
    _outline(rect) {
        this._rectangle.set_position(rect.x, rect.y);
        this._rectangle.set_size(rect.width, rect.height);
        this._rectangle.show();
        // CSS lengths follow the theme scale; actor positions use stage coordinates.
        // Clip the strokes to their allocations for drags smaller than a border.
        const inset = St.ThemeContext.get_for_stage(global.stage).scale_factor;
        if (rect.width > 2 * inset && rect.height > 2 * inset) {
            this._contrast.set_position(rect.x + inset, rect.y + inset);
            this._contrast.set_size(rect.width - 2 * inset, rect.height - 2 * inset);
            this._contrast.show();
        } else {
            this._contrast.hide();
        }
    }
    selectAsync() {
        return new Promise((resolve, reject) => {
            this._finish = resolve;
            try {
                this.show();
                this._grab = Main.pushModal(this);
                check(!this._grab.is_revoked(), 'busy_selecting',
                    'GNOME cannot acquire the selection input grab');
                this._hint.ease({opacity: 0, delay: RegionSelection.hintHoldMs, duration: RegionSelection.hintFadeMs,
                    mode: Clutter.AnimationMode.EASE_OUT_QUAD});
            } catch (error) {
                this._finish = null;
                this._cleanup();
                reject(error);
            }
        });
    }
    _cleanup() {
        if (this._closed)
            return;
        this._closed = true;
        const grab = this._grab;
        this._grab = null;
        if (grab)
            Main.popModal(grab);
        this._hint.remove_all_transitions();
        this.hide();
        this.destroy();
    }
    _settle(value) {
        const finish = this._finish;
        this._finish = null;
        this._cleanup();
        finish?.(value);
    }
    cancel() {
        this._settle(null);
    }
});

// AppIndicators exposes the existing panel button through St accessible_name,
// but GNOME 50's host does not render SNI ToolTip. Own only a companion label.
class TrayTooltipCompanion {
    enable() {
        this._enabled = true; this._generation = 0; this._signals = [];
        this._client = this._watcher = null; this._actorSignals = [];
        this._sessionSignal = Main.sessionMode.connect('updated', () => this._reset());
        const watch = (name, field) => Gio.bus_watch_name(Gio.BusType.SESSION, name,
            Gio.BusNameWatcherFlags.NONE, (_bus, _name, owner) => {
                if (this[field] !== owner) {this[field] = owner; this._reset();}
            }, () => {this[field] = null; this._reset();});
        this._clientWatch = watch(CLIENT, '_client');
        this._watcherWatch = watch(SNI_WATCHER, '_watcher');
    }
    disable() {
        this._enabled = false; this._reset();
        Main.sessionMode.disconnect(this._sessionSignal);
        Gio.bus_unwatch_name(this._clientWatch); Gio.bus_unwatch_name(this._watcherWatch);
    }
    _reset() {
        this._generation++; this._pending?.cancellable.cancel();
        if (this._pending?.deadline) {GLib.source_remove(this._pending.deadline); this._pending.deadline = 0;}
        this._pending = null; this._again = false;
        if (this._timer) GLib.source_remove(this._timer);
        this._timer = 0; this._releaseActor(); this._item = null; this._text = '';
        for (const id of this._signals) Gio.DBus.session.signal_unsubscribe(id);
        this._signals = [];
        if (!this._enabled || Main.sessionMode.isLocked || !this._client || !this._watcher) return;
        this._retryAttempts = 0; this._retryActor();
        this._refresh();
    }
    _retryActor() {
        if (this._timer || !this._enabled || Main.sessionMode.isLocked || !this._client || !this._watcher) return;
        this._timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, () => {
            if (++this._retryAttempts > 15) {this._timer = 0; return GLib.SOURCE_REMOVE;}
            this._refresh(); return GLib.SOURCE_CONTINUE;
        });
    }
    _call(service, path, iface, method, parameters, cancellable) {
        return new Promise((resolve, reject) => {
            Gio.DBus.session.call(service, path, iface, method, parameters, null,
                Gio.DBusCallFlags.NONE, 1000, cancellable, (bus, result) => {
                    try {resolve(bus.call_finish(result).deep_unpack()[0]);} catch (error) {reject(error);}
                });
        });
    }
    _owner(name, cancellable) {
        return this._call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'GetNameOwner', new GLib.Variant('(s)', [name]), cancellable);
    }
    _pid(owner, cancellable) {
        return this._call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'GetConnectionUnixProcessID', new GLib.Variant('(s)', [owner]), cancellable);
    }
    async _property(owner, path, iface, name, cancellable) {
        let value = await this._call(owner, path, 'org.freedesktop.DBus.Properties', 'Get',
            new GLib.Variant('(ss)', [iface, name]), cancellable);
        // Properties.Get wraps its value in a variant; never request pixmaps.
        if (value?.deep_unpack) value = value.deep_unpack();
        return value;
    }
    async _findItem(pid, cancellable) {
        const watcher = SNI_WATCHER, path = SNI_WATCHER_PATH;
        if (await this._property(this._watcher, path, watcher, 'IsStatusNotifierHostRegistered', cancellable) !== true)
            throw new Error('Native tray host unavailable');
        const entries = await this._property(this._watcher, path, watcher, 'RegisteredStatusNotifierItems', cancellable);
        if (!Array.isArray(entries) || entries.length > 64) throw new Error('Tray registration exceeds bound');
        const matches = (await Promise.all(entries.map(async entry => {
            if (typeof entry !== 'string' || entry.length > 512) return null;
            const slash = entry.indexOf('/'), service = slash < 0 ? entry : entry.slice(0, slash);
            const itemPath = slash < 0 ? SNI_ITEM_PATH : entry.slice(slash);
            if (!/^(?::[0-9]+\.[0-9]+|[A-Za-z_][A-Za-z0-9_.-]+)$/.test(service) || itemPath !== SNI_ITEM_PATH) return null;
            try {
                const owner = await this._owner(service, cancellable);
                // A foreign application never supplies even item metadata.
                const itemPid = await this._pid(owner, cancellable);
                if (itemPid !== pid) return null;
                if (await this._property(owner, itemPath, SNI_ITEM, 'Id', cancellable) !== 'putgd') return null;
                return {service, owner, path: itemPath, pid: itemPid};
            } catch {return null;}
        }))).filter(Boolean);
        if (matches.length !== 1 || matches[0].pid !== pid) throw new Error('Resident tray item is not unique');
        return matches[0];
    }
    _subscribe(item) {
        const bus = Gio.DBus.session;
        this._signals.push(bus.signal_subscribe(item.owner, SNI_ITEM, 'NewToolTip',
            item.path, null, Gio.DBusSignalFlags.NONE, (_bus, sender) => {
                if (sender === item.owner) this._refresh();
            }));
        this._signals.push(bus.signal_subscribe(item.owner, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
            item.path, SNI_ITEM, Gio.DBusSignalFlags.NONE, (_bus, _sender, _path, _iface, _name, parameters) => {
                const [, changed, invalidated] = parameters.deep_unpack();
                if (Object.hasOwn(changed, 'Title') || invalidated.includes('Title')) this._refresh();
            }));
        this._signals.push(bus.signal_subscribe('org.freedesktop.DBus', 'org.freedesktop.DBus', 'NameOwnerChanged',
            '/org/freedesktop/DBus', item.service, Gio.DBusSignalFlags.NONE, () => this._reset()));
        for (const name of ['StatusNotifierItemRegistered', 'StatusNotifierItemUnregistered'])
            this._signals.push(bus.signal_subscribe(this._watcher, SNI_WATCHER, name,
                SNI_WATCHER_PATH, null, Gio.DBusSignalFlags.NONE, () => this._reset()));
    }
    _findActor(title) {
        const pending = [Main.panel], matches = [];
        let count = 0;
        while (pending.length && ++count <= 512) {
            const actor = pending.pop();
            if (actor instanceof St.Widget && actor.accessible_name === title && actor.reactive && actor.visible && actor.mapped)
                matches.push(actor);
            pending.push(...actor.get_children());
        }
        return pending.length === 0 && matches.length === 1 ? matches[0] : null;
    }
    _tooltip(value) {
        if (!Array.isArray(value) || value.length !== 4 || typeof value[2] !== 'string' || typeof value[3] !== 'string')
            throw new Error('Invalid native tooltip');
        const text = value[2] + (value[3] ? '\n' + value[3] : '');
        if (!value[2] || value[2].length > 160 || value[3].split('\n').length > 4 ||
            text.length > 2048 || new TextEncoder().encode(text).length > 2048 || /[\x00-\x09\x0b-\x1f\x7f]/.test(text))
            throw new Error('Native tooltip exceeds text bounds');
        return text;
    }
    async _refresh() {
        if (!this._enabled || Main.sessionMode.isLocked || !this._client || !this._watcher) return;
        if (this._pending) {this._again = true; return;}
        const generation = this._generation, cancellable = new Gio.Cancellable();
        const pending = {cancellable, deadline: 0}; this._pending = pending;
        pending.deadline = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 4000, () => {
            pending.deadline = 0; cancellable.cancel(); return GLib.SOURCE_REMOVE;
        });
        try {
            const client = await this._owner(CLIENT, cancellable);
            if (client !== this._client) throw failure('tray_identity_changed', 'Resident owner changed');
            const pid = await this._pid(client, cancellable);
            const item = this._item ?? await this._findItem(pid, cancellable);
            if (await this._pid(item.owner, cancellable) !== pid) throw failure('tray_identity_changed', 'Tray process changed');
            const [title, nativeTooltip] = await Promise.all([
                this._property(item.owner, item.path, SNI_ITEM, 'Title', cancellable),
                this._property(item.owner, item.path, SNI_ITEM, 'ToolTip', cancellable),
            ]);
            const tooltip = this._tooltip(nativeTooltip);
            if (typeof title !== 'string' || !title || title.length > 160 || /[\x00-\x1f\x7f]/.test(title))
                throw new Error('Invalid tray title');
            const [itemOwner, clientOwner, watcherOwner] = await Promise.all([
                this._owner(item.service, cancellable),
                this._owner(CLIENT, cancellable),
                this._owner(SNI_WATCHER, cancellable),
            ]);
            if (itemOwner !== item.owner || clientOwner !== client || watcherOwner !== this._watcher)
                throw failure('tray_identity_changed', 'Tray identity changed during read');
            if (generation !== this._generation || !this._enabled || Main.sessionMode.isLocked) return;
            if (!this._item) {this._item = item; this._subscribe(item);}
            const actor = this._findActor(title);
            if (!actor) {this._releaseActor(); this._text = ''; this._retryActor(); return;}
            this._attach(actor); this._title = title; this._text = tooltip;
            if (this._timer) GLib.source_remove(this._timer);
            this._timer = 0; this._retryAttempts = 0; this._show();
        } catch (error) {
            if (generation === this._generation) {
                this._label?.hide(); this._text = '';
                if (error.bridgeCode === 'tray_identity_changed') this._releaseActor();
                this._retryActor();
            }
        } finally {
            if (pending.deadline) GLib.source_remove(pending.deadline);
            if (generation === this._generation) {
                this._pending = null;
                if (this._again) {this._again = false; this._refresh();}
            }
        }
    }
    _attach(actor) {
        if (this._actor === actor) return;
        this._releaseActor(); this._actor = actor;
        this._label = new St.Label({style_class: 'dash-label', reactive: false, can_focus: false, visible: false});
        this._label.clutter_text.set_line_wrap(true); Main.uiGroup.add_child(this._label);
        this._actorSignals = [actor.connect('notify::hover', () => {
            if (this._hoverTimer) GLib.source_remove(this._hoverTimer);
            this._hoverTimer = 0; this._label?.hide();
            if (!actor.hover) this._suppressed = false;
            if (actor.hover && !this._suppressed) this._hoverTimer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 300, () => {
                this._hoverTimer = 0; this._refresh(); return GLib.SOURCE_REMOVE;
            });
        }), actor.connect('captured-event', (_actor, event) => {
            if (event.type() === Clutter.EventType.BUTTON_PRESS) {
                this._suppressed = true; this._label?.hide();
                if (this._hoverTimer) GLib.source_remove(this._hoverTimer);
                this._hoverTimer = 0;
            }
            return Clutter.EVENT_PROPAGATE;
        }), actor.connect('notify::accessible-name', () => this._refresh()),
        actor.connect('destroy', () => {this._releaseActor(); this._retryAttempts = 0; this._retryActor();})];
    }
    _show() {
        if (!this._label || !this._actor?.hover || this._hoverTimer || this._suppressed || !this._text || Main.sessionMode.isLocked) return;
        if (this._findActor(this._title) !== this._actor) {this._label.hide(); return;}
        const [x, y] = this._actor.get_transformed_position(), [width, height] = this._actor.get_transformed_size();
        const monitor = Main.layoutManager.monitors.find(m => x + width / 2 >= m.x && x + width / 2 < m.x + m.width &&
            y + height / 2 >= m.y && y + height / 2 < m.y + m.height);
        if (!monitor || monitor.width <= 16 || monitor.height <= 16) {this._label.hide(); return;}
        this._label.set_text(this._text);
        this._label.set_size(-1, -1);
        const naturalWidth = Math.min(this._label.get_preferred_width(-1)[1], monitor.width - 16);
        const naturalHeight = this._label.get_preferred_height(naturalWidth)[1];
        if (naturalHeight > monitor.height - 16) {this._label.hide(); return;}
        this._label.set_size(naturalWidth, naturalHeight);
        this._label.set_position(Math.max(monitor.x + 8, Math.min(x + width / 2 - naturalWidth / 2, monitor.x + monitor.width - naturalWidth - 8)),
            Math.max(monitor.y + 8, Math.min(y + height + 8, monitor.y + monitor.height - naturalHeight - 8)));
        this._label.show();
    }
    _releaseActor() {
        if (this._hoverTimer) GLib.source_remove(this._hoverTimer);
        this._hoverTimer = 0; this._suppressed = false;
        const actor = this._actor; this._actor = null;
        for (const id of this._actorSignals) actor?.disconnect(id);
        this._actorSignals = [];
        this._label?.destroy(); this._label = null;
    }
}

export default class PutgdExtension extends Extension {
    enable() {
        this._enabled = true;
        this._requests = new Map();
        this._bindings = new Map();
        this._selector = null;
        this._revision = 1;
        this._copyEpoch = 0;
        this._uiProcess = 0;
        this._uiProcessOwner = null;
        this._selection = global.display.get_selection();
        this._ownerSignal = this._selection.connect('owner-changed', (_selection, type) => {
            if (type !== Meta.SelectionType.SELECTION_CLIPBOARD)
                return;
            this._revision++;
            this._dbus.emit_signal('ClipboardChanged', new GLib.Variant('(t)', [this._revision]));
        });
        this._layoutSignal = Main.layoutManager.connect('monitors-changed', () => {
            for (const [id, request] of this._requests) {
                if (request.action && request.action !== 'clipboard')
                    this._drop(id, 'display_changed', 'Display layout changed during capture');
            }
        });
        this._sessionSignal = Main.sessionMode.connect('updated', () => {
            if (Main.sessionMode.isLocked) {
                for (const id of this._requests.keys())
                    this._drop(id, 'helper_unavailable', 'GNOME session locked');
                this._hideRecordingFrame();
            }
        });
        this._acceleratorSignal = global.display.connect('accelerator-activated', (_display, id) => {
            const action = this._bindings.get(id);
            if (action && this._enabled && !Main.sessionMode.isLocked)
                this._hotkey(action);
        });
        this._clientWatch = Gio.bus_watch_name(Gio.BusType.SESSION, CLIENT,
            Gio.BusNameWatcherFlags.NONE, null, () => {
                for (const id of this._requests.keys())
                    this._drop(id, 'helper_unavailable', 'Resident application disconnected');
                this._hideRecordingFrame();
                this._unbind();
                this._uiProcess = 0;
                this._uiProcessOwner = null;
            });
        this._dbus = Gio.DBusExportedObject.wrapJSObject(XML, this);
        this._dbus.export(Gio.DBus.session, PATH);
        this._dbus.emit_signal('CapabilitiesChanged', null);
        this._trayTooltip = new TrayTooltipCompanion();
        this._trayTooltip.enable();
    }

    disable() {
        if (!this._enabled)
            return;
        this._enabled = false;
        this._trayTooltip?.disable();
        this._trayTooltip = null;
        for (const id of this._requests.keys())
            this._drop(id, 'helper_unavailable', 'GNOME extension disabled');
        this._hideRecordingFrame();
        this._unbind();
        if (this._ownerSignal)
            this._selection.disconnect(this._ownerSignal);
        if (this._layoutSignal)
            Main.layoutManager.disconnect(this._layoutSignal);
        if (this._sessionSignal)
            Main.sessionMode.disconnect(this._sessionSignal);
        if (this._acceleratorSignal)
            global.display.disconnect(this._acceleratorSignal);
        if (this._clientWatch)
            Gio.bus_unwatch_name(this._clientWatch);
        this._ownerSignal = this._layoutSignal = this._sessionSignal = this._acceleratorSignal = this._clientWatch = 0;
        this._dbus.emit_signal('CapabilitiesChanged', null);
        this._dbus.unexport();
        this._dbus = null;
        this._selection = null;
        this._requests.clear();
    }

    _authenticate(invocation) {
        check(this._enabled && !Main.sessionMode.isLocked, 'helper_unavailable', 'GNOME user session is unavailable');
        const owner = Gio.DBus.session.call_sync('org.freedesktop.DBus',
            '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'GetNameOwner',
            new GLib.Variant('(s)', [CLIENT]), new GLib.VariantType('(s)'),
            Gio.DBusCallFlags.NONE, 1000, null).deep_unpack()[0];
        check(owner === invocation.get_sender(), 'bridge_unauthorized', 'Request is not from the resident GNOME bridge');
    }

    async _run(invocation, operation) {
        try {
            this._authenticate(invocation);
            const result = await operation(invocation.get_sender());
            invocation.return_value(new GLib.Variant('(s)', [JSON.stringify(result ?? {})]));
        } catch (error) {
            // Never log exception text from native MIME owners or source names.
            const known = typeof error.bridgeCode === 'string';
            const id = invocation.get_parameters().deep_unpack()[0];
            const request = this._requests.get(id);
            if (typeof id === 'string' && this._requests.get(id)?.sender === invocation.get_sender())
                this._drop(id);
            invocation.return_value(new GLib.Variant('(s)', [JSON.stringify({error: {
                code: known ? error.bridgeCode : 'helper_failed',
                message: known ? error.message : 'GNOME native request failed; inspect helper setup',
                details: known ? {} : {api: request?.stage ?? 'control',
                    exceptionType: ['TypeError', 'ReferenceError', 'Error'].includes(error.name) ? error.name : 'native'},
            }})]));
        }
    }

    _new(id, sender) {
        check(/^[0-9a-f-]{36}$/.test(id) && !this._requests.has(id),
            'bridge_protocol', 'Request ID is invalid or already in use');
        check(this._requests.size < 2048, 'busy_selecting', 'Too many pending GNOME bridge requests');
        const request = {id, sender, cancellable: new Gio.Cancellable(), data: null};
        request.timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 120000, () => {
            request.timer = 0;
            this._drop(id, 'transfer_timeout', 'GNOME source acquisition timed out');
            return GLib.SOURCE_REMOVE;
        });
        this._requests.set(id, request);
        return request;
    }

    _get(id, sender) {
        const request = this._requests.get(id);
        check(request && request.sender === sender, 'bridge_protocol', 'Request ID is stale or does not belong to this caller');
        return request;
    }

    _drop(id, code = 'cancelled', message = 'GNOME acquisition cancelled') {
        const request = this._requests.get(id);
        if (!request)
            return;
        this._requests.delete(id);
        if (request.timer)
            GLib.source_remove(request.timer);
        request.timer = 0;
        if (request.transferTimer)
            GLib.source_remove(request.transferTimer);
        request.transferTimer = 0;
        request.error = failure(code, message);
        request.cancellable.cancel();
        if (request.selector) {
            // Cancellation settles the promise and destroys our own actors/grab.
            request.selector.cancel();
            request.selector = null;
            this._selector = null;
        }
        request.data = null;
        request.parts = null;
        request.window = null;
        request.actor = null;
    }

    _alive(request) {
        if (request.error)
            throw request.error;
        check(this._enabled && this._requests.get(request.id) === request,
            'cancelled', 'GNOME request cancelled');
    }

    async _layout(cancellable) {
        const state = await new Promise((resolve, reject) => {
            Gio.DBus.session.call('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
                'org.gnome.Mutter.DisplayConfig', 'GetCurrentState', null, null,
                Gio.DBusCallFlags.NONE, 1000, cancellable, (connection, result) => {
                    try { resolve(connection.call_finish(result).deep_unpack()); }
                    catch { reject(failure('capture_geometry', 'Mutter display configuration is unavailable')); }
                });
        });
        const [serial, physical, logical] = state;
        return Main.layoutManager.monitors.map(monitor => {
            const configured = logical.find(entry => entry[0] === monitor.x && entry[1] === monitor.y);
            check(configured && configured[5].length > 0, 'capture_geometry', 'Mutter logical monitor metadata is unavailable');
            const connectors = configured[5].map(spec => spec[0]).sort();
            const modes = connectors.map(connector => {
                const output = physical.find(entry => entry[0][0] === connector);
                const mode = output?.[1].find(entry => entry[6]['is-current']?.deep_unpack());
                check(mode, 'capture_geometry', 'Mutter current native monitor mode is unavailable');
                return [mode[1], mode[2]];
            });
            return {identity: connectors.join('+'), logical: rectangle(monitor), density: configured[2],
                rotation: configured[3] % 4 * 90, transform: configured[3], physical: modes, serial};
        });
    }

    HelloAsync(_args, invocation) {
        this._run(invocation, () => ({version: 1, shellMajor: Number(Config.PACKAGE_VERSION.split('.')[0]),
            extensionVersion: this.metadata.version, clipboardRevision: this._revision, uiProcessVersion: 1,
            recordingVersion: 1}));
    }

    InvalidateCopyAsync(_args, invocation) {
        this._run(invocation, () => { this._copyEpoch++; return {}; });
    }

    RevisionAsync(_args, invocation) {
        this._run(invocation, () => ({clipboardRevision: this._revision}));
    }

    SetUiProcessAsync([pid], invocation) {
        this._run(invocation, sender => {
            this._uiProcess = pid;
            this._uiProcessOwner = sender;
            return {};
        });
    }

    async _prepare(id, action, sender) {
        // Clipboard imports protect their source from an unsent result. Capture
        // uploads preserve earlier links; explicit/local copies use InvalidateCopy.
        if (action === 'clipboard')
            this._copyEpoch++;
        check(SOURCES.includes(action), 'source_unsupported', 'Unknown GNOME source action');
        const request = this._new(id, sender);
        request.action = action;
        request.stage = 'prepare';
        request.revision = this._revision;
        // Freeze focus and geometry synchronously in the Shell hotkey callback,
        // before display metadata retrieval or native-app acceptance yields.
        if (action === 'window') {
            const window = global.display.focus_window;
            check(window && !this._residentWindow(window, sender),
                'capture_target', 'Focus an external application window before capture');
            const actor = window.get_compositor_private();
            check(actor && !window.minimized, 'capture_target', 'Focused external window is unavailable');
            request.window = window;
            request.actor = actor;
            request.frame = rectangle(window.get_frame_rect());
        }
        request.layout = action === 'clipboard' ? null : await this._layout(request.cancellable);
        this._alive(request);
        return {version: 1, requestId: id};
    }

    async _hotkey(action) {
        const id = GLib.uuid_string_random();
        try {
            const sender = Gio.DBus.session.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                'org.freedesktop.DBus', 'GetNameOwner', new GLib.Variant('(s)', [CLIENT]),
                new GLib.VariantType('(s)'), Gio.DBusCallFlags.NONE, 1000, null).deep_unpack()[0];
            // A selection needs the pointer and keyboard put.gd's open tray menu
            // holds, and must not show that menu (#924).
            if (action !== 'desktop' && action !== 'window')
                this._closeTrayMenu(sender);
            // Recording selects its own region later; only the key press is relayed.
            if (action === 'record') {
                if (this._enabled)
                    this._dbus.emit_signal('Action', new GLib.Variant('(ss)', [action, id]));
                return;
            }
            try {
                await this._prepare(id, action === 'edit' ? 'region' : action, sender);
            } catch (error) {
                const request = this._requests.get(id);
                if (!request)
                    return;
                request.error = error.bridgeCode ? error : failure('helper_failed', 'GNOME could not freeze the hotkey source');
            }
            if (this._enabled && this._requests.has(id))
                this._dbus.emit_signal('Action', new GLib.Variant('(ss)', [action, id]));
        } catch {
            // No resident bridge: binding has no capture or clipboard effect.
            this._drop(id);
        }
    }

    // put.gd's tray menu, drawn by the Shell's AppIndicator host, keeps its grab
    // while the binding runs (it is allowed in POPUP mode, #341) and stays over
    // a selection and in its picture (#924). Close it, as put.gd's own Qt tray
    // menu closes; desktop and window captures keep menus in the picture.
    // Only a menu whose StatusNotifierItem belongs to the resident is closed.
    _closeTrayMenu(sender) {
        const menu = Main.panel?.menuManager?.activeMenu;
        const busName = menu?.isOpen ? menu.sourceActor?._indicator?.busName : null;
        if (typeof busName !== 'string' || !busName)
            return false;
        try {
            if (this._clientPid(busName) !== this._clientPid(sender))
                return false;
        } catch {
            return false;
        }
        // BoxPointer.PopupAnimation.NONE: gone before the selection or freeze.
        menu.close(0);
        return true;
    }

    PrepareAsync([id, action], invocation) {
        this._run(invocation, sender => this._prepare(id, action, sender));
    }

    // The resident, its registered views helper, and every other process it
    // started directly (the Edit & upload editor, the recording helper) own
    // put.gd's windows. Mutter's PID comes from the Wayland socket.
    _residentWindow(window, sender) {
        const pid = window.get_pid();
        const resident = this._clientPid(sender);
        const uiPid = this._uiProcessOwner === sender ? this._uiProcess : 0;
        return pid === resident || (uiPid > 0 && pid === uiPid) || (pid > 0 && this._parentPid(pid) === resident);
    }

    _clientPid(sender) {
        return Gio.DBus.session.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'GetConnectionUnixProcessID', new GLib.Variant('(s)', [sender]),
            new GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, 1000, null).deep_unpack()[0];
    }

    AcquireAsync([id, json], invocation) {
        this._run(invocation, async sender => {
            const request = this._get(id, sender);
            check(!request.started, 'bridge_protocol', 'Source request has already been consumed');
            this._alive(request);
            request.started = true;
            request.limits = limitsFrom(json);
            if (request.action === 'clipboard')
                return this._clipboard(request);
            return this._capture(request);
        });
    }

    async _capture(request) {
        const limits = request.limits;
        check(JSON.stringify(await this._layout(request.cancellable)) === JSON.stringify(request.layout),
            'display_changed', 'Display layout changed before capture');
        const parts = [];
        const frames = [];
        let total = 0;
        let pixelCount = 0;
        let selection;
        let frozen = null;
        if (request.action === 'window') {
            check(request.window.get_compositor_private() === request.actor && !request.window.minimized &&
                JSON.stringify(rectangle(request.window.get_frame_rect())) === JSON.stringify(request.frame),
            'capture_target', 'Frozen external window disappeared or changed geometry');
            request.stage = 'window-texture';
            // paint_to_content renders at the ceiled resource scale: a 1.5x head
            // gave the 2x backing grid. Render at the window's head scale instead,
            // on the same output pixel grid as a desktop capture of that head.
            const head = request.layout[request.window.get_monitor()];
            check(head, 'display_changed', 'The focused window has no monitor');
            const grid = RegionSelection.outputGrid([request.actor.x, request.actor.y, request.actor.width, request.actor.height], head);
            const [width, height] = grid.pixels;
            check(width > 0 && height > 0 && width * height <= limits.imagePixels && width * height * 4 <= limits.imageBytes,
                'input_excessive', 'Frozen window exceeds image limits');
            const texture = this._paintWindow(request.actor, grid.logical, width, height);
            const stream = Gio.MemoryOutputStream.new_resizable();
            request.stage = 'window-encode';
            await Shell.Screenshot.composite_to_stream(texture, 0, 0, width, height, 1, null, 0, 0, 1, stream);
            this._alive(request);
            check(JSON.stringify(rectangle(request.window.get_frame_rect())) === JSON.stringify(request.frame),
                'capture_target', 'Frozen window geometry changed during capture');
            const data = bytesOf(stream);
            // The texture includes the compositor's frame/shadow; use its
            // logical extents instead of recapturing the current focus.
            selection = grid.logical;
            frames.push({identity: `window-${request.window.get_stable_sequence()}`, logical: selection,
                native: [0, 0, width, height], density: width / selection[2], rotation: 0, offset: 0, length: data.length});
            parts.push(data);
            total = data.length;
        } else {
            check(request.layout.length > 0, 'capture_geometry', 'No enabled monitor is available');
            const xs = request.layout.map(m => m.logical[0]);
            const ys = request.layout.map(m => m.logical[1]);
            const right = request.layout.map(m => m.logical[0] + m.logical[2]);
            const bottom = request.layout.map(m => m.logical[1] + m.logical[3]);
            selection = [Math.min(...xs), Math.min(...ys), Math.max(...right) - Math.min(...xs), Math.max(...bottom) - Math.min(...ys)];
            if (request.action === 'region') {
                check(!this._selector, 'busy_selecting', 'Another GNOME region selection is active');
                // Screenshot first (#629): read the whole stage before the
                // selector opens, so open menus stay in the frame it shows.
                if (limits.regionMode === 'screenshot_first') {
                    request.stage = 'region-freeze';
                    const [content, scale] = await new Shell.Screenshot().screenshot_stage_to_content();
                    this._alive(request);
                    check(content && Number.isFinite(scale) && scale > 0, 'capture_failed', 'GNOME could not freeze the desktop');
                    frozen = {content, scale, texture: content.get_texture()};
                }
                const selector = new RegionSelector(false, request.layout.map(m => ({x: m.logical[0], y: m.logical[1], width: m.logical[2], height: m.logical[3], density: frozen ? frozen.scale : m.density})), request.limits.outcome, frozen?.content ?? null, request.limits.appearance ?? {});
                this._selector = request.selector = selector;
                request.stage = 'region-select';
                const selected = await selector.selectAsync();
                request.selector = null;
                this._selector = null;
                this._alive(request);
                check(selected, 'cancelled', 'GNOME region selection cancelled');
                selection = rectangle(selected);
            }
            // selectAsync removes its actors and modal grab before resolving.
            // Live selection reads the desktop now; Screenshot first cuts each
            // monitor from the frozen stage texture, with no second capture.
            for (const monitor of request.layout) {
                const [x, y, width, height] = monitor.logical;
                const density = frozen ? frozen.scale : monitor.density;
                const estimated = Math.ceil(width * density) * Math.ceil(height * density);
                check(estimated <= limits.imagePixels - pixelCount && estimated * 4 <= limits.imageBytes - pixelCount * 4,
                    'input_excessive', 'Monitor capture exceeds image limits');
                const stream = Gio.MemoryOutputStream.new_resizable();
                request.stage = 'monitor-capture';
                if (frozen) {
                    const [left, top, w, h] = RegionSelection.textureArea([x, y, width, height], frozen.scale,
                        frozen.texture.get_width(), frozen.texture.get_height());
                    check(w > 0 && h > 0, 'display_changed', 'Frozen desktop does not cover the monitor');
                    await Shell.Screenshot.composite_to_stream(frozen.texture, left, top, w, h, frozen.scale, null, 0, 0, 1, stream);
                } else {
                    await new Shell.Screenshot().screenshot_area(x, y, width, height, stream);
                }
                this._alive(request);
                const data = bytesOf(stream);
                const [pixelWidth, pixelHeight] = this._pngSize(data);
                pixelCount += pixelWidth * pixelHeight;
                check(pixelCount <= limits.imagePixels && pixelCount * 4 <= limits.imageBytes && data.length <= limits.imageBytes - total,
                    'input_excessive', 'Monitor frames exceed image limits');
                frames.push({...monitor, native: [0, 0, pixelWidth, pixelHeight], density: pixelWidth / width,
                    offset: total, length: data.length});
                total += data.length;
                parts.push(data);
            }
        }
        this._alive(request);
        check(JSON.stringify(await this._layout(request.cancellable)) === JSON.stringify(request.layout),
            'display_changed', 'Display layout changed during capture');
        check(total <= limits.imageBytes, 'input_excessive', 'Capture PNG exceeds image limits');
        request.data = new Uint8Array(total);
        let offset = 0;
        for (const part of parts) {
            request.data.set(part, offset);
            offset += part.length;
        }
        request.metadata = {version: 1, requestId: request.id, mime: 'application/x-putgd-capture', bytes: total, frames, selection};
        request.actor = request.window = null;
        return request.metadata;
    }

    // Mutter's window snapshot (create_framebuffer_from_window_actor) at an
    // explicit scale: [x, y, width, height] stage logical onto width x height pixels.
    _paintWindow(actor, [x, y, width, height], pixelWidth, pixelHeight) {
        const context = global.stage.context.get_backend().get_cogl_context();
        const texture = Cogl.Texture2D.new_with_size(context, pixelWidth, pixelHeight);
        check(texture, 'capture_failed', 'GNOME could not allocate the window texture');
        texture.set_auto_mipmap(false);
        const framebuffer = Cogl.Offscreen.new_with_texture(texture);
        try {
            framebuffer.allocate();
        } catch {
            throw failure('capture_failed', 'GNOME could not allocate the window framebuffer');
        }
        framebuffer.clear4f(Cogl.BufferBit.COLOR, 0, 0, 0, 0);
        framebuffer.orthographic(0, 0, width, height, 0, 1);
        framebuffer.translate(-x, -y, 0);
        actor.inhibit_culling();
        try {
            const paint = Clutter.PaintContext.new_for_framebuffer(framebuffer, null, Clutter.PaintFlag.NONE,
                actor.get_color_state());
            actor.paint(paint);
            paint.destroy();
        } finally {
            actor.uninhibit_culling();
        }
        return texture;
    }

    _pngSize(data) {
        check(data.length >= 24 && data[0] === 137 && data[1] === 80 && data[2] === 78 && data[3] === 71,
            'bridge_protocol', 'Shell returned an invalid PNG');
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        return [view.getUint32(16), view.getUint32(20)];
    }

    async _clipboard(request) {
        check(request.revision === this._revision, 'clipboard_changed', 'Clipboard changed before source acquisition');
        const offered = this._selection.get_mimetypes(Meta.SelectionType.SELECTION_CLIPBOARD) ?? [];
        const candidates = [];
        if (offered.includes('text/uri-list'))
            candidates.push('text/uri-list');
        const images = offered.filter(m => m.startsWith('image/') && request.limits.imageMimes?.includes(m));
        images.sort((a, b) => Number(b === 'image/png') - Number(a === 'image/png'));
        candidates.push(...images);
        for (const mime of ['text/plain;charset=utf-8', 'UTF8_STRING', 'text/plain']) {
            if (offered.includes(mime))
                candidates.push(mime);
        }
        check(candidates.length > 0, 'source_unsupported', 'Clipboard has no supported files, image, or plain text');
        for (const mime of candidates) {
            const bound = mime.startsWith('image/') ? request.limits.imageBytes : request.limits.textBytes;
            const stream = Gio.MemoryOutputStream.new_resizable();
            // Read bound+1: reaching it proves an excessive offer without an
            // unlimited allocation or a silently truncated accepted payload.
            await new Promise((resolve, reject) => {
                request.transferTimer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 5000, () => {
                    request.transferTimer = 0;
                    request.cancellable.cancel();
                    reject(failure('transfer_timeout', 'Clipboard owner exceeded the five-second transfer deadline'));
                    return GLib.SOURCE_REMOVE;
                });
                request.stage = 'clipboard-transfer';
                this._selection.transfer_async(Meta.SelectionType.SELECTION_CLIPBOARD, mime, bound + 1,
                    stream, request.cancellable, (selection, result) => {
                        if (request.transferTimer)
                            GLib.source_remove(request.transferTimer);
                        request.transferTimer = 0;
                        try {
                            selection.transfer_finish(result);
                            resolve();
                        } catch {
                            reject(failure('transfer_timeout', 'Clipboard owner did not complete its bounded transfer'));
                        }
                    });
            });
            this._alive(request);
            check(request.revision === this._revision, 'clipboard_changed', 'Clipboard owner changed during source acquisition');
            const data = bytesOf(stream);
            check(data.length <= bound, 'input_excessive', 'Clipboard offer exceeds resource limits');
            if (mime === 'text/uri-list') {
                const text = new TextDecoder().decode(data);
                const uris = text.split(/\r?\n/).map(s => s.trim()).filter(s => s && !s.startsWith('#'));
                // Remote-only URI offers fall through to the owner's actual
                // image/text representation; mixed lists remain a batch error.
                if (!uris.some(uri => uri.startsWith('file:')))
                    continue;
                check(uris.length <= request.limits.maxFiles, 'input_excessive', 'Clipboard file offer exceeds file-count limits');
            }
            if (data.length === 0)
                continue;
            request.data = data;
            request.metadata = {version: 1, requestId: request.id, mime, bytes: data.length};
            return request.metadata;
        }
        throw failure('source_unsupported', 'Clipboard offers no usable local files, image, or plain text');
    }

    ReadAsync([id, offset, maximum], invocation) {
        this._run(invocation, sender => {
            const request = this._get(id, sender);
            check(request.data && maximum > 0 && maximum <= CHUNK && offset < request.data.length,
                'bridge_protocol', 'Transfer offset or chunk size is invalid');
            return {version: 1, requestId: id, offset,
                data: GLib.base64_encode(request.data.subarray(offset, Math.min(offset + maximum, request.data.length)))};
        });
    }

    ReleaseAsync([id], invocation) {
        this._run(invocation, sender => {
            this._get(id, sender);
            this._drop(id);
            return {};
        });
    }

    BeginCopyAsync([id, mime, length], invocation) {
        this._run(invocation, sender => {
            check(['image/png', 'text/plain;charset=utf-8', 'text/uri-list'].includes(mime),
                'source_unsupported', 'Unsupported clipboard result MIME');
            check(length > 0 && length <= 1099511627776, 'input_excessive', 'Clipboard result exceeds bridge limits');
            const request = this._new(id, sender);
            request.mime = mime;
            request.copyEpoch = ++this._copyEpoch;
            request.revision = this._revision;
            request.expected = length;
            request.parts = [];
            request.received = 0;
            return {};
        });
    }

    WriteCopyAsync([id, offset, encoded], invocation) {
        this._run(invocation, sender => {
            const request = this._get(id, sender);
            check(request.parts && encoded.length <= 4 * Math.ceil(CHUNK / 3) && /^[A-Za-z0-9+/]*={0,2}$/.test(encoded),
                'bridge_protocol', 'Clipboard result chunk is invalid');
            const data = GLib.base64_decode(encoded);
            check(data.length > 0 && data.length <= CHUNK && GLib.base64_encode(data) === encoded &&
                offset === request.received && data.length <= request.expected - request.received,
            'bridge_protocol', 'Clipboard result transfer offset is invalid');
            request.parts.push(data);
            request.received += data.length;
            return {};
        });
    }

    CommitCopyAsync([id], invocation) {
        this._run(invocation, sender => {
            const request = this._get(id, sender);
            check(request.parts && request.received === request.expected, 'bridge_protocol', 'Clipboard result transfer is incomplete');
            check(request.copyEpoch === this._copyEpoch && request.revision === this._revision,
                'copy_suppressed', 'Clipboard changed or a newer invocation superseded result copying');
            const data = new Uint8Array(request.expected);
            let offset = 0;
            for (const part of request.parts) {
                data.set(part, offset);
                offset += part.length;
            }
            request.stage = 'clipboard-own';
            St.Clipboard.get_default().set_content(St.ClipboardType.CLIPBOARD, request.mime, new GLib.Bytes(data));
            request.parts = null;
            return {clipboardRevision: this._revision};
        });
    }

    // Screen recording: the resident asks for a rectangle before the helper
    // starts the Mutter stream, so the region is chosen with the same selector
    // as screenshots. Release(id) cancels it.
    SelectAreaAsync([selection], invocation) {
        this._run(invocation, async sender => {
            check(!this._selector, 'busy_selecting', 'Another GNOME region selection is active');
            check(selection.length <= 4096, 'bridge_protocol', 'Invalid region appearance');
            const options = selection.startsWith('{') ? JSON.parse(selection) : {id: selection};
            const id = options.id;
            const request = this._new(id, sender);
            try {
                request.action = 'select';
                request.stage = 'region-select';
                const selector = new RegionSelector(true, null, '', null, options.appearance ?? {});
                this._selector = request.selector = selector;
                const selected = await selector.selectAsync();
                request.selector = null;
                this._selector = null;
                this._alive(request);
                check(selected, 'cancelled', 'GNOME region selection cancelled');
                this._finish(id);
                return {version: 1, requestId: id, rect: rectangle(selected)};
            } finally {
                // _run receives the JSON envelope, not this decoded request ID.
                // Release a revoked grab or failed selector immediately as well.
                this._drop(id);
            }
        });
    }

    _finish(id) {
        const request = this._requests.get(id);
        if (request?.timer)
            GLib.source_remove(request.timer);
        this._requests.delete(id);
    }

    _parentPid(pid) {
        try {
            const [, bytes] = GLib.file_get_contents(`/proc/${pid}/stat`);
            const text = new TextDecoder().decode(bytes);
            // "pid (comm) state ppid ...": comm may hold spaces and parentheses.
            return Number(text.slice(text.lastIndexOf(')') + 2).split(' ')[1]);
        } catch {
            return 0;
        }
    }

    // Only a control bar owned by a direct child of the authenticated resident.
    _recordingBar(sender, pid) {
        const resident = this._clientPid(sender);
        const bars = global.get_window_actors().map(actor => actor.meta_window).filter(window =>
            window && window.get_title() === RECORDING_BAR_TITLE && (!pid || window.get_pid() === pid) &&
            this._parentPid(window.get_pid()) === resident);
        return bars.length === 1 ? bars[0] : null;
    }

    async _placeRecordingBar(sender, pid, rect) {
        let window = this._recordingBar(sender, pid);
        // Qt maps the bar a frame after the helper shows it.
        for (let waited = 0; !window && waited < 2000; waited += 50) {
            await new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, 50, () => {
                resolve();
                return GLib.SOURCE_REMOVE;
            }));
            check(this._enabled, 'helper_unavailable', 'GNOME extension disabled');
            window = this._recordingBar(sender, pid);
        }
        if (!window)
            return {placed: false, outside: false};
        const pause = ms => new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
            resolve();
            return GLib.SOURCE_REMOVE;
        }));
        // Mutter places a new window when it is first shown and would undo an
        // earlier move: wait until it is shown, then confirm the position stays.
        for (let waited = 0; window.is_hidden() && waited < 1000; waited += 50)
            await pause(50);
        const frame = window.get_frame_rect();
        const areas = Main.layoutManager.monitors.map((_monitor, index) => rectangle(Main.layoutManager.getWorkAreaForMonitor(index)));
        // A later state of the same frame keeps the first decision, so a bar
        // that had to sit inside stays in the corner as its handle.
        const earlier = this._barFollow?.window === window && this._barFollow.rect.every((v, i) => v === rect[i]) ? this._barFollow : null;
        const target = barPlacement(rect, [frame.width, frame.height], areas, earlier ? earlier.outside : true);
        if (!window.is_above())
            window.make_above();
        let actual = null;
        for (let attempt = 0; attempt < 5; attempt++) {
            window.move_frame(true, target.rect[0], target.rect[1]);
            await pause(100);
            check(this._enabled, 'helper_unavailable', 'GNOME extension disabled');
            actual = rectangle(window.get_frame_rect());
            if (actual[0] === target.rect[0] && actual[1] === target.rect[1])
                break;
        }
        this._followRecordingBar(window, rect, earlier ? earlier.outside : target.outside);
        return {placed: true, outside: !overlaps(actual, rect), rect: actual};
    }

    // The helper resizes its bar when the state changes (wider while recording,
    // a handle when it has to sit inside); place it again on every size change
    // so it never runs past its monitor and the dim strips keep its hole.
    _followRecordingBar(window, rect, outside) {
        if (this._barFollow?.window !== window) {
            this._unfollowRecordingBar();
            const id = window.connect('size-changed', () => this._replaceRecordingBar());
            this._barFollow = {window, id};
        }
        Object.assign(this._barFollow, {rect, outside});
    }

    _replaceRecordingBar() {
        const follow = this._barFollow;
        if (!follow || !this._enabled)
            return;
        const frame = follow.window.get_frame_rect();
        const areas = Main.layoutManager.monitors.map((_monitor, index) => rectangle(Main.layoutManager.getWorkAreaForMonitor(index)));
        const target = barPlacement(follow.rect, [frame.width, frame.height], areas, follow.outside);
        if (frame.x !== target.rect[0] || frame.y !== target.rect[1])
            follow.window.move_frame(true, target.rect[0], target.rect[1]);
        this._recordingFrame?.moveBar(target.rect);
    }

    _unfollowRecordingBar() {
        if (!this._barFollow)
            return;
        try {
            this._barFollow.window.disconnect(this._barFollow.id);
        } catch {
            // The window is already gone.
        }
        this._barFollow = null;
    }

    ShowRecordingFrameAsync([json], invocation) {
        this._run(invocation, async sender => {
            check(json.length <= 4096, 'bridge_protocol', 'Recording frame exceeds the control-message limit');
            const frame = JSON.parse(json);
            const rect = frame.rect;
            check(Array.isArray(rect) && rect.length === 4 && rect.every(Number.isSafeInteger) && rect[2] > 0 && rect[3] > 0 &&
                RECORDING_STATES.includes(frame.state) && RECORDING_KINDS.includes(frame.kind) &&
                (frame.pid === undefined || (Number.isSafeInteger(frame.pid) && frame.pid >= 0)),
            'bridge_protocol', 'Invalid recording frame');
            const bar = await this._placeRecordingBar(sender, frame.pid || 0, rect);
            check(this._enabled && !Main.sessionMode.isLocked, 'helper_unavailable', 'GNOME user session is unavailable');
            this._recordingFrame ??= new RecordingFrame();
            this._recordingFrame.show(rect, frame.kind, frame.state !== 'ready', bar.placed ? bar.rect : null);
            return {version: 1, bar};
        });
    }

    HideRecordingFrameAsync(_args, invocation) {
        this._run(invocation, () => {
            this._hideRecordingFrame();
            return {};
        });
    }

    _hideRecordingFrame() {
        this._unfollowRecordingBar();
        this._recordingFrame?.destroy();
        this._recordingFrame = null;
    }

    _unbind() {
        for (const id of this._bindings.keys()) {
            Main.wm.allowKeybinding(Meta.external_binding_name_for_action(id), Shell.ActionMode.NONE);
            global.display.ungrab_accelerator(id);
        }
        this._bindings.clear();
    }

    BindAsync([json], invocation) {
        this._run(invocation, () => {
            check(json.length <= 16384, 'bridge_protocol', 'Shortcut configuration exceeds control-message limits');
            const requested = JSON.parse(json);
            this._unbind();
            const bindings = {};
            for (const action of ACTIONS) {
                const entry = requested[action] ?? {};
                const status = {requested: String(entry.requested ?? ''), bound: false};
                if (!status.requested) {
                    status.error = 'disabled';
                } else if (!entry.accelerator || entry.accelerator.length > 256) {
                    status.error = 'shortcut_unsupported';
                } else {
                    const id = global.display.grab_accelerator(entry.accelerator, Meta.KeyBindingFlags.IGNORE_AUTOREPEAT);
                    if (id === Meta.KeyBindingAction.NONE) {
                        status.error = 'shortcut_conflict';
                    } else {
                        this._bindings.set(id, action);
                        // POPUP too: a Shell menu that is open, such as the tray
                        // menu, must not swallow a capture shortcut (#341).
                        Main.wm.allowKeybinding(Meta.external_binding_name_for_action(id), Shell.ActionMode.NORMAL | Shell.ActionMode.POPUP);
                        status.bound = true;
                        status.actual = status.requested;
                    }
                }
                bindings[action] = status;
            }
            return {bindings};
        });
    }
}
