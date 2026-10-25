"""External PID-owned accessibility/input and X11 drawable observations.

No product hooks or checkpoint polling. Pixel-change times are observer upper
bounds, with probe costs and cadence retained; they are not compositor-present
timestamps or a count of every rendered frame. Imports do not open a display.
"""
import ctypes as C
import hashlib
import subprocess
import sys
import time
from pathlib import Path

from bench_ui_process import EvidenceError, identity, same_identity

_x_errors = []
@C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p)
def _x_error(_display, _event):
    _x_errors[:] = [True]
    return 0


class XImage(C.Structure):
    _fields_ = [('width', C.c_int), ('height', C.c_int), ('xoffset', C.c_int), ('format', C.c_int),
                ('data', C.c_void_p), ('byte_order', C.c_int), ('bitmap_unit', C.c_int),
                ('bitmap_bit_order', C.c_int), ('bitmap_pad', C.c_int), ('depth', C.c_int),
                ('bytes_per_line', C.c_int), ('bits_per_pixel', C.c_int),
                ('red_mask', C.c_ulong), ('green_mask', C.c_ulong), ('blue_mask', C.c_ulong)]


class Drawable:
    def __init__(self, window, owner):
        self.window, self.owner = int(window, 16), owner
        self.x = C.CDLL('libX11.so.6')
        self.x.XOpenDisplay.argtypes = [C.c_char_p]
        self.x.XOpenDisplay.restype = C.c_void_p
        self.x.XCloseDisplay.argtypes = [C.c_void_p]
        self.x.XGetImage.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int,
                                   C.c_uint, C.c_uint, C.c_ulong, C.c_int]
        self.x.XGetImage.restype = C.POINTER(XImage)
        self.x.XDestroyImage.argtypes = [C.POINTER(XImage)]
        self.x.XSetErrorHandler.argtypes = [type(_x_error)]
        self.x.XSetErrorHandler(_x_error)  # A disappearing owned window is a retained failure, not process exit.
        self.display = self.x.XOpenDisplay(None)
        if not self.display:
            raise EvidenceError('x11_observer_display_unavailable')
        self.probes = []

    def close(self):
        if self.display:
            self.x.XCloseDisplay(self.display)
            self.display = None

    def pixels(self, rectangle, alpha=False):
        if not same_identity(identity(self.owner['pid']), self.owner):
            raise EvidenceError('drawable_owner_changed')
        started = time.monotonic()
        _x_errors.clear()
        x, y, width, height = rectangle
        if min(x, y) < 0 or not 0 < width <= 4096 or not 0 < height <= 4096:
            raise EvidenceError('invalid_drawable_probe_rectangle')
        image = self.x.XGetImage(self.display, self.window, x, y, width, height, C.c_ulong(-1), 2)
        if not image or _x_errors:
            raise EvidenceError('drawable_pixels_unavailable')
        try:
            info = image.contents
            if (info.bits_per_pixel != 32 or info.byte_order != 0 or info.red_mask != 0xff0000
                    or info.green_mask != 0xff00 or info.blue_mask != 0xff):
                raise EvidenceError('unsupported_x11_observer_pixel_layout')
            if alpha and info.depth != 32:
                raise EvidenceError('selector_requires_argb32_drawable')
            raw = C.string_at(info.data, info.bytes_per_line * info.height)
            from PIL import Image
            pixels = Image.frombytes('RGBA' if alpha else 'RGB', (info.width, info.height), raw, 'raw',
                                     'BGRA' if alpha else 'BGRX', info.bytes_per_line)
        finally:
            self.x.XDestroyImage(image)
        self.probes.append({'completed': time.monotonic(), 'probeSeconds': time.monotonic() - started})
        return pixels

    def response(self, rectangle, action, seconds=5):
        """Separate native pixels from a synchronous action acknowledgement."""
        first_probe = len(self.probes)
        before = hashlib.sha256(self.pixels(rectangle).tobytes()).hexdigest()
        start = time.monotonic()
        action()
        acknowledged = time.monotonic()
        changes, last = [], before
        deadline = start + seconds
        while True:
            digest = hashlib.sha256(self.pixels(rectangle).tobytes()).hexdigest()
            observed = time.monotonic()
            if digest != last:
                changes.append((observed - start) * 1000)
                last = digest
            # Collect a bounded stream after first change to show subsequent
            # sampled changes. Never interpret a quiet interval as a frame.
            if changes and observed >= start + changes[0] / 1000 + .15:
                break
            if observed >= deadline:
                break
            time.sleep(.01)
        return {'acknowledgementMs': (acknowledged - start) * 1000,
                'firstObservedDrawableChangeUpperBoundMs': changes[0] if changes else None,
                'observedDrawableChangesMs': changes, 'observationSeconds': time.monotonic() - start,
                'probeCount': len(self.probes) - first_probe,
                'maximumProbeMs': max(row['probeSeconds'] for row in self.probes[first_probe:]) * 1000,
                'definition': 'native owned drawable digest; first sample after action acknowledgement; 10ms requested polling; no compositor presentation claim'}


