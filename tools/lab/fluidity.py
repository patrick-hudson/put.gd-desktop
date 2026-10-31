#!/usr/bin/env python3
"""Fluid-motion measurements for the recorder and the desktop it records (#6).

A clip that "looks choppy" has frames that repeat or jump. This tool makes that
countable. `fixture` shows a moving picture whose every displayed frame carries
its frame number in a machine-readable strip, drawn at the display's own frame
clock (GdkFrameClock, so it keeps the compositor's pace on X11 and Wayland);
`analyze` reads those numbers back out of a recording and reports how many
distinct source frames the clip holds per second, how long it ever stalled on
one frame and how far it ever skipped; `ticks` reports the fixture's own frame
intervals, so a desktop that stuttered while the recorder ran is visible too.

  fluidity.py fixture [--output NAME] [--seconds N] [--ticks FILE] [--speed PX]
      Full screen on the named output (any output when the session has one):
      the frame-number strip (two reference cells, then 16 Gray-coded bits,
      24 px cells) at the top left, a bar sweeping right at --speed px per
      frame, and a bouncing disc. Prints one JSON line when covered, then runs
      for --seconds (default 30) or until stdin closes. --ticks appends one
      line per frame: frame number and GdkFrameClock time in microseconds.
  fluidity.py analyze CLIP [--fps N] [--strip X,Y,CELL] [--json FILE]
      Decodes the clip (GStreamer), finds the strip (or takes --strip in clip
      pixels), reads every frame's number and reports: clip frames and rate,
      distinct source frames per second, the count of consecutive frame pairs
      whose source frame advanced by 0 (repeat), 1 (fluid), 2 (one display
      frame skipped) and more, the longest stall and skip in milliseconds, and
      the clip's own timestamp gaps. --fps is the recording's configured rate.
  fluidity.py ticks FILE [--hz 60] [--json FILE]
      Frame intervals of a --ticks log: mean, late frames (over 1.5 nominal
      intervals), longest gap. Compare a run while recording with one without.

Needs PyGObject with GTK 3 (fixture) and GStreamer (fixture, analyze).
"""
import argparse
import json
import os
import statistics
import sys
import time

CELL = 24
BITS = 16
STRIP_CELLS = 2 + BITS


def log(message):
    print(time.strftime('%H:%M:%S ') + message, file=sys.stderr, flush=True)


def gray(n):
    return n ^ (n >> 1)


def ungray(g):
    n = 0
    while g:
        n ^= g
        g >>= 1
    return n


# --- fixture -----------------------------------------------------------------

