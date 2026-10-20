#!/usr/bin/env python3
"""Full-screen lab cover and marker player pinned to one named output, plus host-side coverage gates.

The physical-desktop recording checks cover the screen with a solid background
(magenta) and play the `putgd-record --av-marker` clip in front of it, so that a
recording or saved frame shows only the lab's own pixels. The old player,
`waylandsink fullscreen=true`, can't choose an output: on k72's two-output
Plasma desktop KWin put it on the other monitor in 4 of 7 runs, and those
recordings showed the host user's desktop (#165). This tool replaces it:

  cover_player.py outputs
      JSON list of the session's outputs (connector, logical rectangle, scale)
      and of its distinct logical outputs (mirrored outputs share one).
  cover_player.py play [--output NAME] [--color ff00ff] [--media CLIP | --pattern]
                       [--video-size 1280x720] [--audio-sink pulsesink]
                       [--audio-device NAME] [--title T]
      Opens a GTK 3 window full screen on that output (xdg_toplevel.set_fullscreen
      with the output's wl_output on Wayland, _NET_WM_FULLSCREEN_MONITORS on X11)
      and checks that the compositor really put it there at the output's size.
      Without --output it refuses when the session has more than one distinct
      logical output. Prints one JSON line per state on stdout: `covered` once the
      placement check passed, then `playing` (and `looped`) for the video. A
      failed placement check prints `{"ok": false, ...}`, closes the window and
      exits 3. The marker clip (--media, looped without gaps) or a scrolling
      pattern (--pattern) plays at --video-size in the middle of the cover.
      Reads commands on stdin: `media` starts the video (unless --media-now),
      `quit` (or end of input) closes the window.
  cover_player.py precheck --port 5900 [--output NAME] [--min-fraction 0.98]
      Grabs one frame from the host's loopback VNC tile, keeps it in memory
      only, and checks that the chosen output's rectangle is the cover colour.
      Exits 3 when it isn't: run it after `covered` and before recording.
  cover_player.py check-frame IMAGE [--rect X,Y,W,H] [--min-fraction 0.5]
                       [--crop OUT.png]
      Measures the cover colour's share of a saved frame (PPM, or anything
      ffmpeg reads, such as a PNG or a frame extracted from a clip). Below the
      threshold the file is deleted unopened and the command exits 3; with
      --crop a passing frame is cropped to the rectangle and the original
      deleted, so only the covered output can leave the host.

Run everything on the host, in the desktop session's environment
(WAYLAND_DISPLAY or DISPLAY, the session bus). `outputs`, `precheck` and
`check-frame` print sizes, fractions and hashes only, never pixels. Requires
python3-gi with GTK 3 for `play` (GTK 4 only for connector names on Wayland)
and GStreamer's gtksink for --media.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

MAGENTA = (255, 0, 255)
EXIT_UNCOVERED = 3

# GTK 4 knows each monitor's connector on Wayland and X11; GTK 3 only knows the
# model there (on X11 its model is the RandR output name). Run in a child
# process because GTK 3 and GTK 4 can't share one.
GTK4_MONITORS = r'''
import json, gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gtk
Gtk.init()
display = Gdk.Display.get_default()
if display is None:
    raise SystemExit("no display")
found = []
monitors = display.get_monitors()
for i in range(monitors.get_n_items()):
    m = monitors.get_item(i); g = m.get_geometry()
    found.append({"name": m.get_connector() or "", "model": m.get_model() or "",
                  "rect": [g.x, g.y, g.width, g.height], "scale": m.get_scale_factor()})
print(json.dumps(found))
'''


def emit(**fields):
    print(json.dumps(fields, sort_keys=True), flush=True)


def logical_outputs(monitors):
    """Group outputs by logical rectangle: mirrored outputs show the same pixels and count once."""
    groups = {}
    for monitor in monitors:
        groups.setdefault(tuple(monitor['rect']), []).append(monitor['name'] or monitor.get('model') or '?')
    return [{'rect': list(rect), 'names': names} for rect, names in groups.items()]


def choose_output(monitors, wanted=None):
    """The logical output to cover, or ValueError (unknown name, several outputs and none named, none at all)."""
    groups = logical_outputs(monitors)
    if not groups:
        raise ValueError('no_outputs: the session reports no monitors')
    if wanted:
        for group in groups:
            if wanted in group['names']:
                return group
        raise ValueError('unknown_output: %s is not one of %s' % (wanted, ', '.join(n for g in groups for n in g['names'])))
    if len(groups) > 1:
        raise ValueError('several_outputs: %d distinct logical outputs (%s); name one with --output'
                         % (len(groups), '; '.join('/'.join(g['names']) for g in groups)))
    return groups[0]


def session_monitors():
    """The session's monitors as dicts with name, rect and scale (GTK 4 if present, else GTK 3)."""
    try:
        result = subprocess.run([sys.executable, '-c', GTK4_MONITORS], capture_output=True, text=True, timeout=20)
        if result.returncode == 0:
            return json.loads(result.stdout)
    except (OSError, subprocess.TimeoutExpired, ValueError):
        pass
    gdk = gtk3()[1]
    display = gdk.Display.get_default()
    if display is None:
        raise SystemExit('no display: run in the desktop session environment')
    found = []
    for i in range(display.get_n_monitors()):
        m = display.get_monitor(i); g = m.get_geometry()
        found.append({'name': m.get_model() or '', 'model': m.get_model() or '',
                      'rect': [g.x, g.y, g.width, g.height], 'scale': m.get_scale_factor()})
    return found


