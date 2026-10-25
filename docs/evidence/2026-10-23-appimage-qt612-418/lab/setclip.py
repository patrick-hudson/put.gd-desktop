#!/usr/bin/env python3
"""Own the clipboard with a text/uri-list of the given files until killed (Wayland; GTK 4).

A Wayland client may set the selection only while it has keyboard focus, so a small window is
shown, the content is set once it is active, and the window is hidden. Prints "ready" when set."""
import sys
from pathlib import Path
import gi
gi.require_version('Gtk', '4.0'); gi.require_version('Gdk', '4.0')
from gi.repository import Gtk, Gdk, GLib

uris = ''.join(Path(p).resolve().as_uri() + '\r\n' for p in sys.argv[1:]).encode()


def activate(app):
    window = Gtk.ApplicationWindow(application=app, title='put.gd 418 clipboard', default_width=240, default_height=60)
    window.set_child(Gtk.Label(label='put.gd #418 clipboard'))

    def active(*_):
        if not window.is_active() or getattr(window, 'done', False): return
        window.done = True
        provider = Gdk.ContentProvider.new_for_bytes('text/uri-list', GLib.Bytes.new(uris))
        window.get_clipboard().set_content(provider)
        app.hold()
        GLib.timeout_add(400, lambda: (window.set_visible(False), print('ready', flush=True), False)[-1])
    window.connect('notify::is-active', active)
    window.present()


app = Gtk.Application(application_id='gd.put.Qt418Clipboard')
app.connect('activate', activate)
app.run([])
