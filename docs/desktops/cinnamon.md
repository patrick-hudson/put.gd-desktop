# Cinnamon / X11

The adapter requires the Qt `xcb` backend, RandR, XFixes, and a resident process in the same X11 user session. Wayland is a separate integration and is not selected by this adapter. Capture, clipboard import/result ownership, and each requested hotkey have separate diagnostics.

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
receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-19-x11-editor-refusal.md) records the physical
Kubuntu Plasma X11 run.

Capture monitor (#1363) freezes the RandR output whose rectangle contains the
pointer (`QueryPointer` on the root window) with the layout, in the resident, and
the helper reads only that output from visible pixels: the image is the CRTC's
native size, panels included. A pointer over no output is refused with
`capture_target`. Repeat last region captures the remembered logical rectangle
through the path a selected region takes, with no selector; the helper reports each
region's rectangle with its result and the resident keeps it in memory with the
monitors it was chosen on (name, native and logical rectangle, rotation). A repeat
compares only those, not the desktop-settings fingerprint, so restoring a changed
layout makes it work again. See
[capture monitor, delay and repeat](../design/capture-delay.md).

A delayed capture shows its countdown as an override-redirect window of
`putgd-capture`, at the top centre of the pointer's monitor. It takes no focus and
no grab; a click on it cancels. For a live region the same helper shows it after
the selection; every other delayed capture runs the helper's countdown mode first
and freezes its target when that exits. The pill is hidden and the compositor's
repaints over it have settled before anything is read. When the delay ends on a
locked screen (Cinnamon's or the freedesktop screensaver service, or logind's
`LockedHint`), nothing is captured: `session_locked`.

Region selection appears over the live desktop. Application windows, animation and video keep
updating before and during the drag. Drag with the left button; Escape or the right
button cancels. Monitor identity, geometry, scale and rotation are fixed when the
action starts; a changed layout invalidates the selection. Each monitor has a
transparent selection surface carrying native and logical geometry. Surfaces keep
external application input focus and take a temporary keyboard grab for Escape;
dispatch releases the hotkey connection's passive grab before selection starts,
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
finished PNGs` ([#331](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-21-region-inotify-331/README.md)).
Cancellation discards pending results. Other X11
sessions use post-selection root capture and require their own compositor
acceptance; an X11 round trip alone does not prove presentation completed.

Composition preserves the original pixel grid for an entirely single-monitor
selection, using floor/ceiling on local native edges. Spanning composition uses the
highest participating native density, rounds global pixel-grid boundaries outward,
and leaves layout gaps transparent. Already displayed rotated frames are not
rotated a second time.

Clipboard import requests supported X11 selection targets without waiting synchronously on the owner. It prefers local files, images supported by the installed QImageReader plugins (PNG first), and UTF-8 plain text, using the existing payload contract; plain-text URLs remain text. A remote URI list falls through to the owner’s actual image or text representation when available. Each selection transfer has a five-second deadline and resource bounds, and owner changes invalidate acquisition. Imports are queued in frozen-revision order within the accepted operation limit; cancelling a capture leaves independent clipboard imports active. PNG decoding and capture composition run on workers. Results use `X11Clipboard` selection ownership through XCB, with local image results supplied as previously encoded PNG bytes by the shared copy worker so paste does not encode screenshots on the session thread. The resident application must continue running to serve clipboard results. The transfer protocol follows [ICCCM selections](https://xorg.freedesktop.org/archive/X11R7.7/doc/xorg-docs/icccm/icccm.html).

Hotkey setup uses checked XCB passive grabs and reports disabled, unmapped, unsupported, or conflicting bindings individually. Successful grabs include the active Caps Lock, Num Lock, and Scroll Lock modifier combinations. Keyboard mapping changes refresh requested bindings and update actual status. ASCII letter hotkeys do not require an implicit Shift; shifted printable symbols use their actual keymap modifier. Missing modifiers and unsupported symbol levels report explicit binding failures. Conflicting grabs are released without stealing another client's hotkey.

## Recording click highlights

On X11 the recording helper reads button presses itself; nothing in the adapter
or in Cinnamon is involved. `putgd-record` opens an X connection of its own and
selects XInput 2.1 raw button presses on the root window
(`XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS`). Raw events are copies sent to
every client that asks: nothing is grabbed, the click reaches its window
unchanged, and presses inside another client's window or under its pointer grab
are still reported. At each press the helper asks the server where the pointer
is and, for a window recording, where the window is now, so the ring stays on a
window that has moved. The selection exists from Record to Stop. The ring is
drawn on a copy of the frame in plain memory, which matters here: X11 recordings
on Intel use a VA encoder, whose own surfaces cannot be drawn on in place.
Measured on Cinnamon 6.6.9 (Mint 22.3): three presses, three rings, within 1 px
of the press and one frame of it, in monitor, region and window recordings.

An X11 recording's microphone never provides the pipeline's clock: `ximagesrc`
has none, and a recording with the microphone alone used to run on the
microphone's, which stops when the microphone is unplugged and took the rest of
the clip with it. See
[The clock](../design/recording/microphone-and-clicks.md#capture-and-mix).

`recording.capabilities` reports `clickHighlights {available: true, route:
"x11-xinput2", drawnBy: "recorder"}` for monitor, desktop, region and window
recordings. An X server without XInput 2.1 reports `xinput2_unavailable`, and a
`prepare` that asks for click highlights there is refused. The same route
serves every X11 session, Plasma X11 included. The design is in
[Microphone and click highlights](../design/recording/microphone-and-clicks.md).

## Scrolling capture

Scrolling capture ([design](../design/scroll-capture.md)) uses `ximagesrc` on the region with
`show-pointer=false` for its frames and XTest for the wheel, and serves every X11 session,
Plasma X11 included. The helper's own xcb connection sends XTest button 5 and 4 presses and
releases (one pair per notch) and absolute pointer motion; it reads the user's pointer before the
first move and puts it back at the end. It needs the XTEST extension (`injection_unavailable`
without it) and waits up to 10 seconds for Ctrl, Shift, Alt and Super to be up before the first
notch, because Ctrl plus wheel zooms (`capture_timeout`, check `modifiers_released`). The region
selector is the helper's own, as for a region recording. Escape stops the capture and keeps the
image: it is read as an XInput 2.1 raw key press, so the bar never needs the keyboard and the key
still goes where it was going. The capture bar sits 32 px below the rectangle on X11 (10 px
elsewhere), because Muffin draws a shadow around it that at 10 px darkened the last rows of the
image under the bar's columns. `scroll_capture_x11` drives the real helper on Xvfb and
reads the X server's raw input to show that nothing is injected before capturing or after Stop,
Cancel or the end of the control pipe; it does not establish Cinnamon behaviour.

## Copy text

Copy text ([design](../design/text-recognition.md)) selects a region with the same early crosshair
as **Capture region** (`X11CoreSourceAdapter`), live or screenshot-first as configured, and then
reads it. A text capture never changes the rectangle that **Repeat last region** remembers. The
hotkey needs nothing more than the other actions' (it is unassigned by default). It needs no
compositor protocol beyond what region capture already uses, and the clipboard write is an
ordinary one that never touches PRIMARY. Recognition runs in the confined `putgd-ocr` helper on every desktop (see [text recognition](../design/text-recognition.md)), which needs the kernel's Landlock at version 3 or later (Linux 6.2 or newer with Landlock enabled).

TO CONFIRM before merge: no acceptance of Copy text has run on Cinnamon. The settled evidence goes
here when item 8 of #1428 has run on m74.

## Evidence boundary

The automated X11 tests run in a disposable Xvfb display with synthetic clipboard ownership and pixels. They do not establish Cinnamon focus behavior, a physical monitor layout, real global keys, clipboard paste into an independent desktop app, live S3 public links, or release timing.

The release profile remains pending until the Mint 22.3 Cinnamon/X11 physical host and disposable guest complete `tests/desktop/cinnamon.yaml`. Record actual desktop/Qt versions, build identity, display geometry, fixture artifact dimensions/hashes, independent paste outcomes, and timings. A mock, offscreen test, or this document alone does not declare desktop support.