def gtk3():
    import gi
    gi.require_version('Gtk', '3.0')
    gi.require_version('Gdk', '3.0')
    from gi.repository import Gdk, GLib, Gtk
    return Gtk, Gdk, GLib


def parse_color(text):
    text = text.lstrip('#')
    if len(text) != 6:
        raise argparse.ArgumentTypeError('colour must be RRGGBB')
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


def parse_size(text):
    width, _, height = text.partition('x')
    return int(width), int(height)


def parse_rect(text):
    parts = [int(p) for p in text.split(',')]
    if len(parts) != 4 or parts[2] <= 0 or parts[3] <= 0:
        raise argparse.ArgumentTypeError('rectangle must be X,Y,W,H')
    return parts


def coverage(width, height, pixels, layout, rect=None, color=MAGENTA, tolerance=40, samples=250_000):
    """Share of sampled pixels inside rect within tolerance of color.

    pixels is packed rows: layout 'rgb' (3 bytes) or 'bgrx' (4 bytes, the RFB
    client's format). A rect that leaves the frame counts its outside as
    uncovered, so a frame smaller than the output never passes."""
    x, y, w, h = rect or (0, 0, width, height)
    step = max(1, int(((w * h) / samples) ** 0.5))
    bpp, (ri, gi, bi) = (3, (0, 1, 2)) if layout == 'rgb' else (4, (2, 1, 0))
    total = hit = 0
    for row in range(y, y + h, step):
        cols = len(range(x, x + w, step))
        total += cols
        if row < 0 or row >= height:
            continue
        lo, hi = max(x, 0), min(x + w, width)
        if lo >= hi:
            continue
        first = lo + (x - lo) % step
        start = (row * width + first) * bpp
        end = (row * width + hi) * bpp
        line = pixels[start:end]
        stride = bpp * step
        for r, g, b in zip(line[ri::stride], line[gi::stride], line[bi::stride]):
            if abs(r - color[0]) <= tolerance and abs(g - color[1]) <= tolerance and abs(b - color[2]) <= tolerance:
                hit += 1
    return hit / total if total else 0.0


