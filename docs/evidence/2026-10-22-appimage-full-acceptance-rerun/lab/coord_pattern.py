#!/usr/bin/env python3
"""Static full-screen cover whose every pixel encodes its own position (#333 region origin and size).

pixel(x, y) = (x & 255, y & 255, ((x >> 8) & 15) | ((y >> 8) & 15) << 4), so any capture of it decodes
to the screen coordinates it came from. GTK 3 + cairo only. Quits on SIGTERM or 'quit' on stdin.
"""
import signal, sys, gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib
import cairo

cache = {}


def surface(w, h):
    if (w, h) not in cache:
        stride = cairo.ImageSurface.format_stride_for_width(cairo.FORMAT_RGB24, w)
        data = bytearray(stride * h)
        for y in range(h):
            g, hi = y & 255, ((y >> 8) & 15) << 4
            row = bytearray(stride)
            for x in range(w):
                o = x * 4
                row[o] = ((x >> 8) & 15) | hi  # B (little-endian BGRX)
                row[o + 1] = g                 # G
                row[o + 2] = x & 255           # R
            data[y * stride:(y + 1) * stride] = row
        cache[(w, h)] = cairo.ImageSurface.create_for_data(data, cairo.FORMAT_RGB24, w, h, stride)
    return cache[(w, h)]


def decode(r, g, b):
    return r | (b & 15) << 8, g | (b >> 4) << 8


def draw(widget, cr):
    w, h = widget.get_allocated_width(), widget.get_allocated_height()
    cr.set_source_surface(surface(w, h), 0, 0); cr.get_source().set_filter(cairo.FILTER_NEAREST); cr.paint()
    return False


if __name__ == '__main__':
    win = Gtk.Window(title='put.gd acceptance coordinate cover'); area = Gtk.DrawingArea(); area.connect('draw', draw); win.add(area)
    win.connect('destroy', Gtk.main_quit); win.fullscreen(); win.show_all()
    GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal.SIGTERM, lambda *a: (Gtk.main_quit(), False)[1])

    def on_stdin(channel, cond):
        line = channel.readline()
        if not line or line.strip() == 'quit': Gtk.main_quit(); return False
        return True
    GLib.io_add_watch(GLib.IOChannel.unix_new(sys.stdin.fileno()), GLib.PRIORITY_DEFAULT, GLib.IOCondition.IN | GLib.IOCondition.HUP, on_stdin)
    print('{"ok": true, "state": "covered"}', flush=True)
    Gtk.main()