def _visibility_target(window, owner, info=None):
    """Numeric facts about this pinned target only; discard command output."""
    import re
    started = time.monotonic()
    if not same_identity(identity(owner['pid']), owner):
        raise EvidenceError('visibility_owner_changed')
    if info is None:
        info = subprocess.check_output(['xwininfo', '-id', window], text=True, timeout=2,
                                       stderr=subprocess.DEVNULL)
    prop = subprocess.check_output(['xprop', '-id', window, '_NET_WM_PID'], text=True, timeout=2,
                                   stderr=subprocess.DEVNULL)
    if not re.search(r'=\s*' + str(owner['pid']) + r'\s*$', prop):
        raise EvidenceError('visibility_window_owner_changed')
    if 'Map State: IsViewable' not in info:
        raise EvidenceError('visibility_target_not_mapped')
    active = subprocess.check_output(['xprop', '-root', '_NET_ACTIVE_WINDOW'], text=True, timeout=2,
                                     stderr=subprocess.DEVNULL)
    identifiers = re.findall(r'0x[0-9a-fA-F]+', active)
    if len(identifiers) != 1:
        raise EvidenceError('visibility_active_window_unavailable')
    geometry = [int(re.search(pattern, info)[1]) for pattern in (
        r'Absolute upper-left X:\s+(-?\d+)', r'Absolute upper-left Y:\s+(-?\d+)',
        r'Width:\s+(\d+)', r'Height:\s+(\d+)')]
    return {'generationMatches': True, 'ownerPidMatches': True, 'mapped': True,
            'geometry': geometry, 'activeIsTarget': int(identifiers[0], 16) == int(window, 16),
            'probeMs': (time.monotonic() - started) * 1000}


def _park_pointer(point, owner, bounds):
    """Idempotent parking; all private X/Y reads and movement share five seconds."""
    import re
    if (len(point) != 2 or len(bounds) != 2 or
            any(type(value) is not int for value in (*point, *bounds)) or
            not all(0 <= value < maximum for value, maximum in zip(point, bounds))):
        raise EvidenceError('pointer_parking_invalid_coordinates')
    deadline = time.monotonic() + 5

    def remaining():
        if not same_identity(identity(owner['pid']), owner):
            raise EvidenceError('pointer_parking_owner_changed')
        budget = deadline - time.monotonic()
        if budget <= 0:
            raise EvidenceError('pointer_parking_deadline')
        return budget

    def location():
        output = subprocess.check_output(['xdotool', 'getmouselocation', '--shell'],
            text=True, stderr=subprocess.DEVNULL, timeout=remaining())
        remaining()
        if len(output) > 1024:
            raise EvidenceError('pointer_parking_invalid_coordinates')
        values = {}
        for line in output.splitlines():
            if not line.startswith(('X=', 'Y=')):
                continue
            key, value = line.split('=', 1)
            if key in values or not re.fullmatch(r'[0-9]{1,6}', value):
                raise EvidenceError('pointer_parking_invalid_coordinates')
            values[key] = int(value)
        if set(values) != {'X', 'Y'} or not all(
                0 <= values[key] < maximum for key, maximum in zip(('X', 'Y'), bounds)):
            raise EvidenceError('pointer_parking_invalid_coordinates')
        remaining()
        return values['X'], values['Y']

    if location() == tuple(point):
        return
    subprocess.run(['xdotool', 'mousemove', str(point[0]), str(point[1])],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=remaining())
    remaining()
    while location() != tuple(point):
        time.sleep(min(.02, remaining()))