def read_frame(path):
    """(width, height, rgb bytes) of a binary PPM, or of the first frame ffmpeg decodes from any other file."""
    data = Path(path).read_bytes()
    if data.startswith(b'P6'):
        fields, pos = [], 2
        while len(fields) < 3:
            while data[pos:pos + 1].isspace():
                pos += 1
            if data[pos:pos + 1] == b'#':
                pos = data.index(b'\n', pos)
                continue
            end = pos
            while not data[end:end + 1].isspace():
                end += 1
            fields.append(int(data[pos:end])); pos = end
        width, height, maxval = fields
        if maxval != 255:
            raise ValueError('only 8-bit PPM frames are supported')
        return width, height, data[pos + 1:pos + 1 + width * height * 3]
    probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=width,height',
                            '-of', 'csv=p=0', str(path)], capture_output=True, text=True, check=True)
    width, height = (int(v) for v in probe.stdout.split(',')[:2])
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(path), '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                         capture_output=True, check=True).stdout
    return width, height, raw[:width * height * 3]


def write_png_crop(path, width, pixels, rect):
    import struct
    import zlib
    x, y, w, h = rect
    raw = bytearray()
    for row in range(y, y + h):
        raw.append(0)
        raw += pixels[(row * width + x) * 3:(row * width + x + w) * 3]
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


def check_frame(args):
    path = Path(args.image)
    try:
        width, height, pixels = read_frame(path)
        rect = args.rect or [0, 0, width, height]
        fraction = coverage(width, height, pixels, 'rgb', rect, args.color, args.tolerance)
    except Exception as error:  # an unreadable frame is never kept
        path.unlink(missing_ok=True)
        emit(ok=False, reason='unreadable: ' + type(error).__name__, deleted=True)
        return EXIT_UNCOVERED
    if fraction < args.min_fraction:
        path.unlink()
        emit(ok=False, reason='uncovered', fraction=round(fraction, 4), minFraction=args.min_fraction,
             rect=rect, size=[width, height], deleted=True)
        return EXIT_UNCOVERED
    kept = path
    if args.crop:
        x, y, w, h = rect
        if x < 0 or y < 0 or x + w > width or y + h > height:
            path.unlink()
            emit(ok=False, reason='crop_outside_frame', rect=rect, size=[width, height], deleted=True)
            return EXIT_UNCOVERED
        kept = Path(args.crop)
        write_png_crop(kept, width, pixels, rect)
        if kept.resolve() != path.resolve():
            path.unlink()
    emit(ok=True, fraction=round(fraction, 4), minFraction=args.min_fraction, rect=rect, size=[width, height],
         kept=str(kept), sha256=hashlib.sha256(kept.read_bytes()).hexdigest())
    return 0


def precheck(args):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import rfb
    output = choose_output(session_monitors(), args.output)
    client = rfb.RFB(args.port)
    try:
        width, height, pixels = client.frame()
    finally:
        client.close()
    fraction = coverage(width, height, pixels, 'bgrx', output['rect'], args.color, args.tolerance)
    ok = fraction >= args.min_fraction
    emit(ok=ok, check='precheck', output=output['names'], rect=output['rect'], framebuffer=[width, height],
         fraction=round(fraction, 4), minFraction=args.min_fraction)
    return 0 if ok else EXIT_UNCOVERED


