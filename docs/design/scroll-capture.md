# Scrolling capture

A scrolling capture takes one tall PNG of content that is longer than the screen. You
select a region over scrollable content, and the capture follows the content as it moves,
frame by frame, whoever moves it: you (wheel, touchpad, keys, scrollbar) or put.gd
(**Auto-scroll**, which turns the mouse wheel for you). The image goes down the same route
as a region capture. It is [#1376](https://github.com/patrick-hudson/put.gd-desktop/issues/1376),
rebuilt in [#1515](https://github.com/patrick-hudson/put.gd-desktop/issues/1515). The
user's view is in the [user guide](../user/user-guide.md#capture-a-scrolling-region); this
page is the contract.

It never asks the application how long its content is. Only `end_of_content` says that
put.gd itself scrolled to the end of the page and could place every frame on the way. A
capture you finish yourself is the page as far as you scrolled it.

## Workflow

1. **Start.** The tray's **Capture scrolling region**, the main window's button, the
   `scroll` hotkey (unbound by default), `action.invoke {"action":"scroll"}` or the MCP
   `perform_action`. `editor.open {"source":"scroll"}` does the same and opens the result
   in the [editor](image-editor.md) instead of sending it on.
2. **Select.** The recording region selector opens, live as for a recording. The ring sits
   just outside the rectangle. The bar sits beside the rectangle, or inside its bottom-right
   corner when there is no room beside it ([the bar](#the-bar)).
3. **Capture.** It starts after the drag and waits for you to scroll: the bar says
   **Scroll the page**. Nothing is injected. **Auto-scroll** on the bar hands the wheel to
   put.gd and takes it back; **Start with auto-scroll** in Settings
   (`preferences.scrollCaptureAutoScroll`, off by default) starts with it on. Moving the
   mouse pauses auto-scroll, and the capture follows you from there.
4. **End.** **Done** (the bar's button, the `scroll` hotkey again, Esc on X11,
   `scroll.stop`), auto-scroll at the end of the page, the height cap, or ten minutes with
   nothing to follow. Done keeps the image; **Cancel** discards it.
5. **Result.** One PNG, routed by the `scroll` action's output like a region capture
   (local copy, upload, History, link copy). Through the editor, by the `edit` action's.

One scroll session exists at a time. It excludes recording, the editor and any other
selection: `scroll_active` for a request that arrives during one (the record action,
`recording.prepare`, a screenshot), and `recording_active`, `editor_active` or
`busy_selecting` for a scroll request during those. A second `scroll` invoke while one is
capturing is Done. Installing an update is refused while a capture runs.

## Process boundary

Scroll capture is a second session kind of `putgd-record`. The helper already has region
selection, the ring, bar placement, prompt-free frame streams and the staging discipline
of a recording on every desktop; a scroll session replaces the encoder with the tracker.
Frames never leave the helper and the tall raster never enters the resident. A build
without `PUTGD_RECORDING` has no scroll capture: `scroll_unavailable`, reason
`recording_helper_missing`.

| Part | Source | Role |
| --- | --- | --- |
| `ScrollCaptureService` | `src/app/ScrollCaptureService.*` | The resident's session: states preparing, selecting, ready, capturing, finishing, then done, failed or cancelled. Commits the operation to the journal before any frame is read, validates and admits the result, keeps the last 32 results inspectable |
| `ScrollBackend` | `src/app/ScrollBackend.h` | The seam, and the only thing the service knows; tests inject a fake |
| `ScrollHelperBackend` | `src/app/ScrollHelperBackend.*` | The production backend: one `putgd-record` per session over the private control framing, as `RecordingHelperBackend` does |
| `ScrollCapture` | `src/record/ScrollCapture.*` | The helper's session: selector, ring, bar, frame tap, injector, the pointer's rest and watch (#1514), worker thread, spool and PNG |
| `ScrollSession` | `src/record/ScrollSession.*` | The loop: every frame to the tracker, the two drivers, the way to the top, the end of the page, the idle bound. Standard library only, so the same loop runs against a synthetic application in tests |
| `ScrollTracker` | `src/core/ScrollTracker.*` | The matching engine ([below](#the-tracker)) |
| `SpoolCanvas` | `src/record/ScrollSpool.*` | The image on disk, with room at both ends |
| `FrameTap` | `src/record/FrameTap.*` | Source into `videoconvert ! video/x-raw,format=BGRx ! appsink`; no `videorate`, no `videoscale` |

The contract in `ScrollBackend.h`:

- `capabilities()` is `{available, reason?, injector, frames, maxHeight}`, and from the
  helper also `message?` and `selection` (`helper`, `shell` or `none`: who selects the
  region on this desktop). `reason` is `recording_helper_missing`, `injection_unavailable`,
  `frame_source_unavailable` or `region_unavailable`, or while the first probe
  (`putgd-record --scroll-capabilities`, which opens no source and sends no input) has not
  answered, `scroll_not_probed`, `scroll_probe_pending` or `scroll_probe_failed`. Those
  three are transient: ask again, and do not show them as a permanent failure. With
  `selection: none` the probe is not `available`: its reason is `region_unavailable`, and
  the helper's `prepare` and the resident's `scroll.prepare` refuse with it whether or not
  they name a `rect` (see [known limits](#known-limits)). Auto-scroll needs the injector;
  a desktop without one has no scrolling capture at all, so both drivers are there or
  neither.
- `prepare {rect?, maxHeight, maxPixels, fromTop, autoStart, selection, driver,
  finishKey?}` then `select {rect}`, `start`, `autoScroll {on}`, `stop`, `cancel` and
  `overlay {visible, rect?, inside?}`. `driver` is `user` or `putgd`: who turns the page
  at the start. `pause` and `resume` are `autoScroll` off and on (their names from #1514).
  `finishKey` is the key the bar names. Replies come only once the helper reached the
  state. `stop` is Done and keeps the image, and its reply is the object a later
  `scroll.inspect` returns. `cancel` is accepted while a `start` or `stop` is pending (it
  is never `scroll_busy`); a `start` or `stop` waiting for the helper then ends with the
  session. Once a `cancel` is on its way it decides the end: see
  [Done and Cancel](#output-and-limits).
- Events, each with the session id: `selected {rect}`; `progress {state, driver, paused,
  pausedBy?, height, placed, skipped, steps, pixelsPerNotch}`; `finished` with the
  [result](#results-and-end-reasons); `cancelled`; `failed {error}`. After `finished`,
  `cancelled` or `failed` the helper injects nothing more for that id.
- Deadlines: 10 s for the helper to start, 15 s per command, 30 s for `stop`. A capture
  has no time limit of its own; [the idle bound](#output-and-limits) ends one nobody
  touches. A missed deadline is `scroll_helper_failed` with the check (`start_timeout`,
  `command_timeout`); a helper that dies, answers out of order or floods stderr is the
  same code (`exited`, `crashed`, `protocol`, `build`). A session client waits 15 s past
  those deadlines, so the resident's own answer arrives (40 s for the `scroll.*` methods,
  45 s for `scroll.stop`).

On GNOME the Shell extension selects the region and draws the ring, and the resident
places the bar through it (`ScrollShell`, a `RecordingShell` of the scroll session). The
Shell puts it below or above the rectangle, on another monitor, or inside the rectangle's
bottom-right corner; the resident sends the helper `overlay {visible: true, rect, inside}`
and the helper keeps an inside bar's rows out of the image. The ring and the beat pixel
are Shell actors that take no input. The bar never takes the keyboard.

put.gd's own notifications stay out of the picture. When a session is accepted the
resident closes those on screen and holds back new ones until the session ends: in-flight
notifications whose server reply has not come yet, the file-manager notice and Qt's
fallback balloon (the tray icon is hidden and shown again to remove it). It delays the
first frame until the server has reported the old ones closed (at the latest about 1.7 s)
and the hold on the start is bounded (5 s). A notice that cannot be closed, after the
close was retried twice, fails the preparation with `notification_close_failed`; nothing
is captured.
On Omarchy's quickshell notification server a closed card stays on screen, so the resident
also asks the shell to dismiss it by summary with `qs ipc --pid <server> call
notifications dismiss <summary>`, run with the server's own `XDG_RUNTIME_DIR`. A
notification of another application is not detected: see [known limits](#known-limits).

## Frames

The frame stream is the recording helper's own, with the cursor hidden and no resampling.
The portal screen cast is never used: without the native stream the feature is
unavailable. Every frame goes to the tracker as it arrives; the tap keeps only the
newest, so a frame the loop had no time for is dropped, never queued.

| Desktop | Stream | Cursor and scale |
| --- | --- | --- |
| X11 (Cinnamon) | `ximagesrc` on the region | `show-pointer=false` |
| GNOME | Mutter `RecordArea` in a screen cast linked to a RemoteDesktop session | `cursor-mode` 0. The Shell shows its screen-sharing indicator while it runs; ending the session from it is Stop. The extension repaints one transparent pixel of the area every 40 ms (`beat`), because Mutter's area stream sends a frame only for damage inside it |
| Plasma | KWin `zkde_screencast_unstable_v1` `stream_region` | pointer hidden, at the output's scale (from protocol version 5 scale 0 lets KWin pick). A 1×1 layer surface commits every 30 ms while frames are read, because KWin drops an isolated repaint |
| Budgie (labwc), Hyprland | `ext-image-copy-capture-v1` through `appsrc` and `videocrop` | session created without `paint_cursors`. On the Omarchy lab guest Hyprland still composited a software cursor into every copy |

Frames per second while a person scrolls, from the helper's log line `scroll: source N
frames in M ms` (the tracker takes 2 to 7 ms a frame at 1900 × 950 on the slowest
builders, so it keeps up with every rate below):

| Desktop | Measured while a person scrolled |
| --- | --- |
| Plasma 6.8.2 (KWin's region stream with the heartbeat) | 18 to 26 frames a second on long pages, 23 at the median (aurora-amd, Chrome, 2026-11-14); the loop took every frame the stream delivered |
| Cinnamon X11 (`ximagesrc`, 30 a second) | 28 to 30 frames a second (m74, Firefox 158, 2026-11-14) |
| GNOME (Mutter's area stream with the 40 ms beat), Hyprland and Budgie (`ext-image-copy-capture`, on damage) | measured by the acceptance round from the CI packages; the helper's log line says it |

The frame size is the logical rectangle at the output's scale. Lab runs at 125% on Plasma
and GNOME and at 150% on Cinnamon X11 gave exact images; a compositor that resamples a
client's buffer would not.

When auto-scroll is selected at the start, the pointer moves to its rest place before the first frame.

## The tracker

`putgd::scroll::ScrollTracker` (`src/core/ScrollTracker.h`, the interface and every rule;
[#1527](https://github.com/patrick-hudson/put.gd-desktop/pull/1527), the results on 160
real frame sequences) works out how far the content moved since the last frame it placed,
up or down, and composes the page into a canvas the helper owns. In brief:

- Rows are compared by brightness in column bands, with a tolerance, searched outward
  from the offset it expects (the last movement, or auto-scroll's hint).
- Parts that stay where they are while the rest moves (a header, a footer, a side bar, a
  floating box) do not vote, and are in the image once: what is pinned at the top as the
  first screen showed it, what is pinned at the bottom as the last screen showed it, a
  side bar where the first screen showed it.
- It paints the page from evidence of motion
  ([#1538](https://github.com/patrick-hudson/put.gd-desktop/issues/1538)). A cell is
  written when two frames show it moved by the page's step, unless more than half of its
  block's textured cells did not; a textured cell that looks the same where it was and a
  step away counts only if it matches within a tighter tolerance (a table of equal rows). A flat cell (white
  space, a banner's flat inside) proves nothing either way: the frames vote on its colour,
  and the leading colour is used where nothing better was written; the first or last
  screen's copy fills a cell nothing was written for only while the lead of its vote is
  below three. Outside the part of the
  screen that shows the page, in a screen column where something is held back, only the
  page's own background colour can start or take over a cell's lead, written flat except in the cell right beside that
  part (the page's own edge). A cell seen in one
  frame only is written where the screen shows the page there: a stretch of a screen
  column with nothing held back in it (between stuck parts or the frame's edges) where the
  page moved within the last 8 placed frames. When the capture ends, the last placed
  frame's rows past what the frames before it showed are written wherever nothing is held
  back.
- A pinned part that starts to move is still that part: a side column that lets go where
  its own column ends is followed, also while it slides under a header, and drawn once.
  A bar a third of the frame wide or more and a third of its height or less is held back
  as a whole rectangle, the plain parts beside its text included.
- When it finds that the whole view moved (an information bar opens above the page and
  pushes everything down, Firefox's DRM bar among them: two pinned parts or more moved
  together, one of them a bar across two thirds of the frame or more), nothing is painted
  from that frame but the page's rows it uncovered below the image (a bar above the page
  closing), which are written like any frame's, and it never replaces the first or last screen
  kept for what is pinned. A move
  against the way the page was scrolling is carried on to the next placed frames that move
  the same way, up to 8 after each frame it finds, while fewer than half of the pinned
  parts it can follow are found back in place (the bar's opening eases out).
- A frame it cannot place is skipped and nothing of it is written. After two in a row it
  is lost (the bar says **Scroll back a little**). It finds a later frame again in the
  image it has built, so scrolling back brings it back.
- Content that changes in place (a video, a carousel, a hover effect) is taken as first
  seen and ends nothing. Once the tracker has lost the page, a frame matched at a guessed
  place marks no rows as changing in place: over look-alike rows (a pull request's tables)
  the guess is often wrong, and its marks let a sticky header be written over text the image
  already held ([#1795](https://github.com/patrick-hudson/put.gd-desktop/issues/1795)). The
  one exception is the comparison with the previous skipped frame when that frame's place
  was already known or found in the image.
- It never writes a row at a guessed offset. The one exception is a frame with nothing to
  compare (a blank stretch) while auto-scroll gives a hint, and only where that frame
  overlaps what the image holds. A frame that stood still is never placed on the hint.
- A frame needs 16 rows of evidence that agree. The page's last lines above a blank tail
  may have fewer: a match with at least half of them, agreeing in every lane, is taken
  when it is the only one where auto-scroll's hint says the page is.
- A place found again needs `recoverCells` (0.95) agreement from its layer's cells and `recoverKeepShare` (0.80) agreement over its candidate tile area, with `keepShare` as a floor. Another place fitting within `recoverTie` (0.05) makes it ambiguous.
- After skipped frames, without a hint, a pair match is rejected if the frame or its reference repeats itself about as well as the pair matches (`selfRepeat`, within `kRepeatMargin` of 0.05). The tracker stays lost and the bar asks to scroll back; repeated rows alone do not prove the place (#1556).
- With no hint, no skipped frames in a row and a placed move before it, a pair match that turns back against that move by more than half the frame's height loses to another accepted match that goes on the way the page went. The other match must reach `recoverCells`, score no more than `recoverTie` below the turn back, move at least as many tiles, and share at least half the moving columns of whichever match has fewer. A turn back that passes the whole-overlap check (`wholeAgrees`) stays where it is. For example, a person flicking back hard after a pause is placed where the page went. On GitHub's pull request page with frames dropped, the turn back was a look-alike one table away, and taking it left every later frame a table short ([#1797](https://github.com/patrick-hudson/put.gd-desktop/issues/1797)).
- Frames narrower or lower than 16 px (two cells, two block rows) are refused.

## Drivers

### You scroll

Nothing is injected and the pointer is not moved. No watch or rest surface is shown: over
the page there is only the ring, outside the rectangle and transparent to input (a layer
surface with an empty input region, or Shell actors that are not reactive), and the bar
where it has to sit inside the corner (it takes input only on itself). The wheel, the
touchpad, the keys and the scrollbar all reach the application. The image grows as the
page moves, at both ends: scroll above where the capture began and it grows upwards.

### Auto-scroll

Only wheel steps and pointer motion are injected, never a button, never a key, and only
while auto-scroll is on. Each injector turns stream pixels of the rectangle into its
desktop's coordinates.

| Desktop | Wheel | Pointer | Access it needs |
| --- | --- | --- | --- |
| X11 | XTest buttons 4 and 5, press and release per notch | XTest absolute motion; the user's pointer is read first and put back at the end | The XTEST extension. The loop waits up to 10 s for Ctrl, Shift, Alt and Super to be up first (Ctrl plus wheel zooms); `capture_timeout`, check `modifiers_released`, when they stay down |
| GNOME | `org.gnome.Mutter.RemoteDesktop` `NotifyPointerAxisDiscrete` on a session linked to the screen cast | `NotifyPointerMotionAbsolute` on the area stream | None, but every call must come from the D-Bus connection that created the session. The interface is private to Mutter. No owner or a refused `CreateSession` is `injection_unavailable` |
| Plasma | `org_kde_kwin_fake_input` `axis(vertical, 15.0)` per notch, after one `authenticate`; an `axis` of 0 says the wheel has come to rest | `pointer_motion_absolute` in global logical coordinates | Plasma 6.3 to 6.7: the helper's desktop entry lists the interface. 6.8 and later: open to non-sandboxed clients. Version 3 of the interface or later |
| Budgie (labwc), Hyprland | `zwlr_virtual_pointer_v1`: wheel `axis_source`, `axis_discrete` 15.0 with step 1, `frame` | `motion_absolute` mapped to the output (manager version 2 where there are several outputs) | None. Hyprland's `ecosystem.enforce_permissions` does not block it |

KWin forwards a continuous value with no discrete step, so one unit may be 15 px or a
notch depending on the application. That is why auto-scroll measures pixels per notch and
never trusts the unit.

**KWin desktop entry.** Through Plasma 6.7 KWin offers `org_kde_kwin_fake_input` (as it
does the screencast) only to a client whose executable is the first `Exec=` token of a
desktop entry that lists the interface in `X-KDE-Wayland-Interfaces`.
`packaging/linux/record.desktop.in` lists `zkde_screencast_unstable_v1,org_kde_kwin_fake_input`,
and `AppRun` writes the same list into the per-mount entry for the AppImage's
`putgd-record`. KWin caches the answer per connection. Without the entry the helper
reports `injection_unavailable` and says so. `KWIN_WAYLAND_NO_PERMISSION_CHECKS` is never
set. The AppImage also carries `libxcb-xtest.so.0` for the X11 route.

**Pace.** The first step is one notch: what the page moved, once it rests, is the distance
per notch. Only a step that began with the page at rest measures it, and a later sample
over 1.5 times the one it has is ignored: a page that lays itself out anew during a step
(GitHub in Chrome) measured 540 px for a notch of 180. Every later step is
40% of the frame's height (at least one notch, at most 12), so the frames of its animation
overlap well: the tracker places small steps where 600 px bursts sometimes do not.

The next step goes only once the page has been seen at rest, as it is when a person
scrolls: a frame that shows what the last placed frame showed, at least 150 ms after the
page last moved, or no frame at all for 100 ms after the last one that moved. The second
rule is for streams that send a frame only when the picture changes (KWin, Mutter, image
copy). A page that never rests gets 3 s per step.

Why: while a scroll runs on, Chrome leaves tiles of the page undrawn for a moment, black at a
fixed place in the viewport. When the next step went at 85% of the last one's way, Python's
documentation came out with 126,284 black cells from row 17,150. Seen at rest, it has none. Per 10,000 px on Plasma 6.8.2, steps from rest cost 22.4 s against 16.7 s. A
still frame alone costs 71.6 s, because KWin sends the next frame about a second later. A
step at 100% of the way, not at rest, leaves more cells unwritten (189,952).

**The pointer's visit** (#1514). Between wheel steps the pointer rests on a surface of the
helper's own, outside the rectangle: the bar where it is beside the rectangle (or inside
its corner), else a clear 32 px pad at a spot 16 px outside it (not on GNOME, where only
the Shell places windows). For a step it comes in just inside the edge it rests beyond
(48 px at the output's scale), is put there again 60 ms later (the application has to see
it enter), turns the wheel, stays 30 ms and goes back. A browser waits 80 ms after the
pointer last moved before it shows a link's address, and a visit is shorter. Where nothing
scrolls at that point, the centre a third of the way down, near the right edge and two
thirds down follow, two single notches at each.

**The hint.** Auto-scroll knows how far a step will move the page but not when, since an
application animates it. Each frame's hint is the share of the movement still owed that
its time takes up of the step's expected animation; a placed frame corrects what is owed
by what the page really did. A person's scrolling gives no hint. The tracker sums the hints
since the frame it last placed. A frame it calls still without proving it keeps that sum,
and the session does not hand the same distance out again. A step that ends without
moving the page drops what it owed on both sides
([#1543](https://github.com/patrick-hudson/put.gd-desktop/pull/1543)'s review: the sum had
grown past the frame's height).

**Moving the mouse pauses it** (#1514). The helper knows where it last put the pointer.
Its rest surface reporting the pointer anywhere else, or leaving with no visit sent, is the
user. So is a pointer that is not where XTest left it (X11; the server is asked, so this also
works with no rest surface, as when the region fills its output and the bar is hidden). On
Plasma, Hyprland and Budgie, so is the pointer anywhere on a clear watch surface over the output
that leaves holes only at the wheel points, the bar and the pad. On GNOME, so is any position
the Shell extension reports other than the helper's own targets (extension 24, `pointerWatch`;
see [GNOME](../desktops/gnome.md#scrolling-capture)). The gate closes at once: nothing more is
injected, the watch and the pad go, the bar says **Paused: you moved the mouse**, and the
capture follows the user. **Auto-scroll** (once the mouse has been still for 500 ms) or
`scroll.autoScroll {on: true}` turns it on again from wherever the page is.

**It hands the wheel back by itself** when the page moves up while it turns it down (you
scrolled: **Paused: you scrolled**), and when nothing scrolls at any of its wheel points
(**Nothing scrolled here**). A jump up counts as yours only if the next frame agrees: the
page stays up or goes on up. One frame placed far up and then the page back on
auto-scroll's way was a picture out of place. On a starved X server a grab caught the
window half repainted, and the wheel was handed back to nobody
([#1535](https://github.com/patrick-hudson/put.gd-desktop/issues/1535)). It also hands it back when the tracker is lost and five steps
back do not find the page. Each step back is judged on at least three frames and 2.5 s,
because a lost tracker takes about a second per frame. It does the same after more than
five ways back in a row that did not make the image grow. The bar then says **Auto-scroll
lost the page** and **Scroll back a little**. Once you have scrolled back to where the image
finds the page, it says **Found the page again** (`paused`, `pausedBy: lost`), and
**Auto-scroll** carries on from there. The capture goes on following you each time.

**The end of the page.** A step that moved nothing for its response time (600 ms, or three
times the slowest answer seen, at most 3 s) is still. A second one, of one notch with
twice the patience, that also shows the page where it was in frames that arrived after its
response time, is the end, but only at the centre of the region: still steps at another
wheel point move the wheel to the centre, which keeps it from then on, and the two steps are
made again there. (A side bar that sticks once the page has moved can take the wheel for its
own list; Python's documentation in Chrome ended at 1,071 of 110,440 px before this rule.)
The stream alone does not prove the end: one that sent no frame (a damage-driven stream
while the screen is still) has nothing from after the steps, and one that went stale sends
its last picture again with new times. So every end is taken from a fresh frame of the
screen itself (image copy: a new capture session; PipeWire: the stream linked again; X11: a
grab after the call), which must show the page where it was. A fresh frame the tracker
cannot place is no end: auto-scroll steps on, or goes back when lost. (KWin's stream stopped sending new pictures while Chrome went on
scrolling, twice on 2026-11-14, and the restarted stream's first frame came 33 to 56 s
later showing the page elsewhere.) The capture then finishes by itself.

**Scroll to the top first** (`preferences.scrollCaptureFromTop`, off by default; it applies
to auto-scroll only) runs when auto-scroll begins before anything has moved. Steps up of
10 notches, doubled to 40 while the page moves; the top is two checked 10-notch steps that
changed nothing and a frame of the screen that agrees. Still steps prove the top only at a
wheel point where a step of the way moved the page. Where none has, the wheel may be over a
part of the page that takes no wheel, so the way tries the other wheel points, one more
step at each. Only when every point has moved nothing is the screen taken as the top (a
page already at its top, or one that does not scroll here). Auto-scroll then keeps the
point that moved the page, or looks for one from the first. At most 90 s, and the way up gives
up when no check has found the page at rest for 15 s. The image then starts again with the
screen at the top; `topReached` says whether the top was proved. Moving the mouse or Done
on the way up keeps the screen as it is then.

**Switching** between the two drivers keeps the image: the tracker follows the page
through both.

### Finishing with the keyboard

Esc finishes a capture on X11: the helper reads it as an XInput 2.1 raw key press, so it
takes no key from the application (Esc also still reaches the page). On every desktop the
`scroll` hotkey pressed again finishes it, where one is bound (Settings → Hotkeys); the bar
names the key (`finishKey`). Wayland lets no client see a key it has not bound, and put.gd
binds no key for the length of a capture: on Plasma, GNOME, Hyprland and Budgie there is no
Esc, and with no `scroll` hotkey bound the bar's **Done** and the tray's **Capture scrolling
region** (which is Done during a capture) are the controls.

## The bar

| State | Says | Then |
| --- | --- | --- |
| `waiting` | Scroll the page | Done (or the key) when you reach the end |
| `following` | Following | Done (or the key) when you reach the end |
| `lost` | Lost the page (Auto-scroll lost the page, after it gave up) | Scroll back a little |
| `scrollingToTop` | Scrolling to the top… | Move the mouse to pause |
| `autoScrolling` | Auto-scrolling | Move the mouse to pause |
| `paused` | Paused: you moved the mouse / Paused: you scrolled / Found the page again / Nothing scrolled here | Scroll on, or Auto-scroll to go on / Scroll the page yourself |
| `resuming` | Auto-scroll in a moment… | Let go of the mouse |
| `finishing` | Saving… | |

Beside them, the height so far. Buttons: **Auto-scroll** (a toggle), **Done** (enabled
from the first frame on, so in every state) and **Cancel**. One size for all states,
sized for the longest texts, so the buttons never move. It uses the app's style
(`putgd::Ui` roles and IconArt glyphs) and its states are in `putgd-ui-catalog`
(`scroll-bar-*`).

It goes beside the rectangle on the rectangle's own screen (below, above, left, right,
the corners outside), then on another screen. With no room anywhere (one monitor, a region
that fills it) it sits inside the rectangle's bottom-right corner, 10 px from its edges,
and the tracker is told that its rectangle (with 6 px around it, 24 px on X11 for the window
manager's shadow) is not the page (#1519, `ScrollTracker::setExcluded`): its pixels never
vote and are never drawn, and the page under it comes from frames in which it was clear. The
page's last rows beside the bar's place at the very end of the page were never clear: they
get their column's plain colour, and the result counts them in `hiddenCells` and
`hiddenRows`. The wheel is not turned over it. GNOME's Shell places it the same way. Where even the corner is too small for it, the bar is
hidden and the `scroll` hotkey, the tray and Esc on X11 finish the capture.

The finish hint names the keys that work: **Done, Esc or** the bound hotkey on X11 when both are available. When there is no room beside the region, the bar stays inside the available screen area.

## Output and limits

The image lives in a private spool file under `<data>/scroll/<id>/` (0700 directory, 0600
file), as the tracker writes it: a sparse file with room for the cap's rows and a frame
above the first frame and the cap's rows and two frames below it, page row r at file row
r plus the room above. Only rows written take disk. At the end the rows the image has are
one contiguous block of the mapped file, written as a PNG within a bound, and the spool is
deleted. When the way to the top proved the top, the spool is emptied and the image starts
there. Abandoned staging is swept at start, as for recordings. Finished files survive
helper exit until the resident has admitted them.

- **Height cap.** The image never exceeds the cap, which is the smallest of
  `preferences.scrollCaptureMaxHeight` (2,000 to 60,000, default 30,000),
  `limits.imagePixels / width` and `limits.imageBytes / (4 × width)`; for the editor route
  also `EditorJob::admissionPixels / width` (24 MP, 15,000 px at 1,600 wide). The helper
  reports the height it applied as `appliedMaxHeight`.
- **Idle bound.** A capture with no frame placed with movement and no command for 10
  minutes ends by itself. Where a placed frame ever moved (the user's scrolling or
  auto-scroll's, over the whole session; the way to the top is not the image's movement), it
  ends as Done, the image kept, with a note. Where none ever did, the capture was forgotten:
  it ends like Cancel (the session's `Forgotten`). The helper sends `cancelled` with
  `reason: idle` and removes its files. The resident ends the operation cancelled, admits
  nothing (no save, copy, upload or History row) and closes the bar and the ring. The view
  and the operation note "Nothing was scrolled for 10 minutes, so the capture ended without
  an image."
- **Region.** On one output and at least 64 px each way (`scroll_region`).
- **Admission.** The resident checks the file (inside the session's directory, regular,
  owned, the declared size and SHA-256, the PNG header and IHDR size, bounds) and the
  result's promises (below), and re-enters the normal path as a `png` clipboard payload;
  through the editor as a one-file payload. A result that fails is `scroll_output_invalid`.
- **Done and Cancel.** Done keeps the image, in every state. Before it finishes, Done takes a
  fresh frame of the screen (the stream linked again, at most 1.5 s): KWin's region stream
  once sent the same picture 92 times while Chrome went on scrolling, and the last rows of a
  person's capture were missing. Cancel discards the files and
  leaves no History row. The helper answers a Cancel only when its worker has ended, so the
  resident remembers a Cancel that is on its way and it decides the end: other commands are
  answered `scroll_busy`; a pending Done is answered `cancelled`; a `finished` event is
  ignored and the image discarded. A Done whose image was admitted before the Cancel arrived
  has ended the session, and that Cancel gets `scroll_not_found`. Closing the control pipe,
  or the helper leaving, ends injection at once and removes the files of an unfinished capture.

## Results and end reasons

`finished`, `scroll.inspect` and the MCP result carry:

| Field | Meaning |
| --- | --- |
| `endReason`, `truncated` | What ended it, and whether the image is short of what was meant (table below) |
| `scrolledBy` | `user`, `putgd`, `both`, or `none` (nothing moved: one screen) |
| `framesPlaced`, `framesSkipped` | Frames the tracker placed (the first one included) and skipped |
| `endedLost` | The capture ended while the tracker was lost |
| `reachedEnd` | Auto-scroll proved the end of the page |
| `topReached` | `false` when the way to the top was asked for and could not prove the top; absent otherwise |
| `note` | Why, for `lost_track`, `top_not_reached`, the idle bound and auto-scroll stopped at a blank stretch |
| `hiddenCells`, `hiddenRows` | Cells of the page that only the bar inside the region ever covered (filled with their column's plain colour), and the rows that hold them. `hiddenRows` also counts rows that no placed frame covered (since a frame is placed on the hint only where it overlaps the image, none should occur). Absent when there are none |
| `foundAgain` | Times the tracker lost the page while the user scrolled and found it again; included in the helper result, `scroll.inspect`, `putgd-cli` and MCP `get_scroll_capture` |
| `stats` | `steps`, `pixelsPerNotch` (auto-scroll's), `lowConfidenceSteps` (frames placed by expectation or on the hint), `elapsedMs`, `frames`, `recovered`, `pinnedRegions`, `framesPerSecond`, `rowsAtDone` (rows that Done's fresh frame added) |

| `endReason` | `truncated` | Meaning |
| --- | --- | --- |
| `end_of_content` | false | Auto-scroll proved the end of the page and no frame was skipped, and where the way to the top was asked for, the top was proved: the page from the top (or from where it was) to its end |
| `top_not_reached` | true | The same, but the way to the top could not prove the top: the image starts partway down. `topReached` is false and `note` says why |
| `done` | false | Finished by Done, the hotkey, Esc, `scroll.stop`, the idle bound, by auto-scroll at the end of the page after skipped frames (`reachedEnd` true, `framesSkipped` above 0), or by auto-scroll at a stretch that showed nothing (`reachedEnd` false, a note): the page as far as it was scrolled |
| `lost_track` | true | Finished while the tracker was lost: the image ends where it last placed a frame, and the page went somewhere it could not follow |
| `height_cap` | true | The cap was reached first |

`lost_track` no longer ends a capture by itself: the capture waits for you to scroll back,
or for Done. The resident refuses a result that breaks these promises:
`end_of_content` without `reachedEnd`, with a skipped frame or with `topReached: false`;
`top_not_reached` without `topReached: false`; `lost_track` without `endedLost`;
`truncated` other than the table says; no frame placed.

`height_cap`, `top_not_reached` and `lost_track` raise a warning notification ("put.gd —
scrolling capture stopped" or "… incomplete") and put a note on the History row: "Stopped
at 30,000 px. The page continues below.", "Could not reach the top of the page. The image
starts partway down." and "Finished while put.gd had lost its place. The image ends where
it last found the page." The idle bound puts "Finished by itself after 10 minutes without
scrolling." on the row with no notification (a capture in which nothing ever moved has no
row: it ends like Cancel). When the upload or the local save failed, the
row's status shows that first and the note follows after a `·`. No confirmation dialog is
ever shown.

The resident releases the helper as soon as it has validated the PNG, before the image
leaves the upload queue, so a new capture can start while the previous one still waits to
upload.

A capture that keeps nothing fails instead: `capture_timeout` (no frame in 3 s, or modifier
keys held when auto-scroll was to begin), `display_changed` (the frame size changed),
`scroll_unavailable` with `injection_unavailable` or `frame_source_unavailable` (the
desktop refused the input or the stream ended), `encoding_failed` or `input_excessive`
(the spool or the PNG). The codes and what to do are in
[errors](../user/errors.md#scrolling-capture).

A positive `foundAgain` adds a History note and, when there is no other warning, the notice `scroll_found_again`, titled **put.gd — check the scrolling capture**: "The capture lost the page once and found it again. Rows may be missing there: check the image." For more than one it says "N times" instead of "once". The sentence is appended when another note exists; `endReason` and `truncated` stay unchanged. An existing warning keeps its notice kind.

## Known limits

- [#1557](https://github.com/patrick-hudson/put.gd-desktop/issues/1557): small white boxes can cover content in captures you scroll yourself on Cinnamon.
- [#1558](https://github.com/patrick-hudson/put.gd-desktop/issues/1558): on Cinnamon at about 30 frames per second, captures you scroll yourself lose the page often.
- [#1547](https://github.com/patrick-hudson/put.gd-desktop/issues/1547): auto-scroll can lose one period on a page that repeats every step.
- [#1554](https://github.com/patrick-hudson/put.gd-desktop/issues/1554): moving the mouse on Hyprland does not always pause auto-scroll (8 of 20 lab guest runs).
- [#1537](https://github.com/patrick-hudson/put.gd-desktop/issues/1537): a playing video can hold a capture you scroll yourself for minutes.
- [#1559](https://github.com/patrick-hudson/put.gd-desktop/issues/1559): without a watch surface the pointer can be put back late; X11 can report stale hand events.

- **A cookie banner at the bottom under a sticky header** leaves the tracker a narrow band to
  compare. On a news article in Chrome (Plasma 6.8.2) it skipped most frames, and the banner's
  colour was drawn in strips through the image while the capture ended `done`. Read
  `framesSkipped`: a capture that skipped many frames needs a look.
- **Pinned parts that do not line up with the tracker's 8-row blocks** can leave a row or
  two behind. A toolbar whose edge falls two rows into a block of the page keeps that
  block from being held back; a page row seen only there (after a flick past it) is
  written from the toolbar's edge. A banner that slides in at the page's own speed for a
  few frames (Wikipedia's fundraising banner in Chrome) moves like the page while it does,
  and its border can be left in the image where it arrived. A line along the frame's top
  or bottom edge (Firefox's content edge on Cinnamon, one row inside the region) is mostly
  kept out ([#1796](https://github.com/patrick-hudson/put.gd-desktop/issues/1796)). After
  the page moves, the tracker checks each row of a waiting frame's first and last 8-row
  blocks on its own: of the row's textured cells still waiting to be written, it counts
  only those whose page row is in sight and not held back where the next frame shows it.
  When at least 90% of those cells, and at least 16, look the same at the same place of the
  screen and different where their page row moved to, that row's matching cells are not
  written. Cells held back there keep their piece of the line (the scroll
  bar's column), and so does a pinned part drawn from the first screen (Firefox's link
  bubble at the bottom).
- **Frames that come far apart over a page that repeats** (a stream that stalls to about 4-8 frames a second on a
  pull request's tables) can land a table period short, or more, after the tracker loses the page. It happens in two
  ways ([#1823](https://github.com/patrick-hudson/put.gd-desktop/issues/1823)):
  - After a lost stretch, the frame pair takes a look-alike place while the page has gone past the image's end, where
    the image holds no rival place.
  - A distant frame is judged "no movement" after ambiguous skips, and a later frame becomes the reference at the
    wrong place.

  Both count as finding the page again, so the capture carries the "lost the page … found it again. Rows may be
  missing there: check the image" notice. One guard that rejected the first look-alike only led to the next one.
  Requiring a second agreeing frame before a place found again is taken is the design change for it
  ([#1847](https://github.com/patrick-hudson/put.gd-desktop/issues/1847)). A look-alike ahead in the same direction
  after a jump of more than a screen did not show on recorded frames and stays a limit
  ([#1797](https://github.com/patrick-hudson/put.gd-desktop/issues/1797)).
- **A header with a 1-pixel border** whose block rows are joined only by that border down the frame's edges can let
  header pixels into the captured page. The two cell columns nearest each side of the frame no longer join a held-back
  bar up or down, because the window's scroll bar there once stretched GitHub's header 280 rows down the page and those
  rows were painted flat ([#1837](https://github.com/patrick-hudson/put.gd-desktop/issues/1837)). In 8 of 4,000
  randomized scenes this left 300-486 pixels different from the expected image, roughly one row's worth.
- **A header that shows up while the steps are over half the frame** leaves page rows
  that only one frame showed clear, and a few of them can come from under it (nightly did
  the same).
- **What changes in every frame inside something that sticks** (a video advertisement in
  a sticky rail beside the Al Jazeera article in Chrome) can leave a few of its pixels in
  the page beside it, where the tracker had too little evidence to hold it back.
- **A header that sticks from the second frame on** over rows only the first frame showed
  clear can leave its own flat colour there where that colour reaches a vote lead of
  three or more: a lead of three outweighs one screen (which is what drops a notice
  dismissed after the first frame, once the page's colour under it reaches that lead).
- **An information bar above the page that closes while a banner is pinned at the
  bottom**: if the last screen kept for what is pinned still shows the banner clipped
  (no later frame replaced it), the image's last rows can lack the banner's rows the bar
  had pushed out of view (24 rows of a 64-row banner in a synthetic case, when the
  capture ended right after).
- **Something that slides in during the first frames** (Chrome's link bubble at the bottom
  left, from auto-scroll's visit with the pointer) can move by the page's step for a frame
  and be taken as the page there.
- **A blank stretch taller than the region** shows nothing that moves (fewer rows with
  texture than the tracker needs as evidence, leaving out what it has learned as pinned, the
  bar and the side margins, so a pinned header and footer do not count). Auto-scroll stops
  there: frames with nothing on them show no movement, so its steps look still, though the
  wheel may still be moving the page. It does not call that the end of the page: the capture
  ends `done` with `reachedEnd: false`, and the resident shows the note "Auto-scroll stopped
  where the page showed nothing" in a notice, in History and in the result. Your own
  scrolling past it leaves the page where the tracker cannot place it (lost) until you
  scroll back. Including the scrollbar does not help: the tracker leaves the
  side margins out of its vote.
- **The bar inside the corner** hides the page beside its place at the very end of the page:
  those cells are plain, and `hiddenCells` counts them.
- **Pages that lay themselves out anew under the viewport** while it is scrolled (a long
  GitHub pull request in Chrome, near 20,000 px) leave frames no offset explains: the
  tracker is lost. You get it back by scrolling back a little; auto-scroll goes back by its
  step up to five times and then hands the page to you.
- **While lost, the tracker looks for the frame in the whole image**: on a 20,000-row image
  that takes about a second a frame, so the bar answers slowly then.
- **Chrome under auto-scroll** can show parts of the page it has not drawn yet while it
  scrolls fast; on Python's documentation (dark theme, Plasma 6.8.2) some of them, black,
  were taken into the image below 17,000 px. Not seen when you scroll yourself, nor in
  Firefox. Open.
- **Foreign overlays** (a notification of another application over the rectangle) are
  pinned parts or changes in place to the tracker, and can be in the image. put.gd's own
  notices are handled; the Omarchy dismissal is by summary, so a card of another
  application with the same summary goes with them.
- **Hover effects** of the pointer, a tooltip and a browser's link-status bubble can be in
  the image while you scroll with the pointer over the page. Under auto-scroll the pointer
  waits outside and visits for the wheel step only.
- **Hyprland paints the pointer into the frames.** Under auto-scroll the frames of a visit
  (and one after it) do not go to the tracker; your own pointer over the page can be in the
  image.
- **A build with no region selector has no scroll capture, for any caller.** The Ubuntu
  24.04 DEB on KDE Wayland is built without LayerShellQt for Qt 6 (`cmake/Platform.cmake`),
  so it has no layer-shell selector and no layer-shell surfaces for the ring, the bar and
  the heartbeat. The probe reports `selection: none`, `available: false`, reason
  `region_unavailable`; the helper's `prepare` refuses on it, and the resident refuses
  `scroll.prepare` with or without `rect` as `scroll_unavailable`, reason
  `region_unavailable`, before an operation is created. The AppImage, which bundles the
  selector, works.
- **GNOME: an extension loaded before version 22** has no `scroll` hotkey
  (`helper_outdated` until the next login) and no beat. Scroll capture needs version 22 or
  later (`selectsRegions`).
- **Only vertical scrolling** exists. Horizontal scrolling, scrolling by key presses under
  auto-scroll, window and monitor targets and regions that span outputs are out of scope.
  While you scroll yourself, Page Down and the arrow keys work.

## Agent control

Every step is a session method: `scroll.capabilities`, `scroll.prepare {rect?, maxHeight?,
driver?}`, `scroll.select`, `scroll.start`, `scroll.autoScroll {id, on}` (and `scroll.pause`,
`scroll.resume`, its names from #1514), `scroll.stop` (Done), `scroll.cancel` and
`scroll.inspect`, with the MCP tools `prepare_scroll_capture`, `control_scroll_capture`
and `get_scroll_capture`. While a capture runs, `scroll.inspect` says `activity` (the bar's
state), `driver`, `paused`, `pausedBy`, `height`, `placed` and `skipped`. See
[CLI](../automation/cli.md#scrolling-capture) and [MCP](../automation/mcp.md#tools-and-effects).
`scroll.prepare` never starts by itself: select (or pass `rect`), then `scroll.start`.
Auto-scroll sends real input to whatever is under the rectangle: point it only at a window
you opened for the purpose.

## Verification

The suites and what they prove are in [Testing](../testing/testing.md#scrolling-capture-suites);
the acceptance rows are in the [acceptance matrix](../testing/acceptance-matrix.md#scrolling-capture-rows).
