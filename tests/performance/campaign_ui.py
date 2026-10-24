"""External PID-owned accessibility/input and X11 drawable observations.

No product hooks or checkpoint polling. Pixel-change times are observer upper
bounds, with probe costs and cadence retained; they are not compositor-present
timestamps or a count of every rendered frame. Imports do not open a display.
"""
import ctypes as C
import hashlib
import subprocess
import time

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

    def pixels(self, rectangle):
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
            raw = C.string_at(info.data, info.bytes_per_line * info.height)
            from PIL import Image
            pixels = Image.frombytes('RGB', (info.width, info.height), raw, 'raw', 'BGRX', info.bytes_per_line)
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
