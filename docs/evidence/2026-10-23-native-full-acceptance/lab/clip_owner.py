#!/usr/bin/env python3
"""clip_owner.py SECONDS [SEED]: a GTK 4 window that owns a synthetic PNG (a seeded colour grid) on the clipboard for
SECONDS, so an older build's Upload clipboard has something to upload without capturing the screen. Synthetic data only."""
import io, random, sys
import cairo, gi
gi.require_version('Gtk', '4.0'); gi.require_version('Gdk', '4.0')
from gi.repository import Gdk, Gio, GLib, Gtk

seconds, seed = float(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 1
rnd = random.Random(seed)
surface = cairo.ImageSurface(cairo.FORMAT_RGB24, 320, 200); ctx = cairo.Context(surface)
for x in range(0, 320, 40):
    for y in range(0, 200, 40):
        ctx.set_source_rgb(rnd.random(), rnd.random(), rnd.random()); ctx.rectangle(x, y, 40, 40); ctx.fill()
stream = io.BytesIO(); surface.write_to_png(stream); png = stream.getvalue()


def activate(app):
    win = Gtk.ApplicationWindow(application=app, title='put.gd acceptance clipboard owner'); win.set_default_size(240, 120); win.present()
    def own():
        ok = win.get_clipboard().set_content(Gdk.ContentProvider.new_for_bytes('image/png', GLib.Bytes.new(png)))
        print('owned' if ok else 'not-owned', len(png), flush=True); return False
    GLib.timeout_add(600, own)
    GLib.timeout_add(int(seconds * 1000), lambda: (app.quit(), False)[1])


app = Gtk.Application(application_id='gd.put.AcceptanceClipOwner', flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect('activate', activate); app.run([])
