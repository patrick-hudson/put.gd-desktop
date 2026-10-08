#!/usr/bin/env python3
"""Independent GTK4 clipboard owner/paste witness. Synthetic data only."""
import argparse
import io
import json
from pathlib import Path

import cairo
import gi

gi.require_version('Gtk', '4.0')
gi.require_version('Gdk', '4.0')
gi.require_foreign('cairo')
from gi.repository import Gdk, Gio, GLib, Gtk

COLORS = [(220, 30, 40), (20, 180, 70), (35, 70, 220), (235, 190, 20)]


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


class Fixture(Gtk.Application):
    def __init__(self, output):
        super().__init__(application_id='org.putgd.SyntheticGtkClipboard',
                         flags=Gio.ApplicationFlags.NON_UNIQUE)
        self.output = output
        self.window = None
        self.connect('activate', self.activate_fixture)

    def activate_fixture(self, _application):
        self.output.mkdir(parents=True, exist_ok=True)
        for index in (1, 2):
            (self.output / f'synthetic-{index}.txt').write_bytes(
                f'putgd synthetic file {index}\n'.encode())
        self.window = Gtk.ApplicationWindow(application=self)
        self.window.set_title('put.gd independent GTK clipboard fixture')
        self.window.set_default_size(640, 420)
        drawing = Gtk.DrawingArea()
        drawing.set_draw_func(self.paint)
        self.window.set_child(drawing)
        controller = Gtk.EventControllerKey()
        controller.connect('key-pressed', self.key_pressed)
        self.window.add_controller(controller)
        self.window.present()
        write_json(self.output / 'gtk-ready.json', {'schemaVersion': 1,
            'client': 'GTK', 'version': f'{Gtk.get_major_version()}.{Gtk.get_minor_version()}',
            'displayBackend': type(Gdk.Display.get_default()).__name__})

    def paint(self, _drawing, context, _width, _height):
        for index, color in enumerate(COLORS):
            context.set_source_rgb(*(value / 255 for value in color))
            context.rectangle((index % 2) * 320, (index // 2) * 160, 320, 160)
            context.fill()

    def key_pressed(self, _controller, keyval, _keycode, state):
        mods = state & (Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.ALT_MASK |
                        Gdk.ModifierType.SHIFT_MASK)
        source_mods = (Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.ALT_MASK |
                       Gdk.ModifierType.SHIFT_MASK)
        clipboard = self.window.get_clipboard()
        key = Gdk.keyval_to_lower(keyval)
        if mods == source_mods and key in (Gdk.KEY_t, Gdk.KEY_i, Gdk.KEY_f):
            if key == Gdk.KEY_t:
                mime, data = 'text/plain;charset=utf-8', b'putgd synthetic clipboard text'
            elif key == Gdk.KEY_i:
                surface = cairo.ImageSurface(cairo.FORMAT_RGB24, 96, 64)
                context = cairo.Context(surface)
                context.set_source_rgb(*(value / 255 for value in COLORS[0]))
                context.paint()
                stream = io.BytesIO()
                surface.write_to_png(stream)
                mime, data = 'image/png', stream.getvalue()
            else:
                mime = 'text/uri-list'
                data = ''.join((self.output / f'synthetic-{index}.txt').as_uri() + '\r\n'
                               for index in (1, 2)).encode()
            if not clipboard.set_content(Gdk.ContentProvider.new_for_bytes(mime, GLib.Bytes.new(data))):
                raise RuntimeError('GTK failed to own synthetic clipboard content')
            write_json(self.output / 'source.json', {'synthetic': True, 'client': 'GTK', 'mime': mime})
            return True
        if key == Gdk.KEY_v and mods == Gdk.ModifierType.CONTROL_MASK:
            clipboard.read_async(['text/uri-list', 'image/png', 'text/plain;charset=utf-8', 'text/plain'],
                                 GLib.PRIORITY_DEFAULT, None, self.paste_ready)
            return True
        return False

    def paste_ready(self, clipboard, response):
        try:
            stream, mime = clipboard.read_finish(response)
            data = bytearray()
            def consume(source, result):
                try:
                    chunk = source.read_bytes_finish(result).get_data()
                    if chunk:
                        if len(data) + len(chunk) > 4 * 1024 * 1024:
                            raise RuntimeError('Synthetic paste exceeds fixture limit')
                        data.extend(chunk)
                        source.read_bytes_async(65536, GLib.PRIORITY_DEFAULT, None, consume)
                        return
                    source.close(None)
                    value = {'schemaVersion': 1, 'client': 'GTK', 'formats': [mime]}
                    if mime == 'image/png':
                        (self.output / 'paste.png').write_bytes(data)
                        texture = Gdk.Texture.new_from_bytes(GLib.Bytes.new(data))
                        value['image'] = {'width': texture.get_width(), 'height': texture.get_height(), 'saved': True}
                    else:
                        value['text'] = bytes(data).decode('utf-8')
                        if mime == 'text/uri-list':
                            value['urls'] = [line for line in value['text'].splitlines() if line and not line.startswith('#')]
                    write_json(self.output / 'paste.json', value)
                except Exception as error:
                    write_json(self.output / 'paste.json', {'error': type(error).__name__})
            stream.read_bytes_async(65536, GLib.PRIORITY_DEFAULT, None, consume)
        except Exception as error:
            write_json(self.output / 'paste.json', {'error': type(error).__name__})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    options = parser.parse_args()
    Fixture(options.output.resolve()).run([])
