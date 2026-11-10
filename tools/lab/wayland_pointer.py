#!/usr/bin/env python3
"""Drive a wlroots compositor's pointer through zwlr-virtual-pointer-v1.

Headless labwc has no input devices; a region selector still needs a real
drag. This stdlib-only Wayland client creates a virtual pointer and sends
absolute motion and button events over the whole output layout, then waits
for the compositor to process them. It is lab input, never desktop proof.

  wayland_pointer.py drag X1 Y1 X2 Y2 --extent W H [--steps N] [--button left|right]
  wayland_pointer.py click X Y --extent W H [--button left|right]
  wayland_pointer.py move X Y --extent W H
"""
import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wayland_toplevels as wire  # noqa: E402  (the shared wire helpers)

BUTTONS = {'left': 0x110, 'right': 0x111, 'middle': 0x112}


class Pointer(wire.Client):
    def open(self):
        self.registry = self.new_id('wl_registry')
        self.send(wire.message(1, 1, struct.pack('<I', self.registry)))
        self.sync()
        if 'zwlr_virtual_pointer_manager_v1' not in self.globals:
            raise RuntimeError('the compositor offers no zwlr_virtual_pointer_manager_v1')
        self.bind('wl_seat', 1, 'wl_seat')
        seat = self.next_id - 1
        self.bind('zwlr_virtual_pointer_manager_v1', 1, 'manager')
        manager = self.next_id - 1
        self.pointer = self.new_id('pointer')
        self.send(wire.message(manager, 0, struct.pack('<II', seat, self.pointer)))
        self.sync()

    def dispatch(self, object_id, opcode, r):
        # wl_seat capabilities/name and anything else are ignored.
        if self.objects.get(object_id) in ('wl_registry', 'wl_callback', 'wl_display'):
            super().dispatch(object_id, opcode, r)

    def stamp(self):
        return int(time.monotonic() * 1000) & 0xffffffff

    def motion(self, x, y, width, height):
        self.send(wire.message(self.pointer, 1, struct.pack('<IIIII', self.stamp(), int(x), int(y), int(width), int(height))))
        self.send(wire.message(self.pointer, 4))

    def button(self, code, pressed):
        self.send(wire.message(self.pointer, 2, struct.pack('<III', self.stamp(), code, 1 if pressed else 0)))
        self.send(wire.message(self.pointer, 4))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('action', choices=('drag', 'click', 'move'))
    parser.add_argument('coords', type=float, nargs='+')
    parser.add_argument('--extent', type=int, nargs=2, required=True, metavar=('W', 'H'))
    parser.add_argument('--steps', type=int, default=12)
    parser.add_argument('--step-ms', type=int, default=16)
    parser.add_argument('--button', choices=sorted(BUTTONS), default='left')
    parser.add_argument('--settle-ms', type=int, default=300, help='click: the wait between the motion and the press')
    parser.add_argument('--display')
    args = parser.parse_args(argv)
    expected = 4 if args.action == 'drag' else 2
    if len(args.coords) != expected:
        parser.error('%s takes %d coordinates, got %d' % (args.action, expected, len(args.coords)))
    width, height = args.extent
    p = Pointer(wire.socket_path(args.display))
    try:
        p.open()
        if args.action == 'move':
            p.motion(args.coords[0], args.coords[1], width, height)
        elif args.action == 'click':
            p.motion(args.coords[0], args.coords[1], width, height)
            # A client binds wl_pointer only once the seat has announced a pointer, and this device is the
            # seat's first: a press sent right behind it reaches the compositor's own binds and not the
            # client under it. The compositor takes the device and the motion first, then the client gets
            # time to bind and to take the enter.
            p.sync()
            time.sleep(args.settle_ms / 1000)
            p.button(BUTTONS[args.button], True)
            p.sync()
            time.sleep(0.05)
            p.button(BUTTONS[args.button], False)
        else:
            x1, y1, x2, y2 = args.coords
            p.motion(x1, y1, width, height)
            p.sync()
            p.button(BUTTONS[args.button], True)
            for i in range(1, args.steps + 1):
                time.sleep(args.step_ms / 1000)
                p.motion(x1 + (x2 - x1) * i / args.steps, y1 + (y2 - y1) * i / args.steps, width, height)
            p.sync()
            p.button(BUTTONS[args.button], False)
        p.sync()
    finally:
        p.sock.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