def save_visible_window(window, owner, path, port, *, diagnostics_out=None):
    """Retain a VNC crop only when it equals the stable owned drawable.

    Park the pointer in the synthetic background; the exact inner client crop
    excludes compositor decorations/rounded edges. Never persist other pixels
    from the framebuffer, even when another desktop window obscures this one.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/lab'))
    import rfb
    from PIL import Image
    draw = client = failure = None
    exit_attempted = False
    stage = 'activation'
    if diagnostics_out is not None:
        diagnostics_out.update(schemaVersion=1, target={'pid': owner['pid'], 'startTicks': owner['startTicks'],
            'uid': owner['uid'], 'windowXid': int(window, 16)}, attempts=[], metadataComplete=False)
    try:
        subprocess.run(['wmctrl', '-ia', window], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=5)
        stage = 'geometry'
        info = subprocess.check_output(['xwininfo', '-id', window], text=True, timeout=5)
        import re
        x = int(re.search(r'Absolute upper-left X:\s+(-?\d+)', info)[1])
        y = int(re.search(r'Absolute upper-left Y:\s+(-?\d+)', info)[1])
        width = int(re.search(r'Width:\s+(\d+)', info)[1])
        height = int(re.search(r'Height:\s+(\d+)', info)[1])
        inset = 12
        if width <= 2 * inset or height <= 2 * inset:
            raise EvidenceError('owned_visual_crop_too_small')
        if diagnostics_out is not None:
            stage = 'entry-metadata'
            diagnostics_out['entry'] = _visibility_target(window, owner, info)
            diagnostics_out['crop'] = [x + inset, y + inset, width - 2 * inset, height - 2 * inset]
        stage = 'drawable-open'
        draw = Drawable(window, owner)
        stage = 'vnc-open'
        client = rfb.RFB(port)
        stage = 'pointer-parking'
        # Physical m74's window leaves a synthetic background margin. Refuse
        # unsupported geometry instead of retaining a cursor-obscured crop.
        points = [(client.width - 80, client.height - 80), (80, client.height - 80), (client.width - 80, 80)]
        point = next((p for p in points if not (x <= p[0] < x + width and y <= p[1] < y + height)), None)
        if point is None:
            raise EvidenceError('no_synthetic_pointer_parking_margin')
        _park_pointer(point, owner, (client.width, client.height))
        rectangle = [inset, inset, width - 2 * inset, height - 2 * inset]
        for index in range(12):
            row = None
            if diagnostics_out is not None:
                row = {'attempt': index + 1}
                diagnostics_out['attempts'].append(row)
            bracket = started = time.monotonic()
            stage = 'native-before'
            before = draw.pixels(rectangle).tobytes()
            if row is not None:
                row['beforeNativeMs'] = (time.monotonic() - started) * 1000
            stage, started = 'vnc-frame', time.monotonic()
            w, h, pixels = client.frame()
            if row is not None:
                row.update(vncFetchMs=(time.monotonic() - started) * 1000,
                           framebufferWidth=w, framebufferHeight=h)
            if x < 0 or y < 0 or x + width > w or y + height > h:
                raise EvidenceError('owned_visual_not_inside_physical_frame')
            crop = Image.frombytes('RGB', (w, h), pixels, 'raw', 'BGRX').crop(
                (x + inset, y + inset, x + width - inset, y + height - inset))
            stage, started = 'native-after', time.monotonic()
            after = draw.pixels(rectangle).tobytes()
            after_finished = time.monotonic()
            composite = crop.tobytes()
            if row is not None:
                row.update(afterNativeMs=(after_finished - started) * 1000,
                    bracketMs=(after_finished - bracket) * 1000,
                    nativeBeforeEqualsAfter=before == after,
                    compositeEqualsBefore=composite == before, compositeEqualsAfter=composite == after)
            if before == composite == after:
                if diagnostics_out is not None:
                    stage = 'exit-metadata'
                    exit_attempted = True
                    diagnostics_out['exit'] = _visibility_target(window, owner)
                stage = 'save'
                crop.save(path)
                return {'pixelSource': 'physical localhost VNC composite crop', 'equalsStableOwnedDrawable': True,
                        'nativePixelSha256': hashlib.sha256(before).hexdigest(),
                        'clientCrop': [x + inset, y + inset, width - 2 * inset, height - 2 * inset],
                        'window': window, 'owner': owner}
            time.sleep(.1)
        stage = 'comparison-refusal'
        raise EvidenceError('owned_ui_not_proven_visible_on_physical_wall')
    except BaseException as error:
        failure = error
        if diagnostics_out is not None:
            diagnostics_out.update(errorStage=stage, errorType=type(error).__name__)
        raise
    finally:
        secondary_failure = None
        if diagnostics_out is not None:
            if 'entry' in diagnostics_out and not exit_attempted:
                exit_attempted = True
                try:
                    diagnostics_out['exit'] = _visibility_target(window, owner)
                except BaseException as error:
                    diagnostics_out['exitMetadataErrorType'] = type(error).__name__
                    if failure is not None:
                        failure.add_note('visibility_exit_metadata: ' + type(error).__name__)
                        failure.visibility_abort = True
                    else:
                        secondary_failure = error
            diagnostics_out['metadataComplete'] = 'entry' in diagnostics_out and 'exit' in diagnostics_out
            if diagnostics_out['metadataComplete']:
                diagnostics_out['geometryChanged'] = diagnostics_out['entry']['geometry'] != diagnostics_out['exit']['geometry']
            rows = diagnostics_out['attempts']
            complete = [r for r in rows if 'compositeEqualsAfter' in r]
            diagnostics_out.update(attemptCount=len(rows), completedComparisonCount=len(complete),
                nativeChangedCount=sum(not r['nativeBeforeEqualsAfter'] for r in complete),
                stableNativeCompositeMismatchCount=sum(r['nativeBeforeEqualsAfter'] and not r['compositeEqualsBefore'] for r in complete),
                exactMatchCount=sum(r['nativeBeforeEqualsAfter'] and r['compositeEqualsBefore'] and r['compositeEqualsAfter'] for r in complete))
        for label, resource in (('drawable', draw), ('vnc', client)):
            if resource is None:
                continue
            try:
                resource.close()
            except BaseException as error:
                if diagnostics_out is not None:
                    diagnostics_out.setdefault('cleanupErrors', []).append(
                        {'resource': label, 'type': type(error).__name__})
                original = failure or secondary_failure
                if original is not None:
                    original.add_note('visibility_' + label + '_close: ' + type(error).__name__)
                    original.visibility_abort = True
                else:
                    secondary_failure = error
        if secondary_failure is not None:
            raise secondary_failure


class Accessible:
    def __init__(self, owner):
        import gi
        gi.require_version('Atspi', '2.0')
        from gi.repository import Atspi
        self.api, self.owner = Atspi, owner
        desktop = Atspi.get_desktop(0)
        self.root = None
        for i in range(desktop.get_child_count()):
            app = desktop.get_child_at_index(i)
            if app.get_process_id() == owner['pid']:
                self.root = app
        if self.root is None:
            raise EvidenceError('owned_qt_accessibility_unavailable')
        self.check()

    def check(self):
        if not same_identity(identity(self.owner['pid']), self.owner):
            raise EvidenceError('accessible_owner_changed')
        if self.root.get_process_id() != self.owner['pid']:
            raise EvidenceError('accessible_bus_owner_mismatch')
        self.root.clear_cache()

    def find(self, name):
        self.check()
        queue, found, visited = [self.root], [], 0
        while queue:
            node = queue.pop()
            visited += 1
            if visited > 4096:
                raise EvidenceError('accessible_tree_limit')
            actual = (node.get_name() or '').replace('&', '')
            if actual == name and node.get_state_set().contains(self.api.StateType.SHOWING):
                found.append(node)
            queue.extend(child for i in range(max(0, node.get_child_count()))
                         if (child := node.get_child_at_index(i)) is not None)
        if len(found) != 1:
            raise EvidenceError('accessible_control_missing_or_ambiguous')
        return found[0]

    def click(self, name):
        node = self.find(name)
        if not self.api.Action.do_action(node, 0):
            raise EvidenceError('accessible_action_refused')

    def focus(self, node):
        self.check()
        if not self.api.Component.grab_focus(node):
            raise EvidenceError('accessible_focus_refused')

    def search(self, text):
        node = self.find('Search history by file name')
        if not self.api.EditableText.set_text_contents(node, text):
            raise EvidenceError('accessible_search_text_refused')
        self.click('Search')

    def list_names(self):
        node = self.find('Captures and upload activity')
        return [node.get_child_at_index(i).get_name() for i in range(node.get_child_count())]

    def select(self, index):
        node = self.find('Captures and upload activity')
        self.focus(node)
        if not self.api.Selection.select_child(node, index):
            raise EvidenceError('accessible_selection_refused')

    def key(self, *keys):
        self.check()
        subprocess.run(['xdotool', 'key', '--clearmodifiers', *keys], check=True, timeout=5,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def last_row_visible(self):
        node = self.find('Captures and upload activity')
        count = node.get_child_count()
        if count == 0:
            return True
        area = self.api.Component.get_extents(node, self.api.CoordType.SCREEN)
        row = self.api.Component.get_extents(node.get_child_at_index(count - 1), self.api.CoordType.SCREEN)
        return area.y <= row.y and row.y + row.height <= area.y + area.height