def fixture(args):
    import gi
    gi.require_version('Gtk', '3.0')
    gi.require_version('Gdk', '3.0')
    from gi.repository import Gdk, GLib, Gtk

    display = Gdk.Display.get_default()
    monitors = [display.get_monitor(i) for i in range(display.get_n_monitors())]
    chosen = None
    for monitor in monitors:
        if args.output is None or monitor.get_model() == args.output or (monitor.get_connector and monitor.get_connector() == args.output):
            chosen = monitor
            break
    if chosen is None:
        for monitor in monitors:
            geometry = monitor.get_geometry()
            if args.output and f'{geometry.width}x{geometry.height}+{geometry.x}+{geometry.y}' == args.output:
                chosen = monitor
    if chosen is None:
        names = [m.get_model() or '?' for m in monitors]
        print(json.dumps({'ok': False, 'error': 'output not found', 'outputs': names}), flush=True)
        return 3
    if args.output is None and len(monitors) > 1:
        print(json.dumps({'ok': False, 'error': 'several outputs; pass --output', 'outputs': [m.get_model() for m in monitors]}), flush=True)
        return 3
    geometry = chosen.get_geometry()

    window = Gtk.Window(title='put.gd fluidity fixture')
    area = Gtk.DrawingArea()
    window.add(area)
    state = {'frame': 0, 'ticks': open(args.ticks, 'a') if args.ticks else None, 'started': None, 'covered': False}

    def draw(_widget, cr):
        width, height = area.get_allocated_width(), area.get_allocated_height()
        frame = state['frame']
        # Mid-grey background: the strip's reference cells must be the only
        # near-black and near-white runs the analyzer meets on their row.
        cr.set_source_rgb(0.45, 0.45, 0.47)
        cr.rectangle(0, 0, width, height)
        cr.fill()
        # The strip: reference black, reference white, then the Gray-coded frame number, LSB first.
        code = gray(frame & (2 ** BITS - 1))
        for index in range(STRIP_CELLS):
            if index == 0:
                value = 0
            elif index == 1:
                value = 1
            else:
                value = (code >> (index - 2)) & 1
            cr.set_source_rgb(value, value, value)
            cr.rectangle(CELL + index * CELL, CELL, CELL, CELL)
            cr.fill()
        # A bar sweeping right at a constant speed, wrapping; a disc bouncing.
        bar_x = (frame * args.speed) % (width + 40) - 40
        cr.set_source_rgb(0.95, 0.55, 0.1)
        cr.rectangle(bar_x, height * 0.3, 40, height * 0.4)
        cr.fill()
        period = max(60, (height - 160) // 4)
        phase = frame % (2 * period)
        disc_y = 80 + (phase if phase < period else 2 * period - phase) * 4
        cr.set_source_rgb(0.2, 0.75, 0.95)
        cr.arc(width * 0.8, disc_y, 36, 0, 6.2832)
        cr.fill()
        cr.set_source_rgb(0.85, 0.85, 0.85)
        cr.select_font_face('monospace')
        cr.set_font_size(28)
        cr.move_to(CELL, 3 * CELL + 10)
        cr.show_text(f'frame {frame}')
        return False

    def tick(_widget, clock):
        state['frame'] += 1
        if state['ticks']:
            state['ticks'].write(f'{state["frame"]} {clock.get_frame_time()}\n')
        area.queue_draw()
        return GLib.SOURCE_CONTINUE

    def check_placement():
        gdk_window = window.get_window()
        if not gdk_window:
            return True
        placed = display.get_monitor_at_window(gdk_window)
        width, height = window.get_size()
        ok = placed is not None and placed.get_geometry().equal(geometry) and width >= geometry.width - 2 and height >= geometry.height - 2
        if ok and not state['covered']:
            state['covered'] = True
            print(json.dumps({'ok': True, 'state': 'covered', 'output': chosen.get_model(), 'rect': [geometry.x, geometry.y, geometry.width, geometry.height],
                              'strip': [CELL, CELL, CELL], 'scale': chosen.get_scale_factor()}), flush=True)
            if state['ticks']:
                state['ticks'].write(f'# covered {time.time()} rect {geometry.x},{geometry.y},{geometry.width},{geometry.height}\n')
                state['ticks'].flush()
        elif not ok and state['started'] and time.monotonic() - state['started'] > 5 and not state['covered']:
            print(json.dumps({'ok': False, 'error': 'the compositor did not place the window on the output', 'size': [width, height]}), flush=True)
            Gtk.main_quit()
            state['failed'] = True
            return False
        return not state['covered']

    area.connect('draw', draw)
    area.add_tick_callback(tick)
    window.connect('destroy', Gtk.main_quit)
    window.set_decorated(False)
    window.fullscreen_on_monitor(display.get_default_screen(), monitors.index(chosen))
    window.show_all()
    state['started'] = time.monotonic()
    GLib.timeout_add(100, check_placement)
    if args.seconds > 0:
        GLib.timeout_add(int(args.seconds * 1000), Gtk.main_quit)

    def stdin_closed(_source, _condition):
        line = sys.stdin.readline()
        if not line or line.strip() == 'quit':
            Gtk.main_quit()
            return False
        return True

    GLib.io_add_watch(sys.stdin, GLib.IO_IN | GLib.IO_HUP, stdin_closed)
    Gtk.main()
    if state['ticks']:
        state['ticks'].close()
    return 3 if state.get('failed') else 0


# --- analyze -----------------------------------------------------------------

def decode_frames(path):
    """Yields (pts_ns, width, height, stride, bytes) for every frame, GRAY8."""
    import gi
    gi.require_version('Gst', '1.0')
    from gi.repository import Gst
    Gst.init(None)
    pipeline = Gst.parse_launch(f'filesrc location="{path}" ! decodebin ! videoconvert ! video/x-raw,format=GRAY8 ! appsink name=sink sync=false max-buffers=4 drop=false')
    sink = pipeline.get_by_name('sink')
    pipeline.set_state(Gst.State.PLAYING)
    bus = pipeline.get_bus()
    try:
        while True:
            sample = sink.emit('try-pull-sample', Gst.SECOND)
            if sample is None:
                message = bus.pop_filtered(Gst.MessageType.ERROR | Gst.MessageType.EOS)
                if message is not None or sink.get_property('eos'):
                    if message is not None and message.type == Gst.MessageType.ERROR:
                        error, _debug = message.parse_error()
                        raise RuntimeError(f'decode failed: {error.message}')
                    break
                continue
            caps = sample.get_caps().get_structure(0)
            width, height = caps.get_value('width'), caps.get_value('height')
            buffer = sample.get_buffer()
            ok, info = buffer.map(Gst.MapFlags.READ)
            if not ok:
                raise RuntimeError('cannot map a decoded frame')
            try:
                data = bytes(info.data)
            finally:
                buffer.unmap(info)
            stride = len(data) // height
            yield buffer.pts, width, height, stride, data
    finally:
        pipeline.set_state(Gst.State.NULL)


def find_strip(width, height, stride, data):
    """Locates the reference black/white cell pair: returns (x, y, cell) or None."""
    best = None
    for y in range(2, height // 3, 2):
        row = data[y * stride:y * stride + width]
        x = 0
        while x < width - 16:
            if row[x] < 48 and (x == 0 or row[x - 1] >= 48):
                start = x
                while x < width and row[x] < 48:
                    x += 1
                dark = x - start
                bright = 0
                while x + bright < width and row[x + bright] > 208:
                    bright += 1
                # The white reference may be followed by white data cells, so the
                # bright run is at least a cell; the dark run is exactly one.
                if dark >= 8 and bright >= dark * 0.8:
                    cell = dark
                    # The strip is `cell` tall: confirm the row above the top edge is background.
                    if best is None or best[2] < cell:
                        best = (start, y, cell)
                    break
                x += bright
            else:
                x += 1
    if best is None:
        return None
    x0, y_hit, cell = best
    # Walk up to the strip's top edge, then centre vertically.
    y = y_hit
    while y > 0 and data[(y - 1) * stride + x0 + cell // 2] < 48:
        y -= 1
    return x0, y, cell


def read_counter(width, stride, data, strip):
    x0, y0, cell = strip
    cy = y0 + cell // 2
    def sample(index):
        cx = x0 + index * cell + cell // 2
        total = 0
        for dy in (-2, -1, 0, 1, 2):
            base = (cy + dy) * stride
            for dx in (-2, -1, 0, 1, 2):
                total += data[base + cx + dx]
        return total / 25
    black, white = sample(0), sample(1)
    if white - black < 64 or x0 + STRIP_CELLS * cell > width:
        return None
    threshold = (black + white) / 2
    code = 0
    for bit in range(BITS):
        if sample(2 + bit) > threshold:
            code |= 1 << bit
    return ungray(code)


def analyze_clip(clip, fps=0, strip=None, frames_source=None):
    """The fluidity report of one clip (a dict); frames_source overrides decoding (tests)."""
    frames = []
    unreadable = 0
    for pts, width, height, stride, data in (frames_source if frames_source is not None else decode_frames(clip)):
        if strip is None:
            strip = find_strip(width, height, stride, data)
            if strip is None:
                unreadable += 1
                frames.append((pts, None))
                continue
        counter = read_counter(width, stride, data, strip)
        if counter is None:
            unreadable += 1
        frames.append((pts, counter))
    if not frames:
        return {'ok': False, 'error': 'no frames decoded', 'clip': os.path.abspath(clip)}
    pts = [p for p, _ in frames if p is not None]
    counters = [c for _, c in frames]
    nominal = fps
    duration_s = (pts[-1] - pts[0]) / 1e9 + (1 / nominal if nominal else 0) if len(pts) > 1 else 0
    gaps_ms = [(b - a) / 1e6 for a, b in zip(pts, pts[1:])]
    deltas = {'repeat': 0, 'fluid': 0, 'skip1': 0, 'skip2plus': 0, 'backwards': 0}
    stall_run = longest_stall = 0
    longest_skip = 0
    previous = None
    for counter in counters:
        if counter is None:
            continue
        if previous is not None:
            step = (counter - previous) % (2 ** BITS)
            if step > 2 ** (BITS - 1):
                deltas['backwards'] += 1
                step = 0
            if step == 0:
                deltas['repeat'] += 1
                stall_run += 1
                longest_stall = max(longest_stall, stall_run)
            else:
                stall_run = 0
                if step == 1:
                    deltas['fluid'] += 1
                elif step == 2:
                    deltas['skip1'] += 1
                else:
                    deltas['skip2plus'] += 1
                longest_skip = max(longest_skip, step - 1)
        previous = counter
    readable = [c for c in counters if c is not None]
    distinct = len(set(readable))
    frame_ms = 1000 / nominal if nominal else (statistics.median(gaps_ms) if gaps_ms else 0)
    report = {
        'ok': True, 'clip': os.path.abspath(clip), 'strip': list(strip) if strip else None,
        'frames': len(frames), 'durationS': round(duration_s, 3), 'clipFps': round(len(frames) / duration_s, 2) if duration_s else None,
        'nominalFps': nominal, 'unreadable': unreadable,
        'distinctSourceFrames': distinct, 'distinctPerSecond': round(distinct / duration_s, 2) if duration_s else None,
        'pairs': deltas, 'fluidFraction': round(deltas['fluid'] / max(1, sum(deltas.values())), 4),
        # A repeat means the display frame the recorder would have shown next was not captured in time.
        'longestStallFrames': longest_stall, 'longestStallMs': round(longest_stall * frame_ms, 1),
        'longestSkipFrames': longest_skip, 'longestSkipMs': round(longest_skip * frame_ms, 1),
        'ptsGapMs': {'min': round(min(gaps_ms), 2), 'median': round(statistics.median(gaps_ms), 2), 'max': round(max(gaps_ms), 2),
                     'offGrid': sum(1 for g in gaps_ms if frame_ms and abs(g - frame_ms) > frame_ms / 4)} if gaps_ms else None,
    }
    return report


def analyze(args):
    strip = tuple(int(v) for v in args.strip.split(',')) if args.strip else None
    report = analyze_clip(args.clip, args.fps, strip)
    text = json.dumps(report, indent=2)
    print(text)
    if args.json:
        with open(args.json, 'w') as handle:
            handle.write(text + '\n')
    return 0 if report.get('ok') else 1


# --- ticks -------------------------------------------------------------------

def ticks_report(path, hz=60, since=None, until=None):
    """Frame intervals of a fixture --ticks log, optionally between two GdkFrameClock times (microseconds)."""
    times = []
    with open(path) as handle:
        for line in handle:
            if line.startswith('#') or not line.strip():
                continue
            parts = line.split()
            if len(parts) >= 2:
                stamp = int(parts[1])
                if (since is None or stamp >= since) and (until is None or stamp <= until):
                    times.append(stamp)
    if len(times) < 2:
        return {'ok': False, 'error': 'fewer than two ticks', 'file': os.path.abspath(path)}
    intervals = [(b - a) / 1000 for a, b in zip(times, times[1:])]
    nominal = 1000 / hz
    late = [i for i in intervals if i > 1.5 * nominal]
    return {'ok': True, 'file': os.path.abspath(path), 'frames': len(times), 'seconds': round((times[-1] - times[0]) / 1e6, 2),
            'nominalMs': round(nominal, 2), 'meanMs': round(statistics.mean(intervals), 3), 'maxMs': round(max(intervals), 2),
            'lateFrames': len(late), 'lateFraction': round(len(late) / len(intervals), 4),
            'missedFrames': sum(int(round(i / nominal)) - 1 for i in late)}


def ticks(args):
    report = ticks_report(args.file, args.hz)
    text = json.dumps(report, indent=2)
    print(text)
    if args.json:
        with open(args.json, 'w') as handle:
            handle.write(text + '\n')
    return 0 if report.get('ok') else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    commands = parser.add_subparsers(dest='command', required=True)
    f = commands.add_parser('fixture')
    f.add_argument('--output')
    f.add_argument('--seconds', type=float, default=30)
    f.add_argument('--ticks')
    f.add_argument('--speed', type=int, default=8)
    f.set_defaults(run=fixture)
    a = commands.add_parser('analyze')
    a.add_argument('clip')
    a.add_argument('--fps', type=int, default=0)
    a.add_argument('--strip')
    a.add_argument('--json')
    a.set_defaults(run=analyze)
    t = commands.add_parser('ticks')
    t.add_argument('file')
    t.add_argument('--hz', type=float, default=60)
    t.add_argument('--json')
    t.set_defaults(run=ticks)
    args = parser.parse_args(argv)
    return args.run(args)


if __name__ == '__main__':
    sys.exit(main())
