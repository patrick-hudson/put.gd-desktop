#!/usr/bin/env python3
"""Native GTK checkerboard/paste witness for the multi-output geometry campaign.

Synthetic pixels only. One fullscreen window per output. Ctrl+V performs an
independent native clipboard paste; Ctrl+Shift+T owns the cancellation sentinel.
The JSON output records actual GDK monitor geometry, never a requested layout.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import sys
import time
import uuid

COLORS = [(220, 30, 40), (20, 180, 70), (35, 70, 220), (235, 190, 20),
          (30, 200, 210), (180, 40, 200), (230, 110, 30), (70, 90, 110)]
TILE = 128
SENTINEL = 'putgd multi-output cancellation sentinel'


def geometry_ready(monitors):
    return bool(monitors) and all(m.get('fullscreen') and m.get('draws', 0) > 0 and
        (m.get('allocatedWidth'), m.get('allocatedHeight')) == (m['width'], m['height']) and
        m.get('placedRect') == [m[k] for k in ('x', 'y', 'width', 'height')] for m in monitors)


def save(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def main():
    import gi
    gi.require_version('Gtk', '3.0')
    gi.require_version('Gdk', '3.0')
    gi.require_foreign('cairo')
    from gi.repository import Gdk, GLib, Gtk
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--motion', action='store_true')
    p.add_argument('--attempt', default=uuid.uuid4().hex)
    a = p.parse_args()
    a.output.mkdir(mode=0o700, parents=True, exist_ok=False)
    display = Gdk.Display.get_default()
    if display is None:
        raise SystemExit('No native display')
    monitors = []
    windows = []
    draws = []
    tick = [0]
    def paint(_area, cr, index):
        draws[index] += 1
        w, h = _area.get_allocated_width(), _area.get_allocated_height()
        for y in range(0, h, TILE):
            for x in range(0, w, TILE):
                color = COLORS[(index * 4 + (x // TILE) % 2 + 2 * ((y // TILE) % 2)) % len(COLORS)]
                cr.set_source_rgb(*(v / 255 for v in color))
                cr.rectangle(x, y, TILE, TILE)
                cr.fill()
        if a.motion:
            cr.set_source_rgb(1, 1, 1)
            # Every 160px recording target sees motion, including on a wide
            # output where a single sweeping bar can miss the sampled region.
            phase = (tick[0] * 13) % TILE
            for x in range(-TILE + phase, w, TILE):
                cr.rectangle(x, 0, 32, h)
            cr.fill()
        return False
    def key(_window, event):
        state = event.state & (Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.SHIFT_MASK | Gdk.ModifierType.MOD1_MASK)
        clipboard = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
        if state == Gdk.ModifierType.CONTROL_MASK and Gdk.keyval_to_lower(event.keyval) == Gdk.KEY_v:
            image = clipboard.wait_for_image()
            if image is not None:
                image.savev(str(a.output / 'paste.png'), 'png', [], [])
                result = {'kind': 'image', 'width': image.get_width(), 'height': image.get_height()}
            else:
                result = {'kind': 'text', 'text': clipboard.wait_for_text()}
            save(a.output / 'paste.json', result)
            return True
        if state == (Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.SHIFT_MASK) and Gdk.keyval_to_lower(event.keyval) == Gdk.KEY_t:
            clipboard.set_text(SENTINEL, -1)
            return True
        return False
    for index in range(display.get_n_monitors()):
        draws.append(0)
        monitor = display.get_monitor(index)
        g = monitor.get_geometry()
        monitors.append({'index': index, 'x': g.x, 'y': g.y, 'width': g.width, 'height': g.height,
                         'integerScale': monitor.get_scale_factor(), 'model': monitor.get_model()})
        window = Gtk.Window(title=f'put.gd geometry head {index}')
        window.set_decorated(False)
        window.move(g.x, g.y)
        area = Gtk.DrawingArea()
        area.connect('draw', paint, index)
        window.add(area)
        window.connect('key-press-event', key)
        window.show_all()
        window.fullscreen_on_monitor(Gdk.Screen.get_default(), index)
        windows.append(window)
    deadline = time.monotonic() + 10
    failure = []
    def ready():
        actual = []
        for i, window in enumerate(windows):
            if window.get_window() is None:
                if time.monotonic() < deadline:
                    return True
                failure.append('Compositor did not map the fixture before deadline')
                Gtk.main_quit()
                return False
            placed = display.get_monitor_at_window(window.get_window()).get_geometry()
            actual.append({**monitors[i], 'allocatedWidth': window.get_allocated_width(),
                           'allocatedHeight': window.get_allocated_height(),
                           'placedRect': [placed.x, placed.y, placed.width, placed.height],
                           'draws': draws[i],
                           'fullscreen': bool(window.get_window().get_state() & Gdk.WindowState.FULLSCREEN)})
        complete = geometry_ready(actual)
        if not complete:
            if time.monotonic() < deadline:
                return True
            save(a.output / 'readiness-failure.json', {'attempt': a.attempt, 'pid': os.getpid(), 'monitors': actual})
            failure.append('Compositor did not acknowledge separately drawn fullscreen windows before deadline')
            Gtk.main_quit()
            return False
        save(a.output / 'ready.json', {'backend': type(display).__name__, 'monitors': actual,
                                     'attempt': a.attempt, 'pid': os.getpid(),
                                     'colors': COLORS, 'tile': TILE, 'motion': a.motion})
        return False
    GLib.timeout_add(100, ready)
    def animate():
        tick[0] += 1
        if a.motion:
            for window in windows:
                window.queue_draw()
        return True
    GLib.timeout_add(33, animate)
    def counts():
        save(a.output / 'frame-counts.json', {'draws': draws, 'ticks': tick[0]})
        return True
    GLib.timeout_add(1000, counts)
    # A normal signal handler also works when input EOF arrives during an output change.
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, lambda: (Gtk.main_quit(), False)[1])
    Gtk.main()
    if failure:
        raise SystemExit(failure[0])


if __name__ == '__main__':
    main()
