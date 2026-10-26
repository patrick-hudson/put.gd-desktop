#!/usr/bin/env python3
"""Independent synthetic GTK4 clipboard owner for the physical campaign.

No desktop content is read. The explicit ``paste`` command reads only content
that the campaign has just asked put.gd to copy, retaining a digest/geometry
receipt, never text or payload bytes in stdout. JSON commands arrive on stdin.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import zlib


def png(path, index, width=640, height=360, noise=False):
    import random
    generator = random.Random(379000 + index)
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        if noise:
            raw.extend(generator.randbytes(width * 3))
        else:
            for x in range(width):
                raw.extend(((x + index * 31) % 256, (y + index * 57) % 256,
                            ((x // 32 + y // 24 + index) % 2) * 180 + 30))
    chunk = lambda kind, data: (struct.pack('>I', len(data)) + kind + data +
                                struct.pack('>I', zlib.crc32(kind + data)))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def clipboard(root):
    import gi
    gi.require_version('Gdk', '4.0')
    gi.require_version('Gtk', '4.0')
    from gi.repository import Gdk, GLib, Gtk
    Gtk.init()
    display = Gdk.Display.get_default()
    if display is None:
        raise RuntimeError('native_display_unavailable')
    selection = display.get_clipboard()
    loop = GLib.MainLoop()

    def emit(**value):
        print(json.dumps(value), flush=True)

    def owned_file(value):
        path = Path(value)
        info = path.lstat()
        if (not path.is_file() or path.is_symlink() or info.st_uid != os.getuid()
                or not path.resolve().is_relative_to(root) or info.st_size > 64 * 1024**2):
            raise ValueError('clipboard_source_not_private_synthetic_file')
        return path

    def paste_ready(clipboard, result, image):
        try:
            if image:
                texture = clipboard.read_texture_finish(result)
                # The paste witness's own file is fixture data, never a payload log.
                path = root / 'independent-paste.png'
                texture.save_to_png(str(path))
                from PIL import Image
                pixels = Image.open(path).convert('RGBA')
                emit(ok=True, kind='image', width=pixels.width, height=pixels.height,
                     pixelSha256=hashlib.sha256(pixels.tobytes()).hexdigest())
            else:
                text = clipboard.read_text_finish(result)
                emit(ok=True, kind='text', sha256=hashlib.sha256((text or '').encode()).hexdigest())
        except Exception:
            emit(ok=False, error='independent_paste_failed')

    def incoming(_source, _condition):
        import sys
        line = sys.stdin.readline()
        if not line:
            loop.quit()
            return False
        try:
            value = json.loads(line)
            if value['action'] == 'file':
                path = owned_file(value['path'])
                data = GLib.Bytes.new((path.resolve().as_uri() + '\r\n').encode())
                if not selection.set_content(Gdk.ContentProvider.new_for_bytes('text/uri-list', data)):
                    raise RuntimeError('clipboard_ownership_failed')
                emit(ok=True, kind='file-uri', file=path.name)
            elif value['action'] == 'paste-image':
                selection.read_texture_async(None, lambda c, r: paste_ready(c, r, True))
            elif value['action'] == 'paste-text':
                selection.read_text_async(None, lambda c, r: paste_ready(c, r, False))
            elif value['action'] == 'clear':
                selection.set_content(None)
                emit(ok=True, kind='cleared')
            elif value['action'] == 'quit':
                selection.set_content(None)
                loop.quit()
                emit(ok=True, kind='quit')
                return False
            else:
                raise ValueError('unsupported_command')
        except Exception:
            emit(ok=False, error='clipboard_command_refused')
        return True

    import sys
    GLib.io_add_watch(sys.stdin, GLib.IO_IN | GLib.IO_HUP, incoming)
    emit(ok=True, kind='ready', backend=type(display).__name__)
    loop.run()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    clipboard(args.root.resolve(strict=True))