def play(args):
    try:
        target = choose_output(session_monitors(), args.output)
    except ValueError as error:
        emit(ok=False, reason=str(error))
        return EXIT_UNCOVERED
    Gtk, Gdk, GLib = gtk3()
    display = Gdk.Display.get_default()
    index = next((i for i in range(display.get_n_monitors())
                  if list(geometry(display.get_monitor(i))) == target['rect']), None)
    if index is None:
        emit(ok=False, reason='output_not_in_gtk3', rect=target['rect'])
        return EXIT_UNCOVERED
    state = {'code': 0, 'covered': False, 'pipeline': None}
    window = Gtk.Window(title=args.title)
    window.set_decorated(False)
    css = Gtk.CssProvider()
    css.load_from_data(('window { background-color: #%02x%02x%02x; }' % args.color).encode())
    window.get_style_context().add_provider(css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    stack = Gtk.Stack()
    stack.add_named(Gtk.DrawingArea(), 'cover')
    window.add(stack)

    def fail(reason, **extra):
        if state['code'] == 0:
            state['code'] = EXIT_UNCOVERED
            emit(ok=False, reason=reason, output=target['names'], rect=target['rect'], **extra)
        quit_player()
        return False

    def quit_player(*_):
        if state['pipeline'] is not None:
            from gi.repository import Gst
            state['pipeline'].set_state(Gst.State.NULL)
        Gtk.main_quit()
        return False

    def placed():
        gdk_window = window.get_window()
        if gdk_window is None or not gdk_window.get_state() & Gdk.WindowState.FULLSCREEN:
            return None
        monitor = display.get_monitor_at_window(gdk_window)
        size = [window.get_allocated_width(), window.get_allocated_height()]
        return list(geometry(monitor)), size

    def verify():
        if state['covered'] or state['code']:
            return False
        result = placed()
        if result is None:
            return True
        rect, size = result
        if rect == target['rect'] and size == target['rect'][2:]:
            state['covered'] = True
            hide_pointer(window, Gdk)
            emit(ok=True, state='covered', output=target['names'], rect=target['rect'], monitorIndex=index)
            if args.media_now:
                start_media()
        return not state['covered']

    def placement_deadline():
        if not state['covered']:
            result = placed()
            fail('not_fullscreen_on_output', placed=result[0] if result else None, size=result[1] if result else None)
        return False

    def start_media():
        if state['pipeline'] is not None or not (args.media or args.pattern):
            return
        try:
            state['pipeline'] = media_pipeline(args, stack, emit, fail)
        except Exception as error:
            fail('media_unavailable: %s' % error)

    def on_stdin(channel, condition):
        line = channel.readline()
        command = line.strip() if line else 'quit'
        if command == 'quit':
            quit_player()
            return False
        if command == 'media' and state['covered']:
            start_media()
        return True

    window.connect('delete-event', quit_player)
    recheck = lambda *_: GLib.idle_add(lambda: verify() and False) and False
    window.connect('window-state-event', recheck)
    window.connect('size-allocate', recheck)
    window.show_all()
    window.fullscreen_on_monitor(window.get_screen(), index)
    GLib.timeout_add(250, verify)
    GLib.timeout_add(args.place_timeout * 1000, placement_deadline)
    GLib.io_add_watch(GLib.IOChannel.unix_new(sys.stdin.fileno()), GLib.PRIORITY_DEFAULT, GLib.IOCondition.IN | GLib.IOCondition.HUP, on_stdin)
    for signal_number in (2, 15):
        GLib.unix_signal_add(GLib.PRIORITY_HIGH, signal_number, quit_player)
    Gtk.main()
    return state['code']


def geometry(monitor):
    g = monitor.get_geometry()
    return g.x, g.y, g.width, g.height


def hide_pointer(window, Gdk):
    window.get_window().set_cursor(Gdk.Cursor.new_for_display(window.get_display(), Gdk.CursorType.BLANK_CURSOR))


def media_pipeline(args, stack, emit, fail):
    """The marker clip (looped without gaps) or a scrolling pattern, at its own size in the middle of the cover.

    The video keeps its size (1280x720 by default) instead of filling the
    output, so every recorded frame of a 1920x1080 output stays more than half
    cover colour and can pass `check-frame`; the marker's flashes still move
    the frame's mean luma far past `--av-sync`'s threshold."""
    import gi
    gi.require_version('Gst', '1.0')
    from gi.repository import Gst
    Gst.init(None)
    if Gst.ElementFactory.find('gtksink') is None:
        raise RuntimeError('GStreamer gtksink is missing (gstreamer1.0-gtk3 / gstreamer1-plugins-good-gtk)')
    width, height = args.video_size
    if args.pattern:
        description = ('videotestsrc is-live=true pattern=smpte horizontal-speed=6 '
                       '! video/x-raw,width=%d,height=%d,framerate=30/1 ! videoconvert ! gtksink name=video' % (width, height))
    else:
        audio = args.audio_sink + (' device=%s' % args.audio_device if args.audio_device else '')
        description = ('filesrc location="%s" ! matroskademux name=d '
                       'd. ! queue ! jpegdec ! videoconvert ! videoscale ! video/x-raw,width=%d,height=%d '
                       '! gtksink name=video sync=true '
                       'd. ! queue ! audioconvert ! audioresample ! %s' % (args.media.replace('"', ''), width, height, audio))
    pipeline = Gst.parse_launch(description)
    widget = pipeline.get_by_name('video').props.widget
    widget.set_size_request(width, height)
    widget.set_halign(3)  # Gtk.Align.CENTER
    widget.set_valign(3)
    stack.add_named(widget, 'media')
    widget.show()
    stack.set_visible_child_name('media')
    looping = {'armed': args.pattern}
    bus = pipeline.get_bus()
    bus.add_signal_watch()

    def on_message(_bus, message):
        if message.type == Gst.MessageType.ASYNC_DONE and not looping['armed']:
            # A segment seek makes the demuxer post SEGMENT_DONE instead of EOS at the end.
            looping['armed'] = True
            pipeline.seek_simple(Gst.Format.TIME, Gst.SeekFlags.FLUSH | Gst.SeekFlags.SEGMENT, 0)
            pipeline.set_state(Gst.State.PLAYING)
        elif message.type == Gst.MessageType.SEGMENT_DONE:
            pipeline.seek_simple(Gst.Format.TIME, Gst.SeekFlags.SEGMENT, 0)
            emit(ok=True, state='looped')
        elif message.type == Gst.MessageType.EOS:
            emit(ok=True, state='eos')
        elif message.type == Gst.MessageType.ERROR:
            fail('media_error: %s' % message.parse_error()[0].message)
        elif message.type == Gst.MessageType.STATE_CHANGED and message.src == pipeline:
            if message.parse_state_changed()[1] == Gst.State.PLAYING:
                emit(ok=True, state='playing', media='pattern' if args.pattern else os.path.basename(args.media))
    bus.connect('message', on_message)
    pipeline.set_state(Gst.State.PLAYING if args.pattern else Gst.State.PAUSED)
    return pipeline


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('outputs')
    p = sub.add_parser('play')
    p.add_argument('--output')
    p.add_argument('--color', type=parse_color, default=MAGENTA)
    p.add_argument('--media', help='a putgd-record --av-marker clip, looped')
    p.add_argument('--pattern', action='store_true', help='a scrolling SMPTE pattern instead of --media')
    p.add_argument('--video-size', type=parse_size, default=(1280, 720))
    p.add_argument('--media-now', action='store_true', help='start the video as soon as the cover is placed')
    p.add_argument('--audio-sink', default='pulsesink')
    p.add_argument('--audio-device')
    p.add_argument('--title', default='put.gd lab cover')
    p.add_argument('--place-timeout', type=int, default=10)
    c = sub.add_parser('precheck')
    c.add_argument('--port', type=int, required=True, help="the host's loopback VNC port showing the real desktop")
    c.add_argument('--output')
    c.add_argument('--color', type=parse_color, default=MAGENTA)
    c.add_argument('--tolerance', type=int, default=40)
    c.add_argument('--min-fraction', type=float, default=0.98)
    f = sub.add_parser('check-frame')
    f.add_argument('image')
    f.add_argument('--rect', type=parse_rect)
    f.add_argument('--color', type=parse_color, default=MAGENTA)
    f.add_argument('--tolerance', type=int, default=40)
    f.add_argument('--min-fraction', type=float, default=0.5)
    f.add_argument('--crop')
    args = parser.parse_args(argv)
    if args.command == 'outputs':
        monitors = session_monitors()
        emit(outputs=monitors, logical=logical_outputs(monitors))
        return 0
    return {'play': play, 'precheck': precheck, 'check-frame': check_frame}[args.command](args)


if __name__ == '__main__':
    sys.exit(main())
