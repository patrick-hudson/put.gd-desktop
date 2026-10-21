# Cinnamon / X11

The adapter requires the Qt `xcb` backend, RandR, XFixes, and a resident process in the same X11 user session. Wayland is a separate integration and is not selected by this adapter. Capture, clipboard import/result ownership, and each requested shortcut have separate diagnostics.

Window capture freezes `_NET_ACTIVE_WINDOW` and the framed target rectangle before journal work can change focus. The frame is captured from visible desktop pixels; overlapping windows appear in the image. A disappeared, unmapped, moved, resized, or app-owned target produces an actionable target error. There is no hidden-window reconstruction. Frame extents follow [EWMH](https://specifications.freedesktop.org/wm/latest/ar01s05.html).

Window capture refuses put.gd's own windows with `capture_target`, in the
resident, before layout work. A focused window is put.gd's when it is one of the
resident's own Qt windows, or its `_NET_WM_PID` is the resident's, the registered
views helper's, the capture helper's (Cinnamon core route), or that of any other
process whose parent (from `/proc/PID/stat`) is the resident. The last rule
covers the Edit & upload editor (`putgd-ui --edit-image`) and the recording
helper, which never register (#103), and matches the GNOME extension's rule. The
same rules run on Plasma X11, which uses this adapter. `_NET_WM_PID` is set by the
client: put.gd's Qt windows set it truthfully, and a client that lies about it
can only get its own window refused. The [X11 editor refusal
receipt](../evidence/2026-10-19-x11-editor-refusal.md) records the physical
Kubuntu Plasma X11 run.

Region selection appears over the live desktop. Windows, animation and video keep
updating before and during the drag. Drag with the left button; Escape or the right
button cancels. Monitor identity, geometry, scale and rotation are fixed when the
action starts; a changed layout invalidates the selection. Each monitor has a
transparent selection surface carrying native and logical geometry. Surfaces keep
external application input focus and take a temporary keyboard grab for Escape;
dispatch releases the shortcut connection's passive grab before selection starts,
including when trigger keys remain held.

Pixels are acquired after the selection surfaces are removed. On an identified
Cinnamon/Muffin session, the native compositor screenshot service captures its
scene into private files. Its D-Bus reply can precede the PNG write
(`ScreenshotArea` returns before Cinnamon paints), so decoding waits for the writer's
close event, validates the PNG and accepted geometry, and uses a ten-second deadline.
The close event needs an inotify instance, a per-user limit
(`fs.inotify.max_user_instances`, 128 by default) shared with every program. When
none is left, the helper polls each file until it ends in a stable PNG IEND trailer
and logs `Cinnamon capture completion watch unavailable (EMFILE); polling for
finished PNGs` ([#331](../evidence/2026-10-21-region-inotify-331/README.md)).
Cancellation discards pending results. Other X11
sessions use post-selection root capture and require their own compositor
acceptance; an X11 round trip alone does not prove presentation completed.

Composition preserves the original pixel grid for an entirely single-monitor
selection, using floor/ceiling on local native edges. Spanning composition uses the
highest participating native density, rounds global pixel-grid boundaries outward,
and leaves layout gaps transparent. Already displayed rotated frames are not
rotated a second time.

Clipboard import requests supported X11 selection targets without waiting synchronously on the owner. It prefers local files, images supported by the installed QImageReader plugins (PNG first), and UTF-8 plain text, using the existing payload contract; plain-text URLs remain text. A remote URI list falls through to the owner’s actual image or text representation when available. Each selection transfer has a five-second deadline and resource bounds, and owner changes invalidate acquisition. Imports are queued in frozen-revision order within the accepted operation limit; cancelling a capture leaves independent clipboard imports active. PNG decoding and capture composition run on workers. Results use Qt clipboard ownership, with local image results supplied as previously encoded PNG bytes by the shared copy worker so paste does not encode screenshots on the session thread. The resident application must continue running to serve clipboard results. The transfer protocol follows [ICCCM selections](https://xorg.freedesktop.org/archive/X11R7.7/doc/xorg-docs/icccm/icccm.html).

Shortcut setup uses checked XCB passive grabs and reports disabled, unmapped, unsupported, or conflicting bindings individually. Successful grabs include the active Caps Lock, Num Lock, and Scroll Lock modifier combinations. Keyboard mapping changes refresh requested bindings and update actual status. ASCII letter shortcuts do not require an implicit Shift; shifted printable symbols use their actual keymap modifier. Missing modifiers and unsupported symbol levels report explicit binding failures. Conflicting grabs are released without stealing another client's shortcut.

## Evidence boundary

The automated X11 tests run in a disposable Xvfb display with synthetic clipboard ownership and pixels. They do not establish Cinnamon focus behavior, a physical monitor layout, real global keys, clipboard paste into an independent desktop app, live S3 public links, or release timing.

The release profile remains pending until the Mint 22.3 Cinnamon/X11 physical host and disposable guest complete `tests/desktop/cinnamon.yaml`. Record actual desktop/Qt versions, build identity, display geometry, fixture artifact dimensions/hashes, independent paste outcomes, and timings. A mock, offscreen test, or this document alone does not declare desktop support.
