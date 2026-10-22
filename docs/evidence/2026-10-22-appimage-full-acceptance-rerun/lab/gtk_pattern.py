#!/usr/bin/env python3
"""Lab cover for a VM without GStreamer's gtksink (Aurora): a full-screen magenta
window with SMPTE-75-style bars scrolling in a 640x360 box in its middle.
--still draws the cover only (the static control). Quits on SIGTERM or 'quit' on stdin."""
import argparse, signal, sys, gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib

BARS = [(191, 191, 191), (191, 191, 0), (0, 191, 191), (0, 191, 0), (191, 0, 191), (191, 0, 0), (0, 0, 191)]
p = argparse.ArgumentParser(); p.add_argument('--still', action='store_true'); p.add_argument('--size', default='640x360')
args = p.parse_args(); bw, bh = (int(v) for v in args.size.split('x'))
state = {'offset': 0}

def draw(widget, cr):
    w, h = widget.get_allocated_width(), widget.get_allocated_height()
    cr.set_source_rgb(1, 0, 1); cr.paint()
    if args.still: return False
    x0, y0 = (w - bw) // 2, (h - bh) // 2
    cr.rectangle(x0, y0, bw, bh); cr.clip()
    bar = bw / len(BARS)
    shift, part = divmod(state['offset'], bar)
    for i in range(-1, len(BARS) + 1):
        r, g, b = BARS[int(i - shift) % len(BARS)]
        cr.set_source_rgb(r / 255, g / 255, b / 255)
        cr.rectangle(x0 + i * bar + part, y0, bar + 1, bh); cr.fill()
    return False

def tick(area):
    state['offset'] += 6; area.queue_draw(); return True

win = Gtk.Window(title='put.gd acceptance cover'); area = Gtk.DrawingArea(); area.connect('draw', draw); win.add(area)
win.connect('destroy', Gtk.main_quit); win.fullscreen(); win.show_all()
if not args.still: GLib.timeout_add(33, tick, area)
GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal.SIGTERM, lambda *a: (Gtk.main_quit(), False)[1])
def on_stdin(channel, cond):
    line = channel.readline()
    if not line or line.strip() == 'quit': Gtk.main_quit(); return False
    return True
GLib.io_add_watch(GLib.IOChannel.unix_new(sys.stdin.fileno()), GLib.PRIORITY_DEFAULT, GLib.IOCondition.IN | GLib.IOCondition.HUP, on_stdin)
print('{"ok": true, "state": "covered"}', flush=True)
if not args.still: print('{"ok": true, "state": "playing"}', flush=True)
Gtk.main()
