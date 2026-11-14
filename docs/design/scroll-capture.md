# Scrolling capture

A scrolling capture takes one tall PNG of content that is longer than the screen. You
select a region over scrollable content; put.gd turns the mouse wheel over it with
injected wheel events, stitches the frames into one image and sends that image down the
same route as a region capture. It is
[#1376](https://github.com/patrick-hudson/put.gd-desktop/issues/1376). The user's view is
in the [user guide](../user/user-guide.md#capture-a-scrolling-region); this page is the
contract.

It never asks the application how long its content is. It finds the top and the bottom by
reaching them, and says so when it could not: only the end reason `end_of_content` means
the whole scroll area, top to bottom, is in the image (with `scrollCaptureFromTop` off, from
the position the capture started at; `topReached` says which).

## Workflow

1. **Start.** The tray's **Capture scrolling region**, the main window's button, the
   `scroll` hotkey (unbound by default), `action.invoke {"action":"scroll"}` or the MCP
   `perform_action`. `editor.open {"source":"scroll"}` does the same and opens the result
   in the [editor](image-editor.md) instead of sending it on.
2. **Select.** The recording region selector opens, live as for a recording. The
   ring sits just outside the rectangle, and a small bar (state, height so far, a line
   that says what the mouse does, **Stop**, **Cancel**) sits outside it too (on X11 32 px
   from the rectangle, so that the window manager's shadow around the bar stays out of
   the last rows; 10 px elsewhere). The bar goes beside the rectangle on its own screen
   wherever it fits: below, above, left, right, then the corners outside it, and only then
   on another screen ([#1514](https://github.com/patrick-hudson/put.gd-desktop/issues/1514)).
   It has one size for every state, so it never moves and Stop stays where it is.
3. **Capture.** It starts by itself after the drag. put.gd keeps the pointer outside the region,
   on the bar or a small clear pad of its own, and brings it in for each wheel step. It scrolls
   the content to its top first (`preferences.scrollCaptureFromTop`, on by default), then sends
   wheel steps down and stitches until the content stops moving. While it runs, the bar says
   "Move the mouse to pause".
4. **Pause.** The moment the user moves the mouse the capture stops sending anything and the
   bar says "Paused: you moved the mouse", with **Resume**, **Stop** and **Cancel** (see
   [the user's mouse](#the-users-mouse)).
5. **End.** At the end of the content, at the height cap, on Stop, or when it
   cannot follow the content any more. Stop keeps the image so far; Cancel discards it.
6. **Result.** One PNG, routed by the `scroll` action's output like a region capture
   (local copy, upload, History, link copy). Through the editor, by the `edit` action's.

One scroll session exists at a time. It excludes recording, the editor and any other
selection: `scroll_active` for a request that arrives during one (the record action,
`recording.prepare`, a screenshot), and `recording_active`, `editor_active` or
`busy_selecting` for a scroll request during those. A second `scroll` invoke while one is
capturing is Stop. Installing an update is refused while a capture runs.

## Process boundary

Scroll capture is a second session kind of `putgd-record`. The helper already has
region selection, the ring, bar placement, prompt-free frame streams and the staging
discipline of a recording on every desktop; a scroll session replaces the encoder with a
stitcher. Frames never leave the helper and the tall raster never enters the resident. A
build without `PUTGD_RECORDING` has no scroll capture: `scroll_unavailable`, reason
`recording_helper_missing`.

| Part | Source | Role |
| --- | --- | --- |
| `ScrollCaptureService` | `src/app/ScrollCaptureService.*` | The resident's session: states preparing, selecting, ready, capturing, finishing, then done, failed or cancelled. Commits the operation to the journal before any frame is read, validates and admits the result, keeps the last 32 results inspectable |
| `ScrollBackend` | `src/app/ScrollBackend.h` | The seam, and the only thing the service knows; tests inject a fake |
| `ScrollHelperBackend` | `src/app/ScrollHelperBackend.*` | The production backend: one `putgd-record` per session over the private control framing, as `RecordingHelperBackend` does |
| `ScrollCapture` | `src/record/ScrollCapture.*` | The helper's session: selector, ring, bar, frame tap, injector, worker thread, spool and PNG |
| `ScrollRun` | `src/record/ScrollRun.*` | The control loop (standard library only, so the same loop runs against a synthetic application in tests) |
| `ScrollStitcher` | `src/core/ScrollStitcher.*` | The offset finder and row assembler |
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
  they name a `rect` (see [no region selector](#known-limits)).
- `prepare {rect?, maxHeight, maxPixels, fromTop, autoStart, selection}` then `select
  {rect}`, `start`, `pause`, `resume`, `stop`, `cancel` and `overlay {visible, rect?}`.
  Replies come only once the helper reached the state. `pause` and `resume` answer
  `{state: "capturing", paused, pausedBy?}`; `resume` of a capture that is not paused is
  `scroll_state`. `stop` keeps the image, and its reply is the object a later
  `scroll.inspect` returns. `cancel` is accepted while a `start` or `stop` is pending (it
  is never `scroll_busy`); a `start` or `stop` waiting for the helper then ends with the
  session. Once a `cancel` is on its way it decides the end: see
  [Stop and Cancel](#output-and-limits).
- Events, each with the session id: `selected {rect}`; `progress {state: scrollingToTop |
  capturing | paused, paused, pausedBy?, height, steps, pixelsPerNotch}`; `finished {path, bytes, sha256, width,
  height, endReason, truncated, appliedMaxHeight, topReached?, stats}`; `cancelled`;
  `failed {error}`.
  After `finished`, `cancelled` or `failed` the helper injects nothing more for that id.
- Deadlines: 10 s for the helper to start, 15 s per command, 30 s for `stop`, 180 s for
  the capture itself, not counting the time it is paused. A missed deadline is `scroll_helper_failed` with the check
  (`start_timeout`, `command_timeout`); a helper that dies, answers out of order or
  floods stderr is the same code (`exited`, `crashed`, `protocol`, `build`). A session client
  waits 15 s past those deadlines, so the resident's own answer arrives (40 s for the
  `scroll.*` methods, 45 s for `scroll.stop`).

On GNOME the Shell extension selects the region and draws the ring, and the resident
places the bar through it (`ScrollShell`, a `RecordingShell` of the scroll session). The
resident sends the helper where the Shell put the bar (`overlay {visible: true, rect}`), so
the pointer can rest on it. A capture with no spot for the bar outside the rectangle hides
it for the whole capture. The Shell still places the bar below or above the rectangle, then
on another monitor: the sides of the rectangle are not tried there. The
bar never takes the keyboard and has no handle inside the frame.

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
unavailable.

| Desktop | Stream | Cursor and scale |
| --- | --- | --- |
| X11 (Cinnamon) | `ximagesrc` on the region | `show-pointer=false` |
| GNOME | Mutter `RecordArea` in a screen cast linked to a RemoteDesktop session | `cursor-mode` 0. The Shell shows its screen-sharing indicator while it runs; ending the session from it is Stop. The extension repaints one transparent pixel of the area every 40 ms (`beat`), because Mutter's area stream sends a frame only for damage inside it |
| Plasma | KWin `zkde_screencast_unstable_v1` `stream_region` | pointer hidden, at the output's scale (from protocol version 5 scale 0 lets KWin pick). A 1×1 layer surface commits every 30 ms while frames are read, because KWin drops an isolated repaint |
| Budgie (labwc), Hyprland | `ext-image-copy-capture-v1` through `appsrc` and `videocrop` | session created without `paint_cursors`. On the Omarchy lab guest Hyprland still composited a software cursor into every copy |

The frame size is the logical rectangle at the output's scale, and the stitcher needs rows
that repeat bit for bit. Lab runs at 125% on Plasma and GNOME and at 150% on Cinnamon X11 gave
exact images; a compositor that resamples a client's buffer would not.

## Scroll injection

Only wheel steps and pointer motion are injected: never a button, never a key. Each
injector turns stream pixels of the rectangle into its desktop's coordinates. Before every
wheel step the loop moves the pointer to where the wheel is turned (rule 3 of
[the control loop](#the-control-loop)), because the wheel goes to whatever is under the
pointer.

| Desktop | Wheel | Pointer | Access it needs |
| --- | --- | --- | --- |
| X11 | XTest buttons 4 and 5, press and release per notch | XTest absolute motion; the user's pointer is read first and put back at the end | The XTEST extension. The loop waits up to 10 s for Ctrl, Shift, Alt and Super to be up first (Ctrl plus wheel zooms); `capture_timeout`, check `modifiers_released`, when they stay down |
| GNOME | `org.gnome.Mutter.RemoteDesktop` `NotifyPointerAxisDiscrete` on a session linked to the screen cast | `NotifyPointerMotionAbsolute` on the area stream | None, but every call must come from the D-Bus connection that created the session. The interface is private to Mutter. No owner or a refused `CreateSession` is `injection_unavailable` |
| Plasma | `org_kde_kwin_fake_input` `axis(vertical, 15.0)` per notch, after one `authenticate`; an `axis` of 0 says the wheel has come to rest | `pointer_motion_absolute` in global logical coordinates | Plasma 6.3 to 6.7: the helper's desktop entry lists the interface. 6.8 and later: open to non-sandboxed clients. Version 3 of the interface or later |
| Budgie (labwc), Hyprland | `zwlr_virtual_pointer_v1`: wheel `axis_source`, `axis_discrete` 15.0 with step 1, `frame` | `motion_absolute` mapped to the output (manager version 2 where there are several outputs) | None. Hyprland's `ecosystem.enforce_permissions` does not block it |

KWin forwards a continuous value with no discrete step, so one unit may be 15 px or a
notch depending on the application. That is why the loop measures pixels per notch and
never trusts the unit.

**KWin desktop entry.** Through Plasma 6.7 KWin offers `org_kde_kwin_fake_input` (as it
does the screencast) only to a client whose executable is the first `Exec=` token of a
desktop entry that lists the interface in `X-KDE-Wayland-Interfaces`.
`packaging/linux/record.desktop.in` lists `zkde_screencast_unstable_v1,org_kde_kwin_fake_input`,
and `AppRun` writes the same list into the per-mount entry for the AppImage's
`putgd-record`. KWin caches the answer per connection. Without the entry the helper
reports `injection_unavailable` and says so. `KWIN_WAYLAND_NO_PERMISSION_CHECKS` is never
set. The AppImage also carries `libxcb-xtest.so.0` for the X11 route.

## The control loop

`ScrollRun` runs on a worker thread and blocks. Its waits are in slices of at most 100 ms,
so Stop is seen soon.

1. **Scroll to the top** (`scrollCaptureFromTop`). Wheel up until the top is proved: two
   checked steps of 10 notches in a row that moved nothing (two, because one wheel step
   can go nowhere), and a fresh frame grabbed from the screen that agrees, because a
   damage-driven stream that sends nothing looks the same as a page at rest. No stitching
   happens here. When the run later switches to another pointer target (rule 7), the
   top is proved again there before the capture goes on, and the first viewport is taken
   anew; no downward step has been kept by then.
   - *Pace.* Every fourth step is a check: the picture settles and is compared with the
     picture at the check before. The steps between are sent as soon as a frame from after
     the wheel step differs from the one before it. While checks find movement the steps
     double, 10, 20, then 40 notches. A larger step that moved nothing counts for
     nothing (an application may clamp or refuse a large wheel event): the step goes back
     to 10. A check that finds the page where the last one found it, although the steps
     between looked like movement, makes every later step a check.
   - *Time.* The way up takes what the page needs, at most 90 s (half the 180 s total, so
     a capture can still follow). It had 15 s until fix round 4: a 27,000 px text view
     needed 17 s on a Hyprland guest, and the capture then began at line 78 and was
     reported as `end_of_content`.
   - *Giving up.* The way up ends without proof when its time is over, when no check has
     found the picture at rest for 15 s (a video or an animation cannot be told from a
     page that still scrolls), or when the source gives no fresh frame for the proof.
     The capture then goes on from where the page is, and the result says so: its
     `topReached` is false, and an end of the content is reported as `top_not_reached`,
     never as `end_of_content` (see [End reasons](#end-reasons)). The helper's log has
     `scroll: way to the top N steps, M ms, top proved` or `top not proved (why)`.

   Where the desktop can say the wheel has come to rest (KWin), it does, so that an
   overscrolled GTK 4 view springs back. If nothing moved at all the capture goes on, and
   step 7 decides whether the region scrolls. The pointer is in the
   rectangle for each wheel step up only, as later (rule 3). Where the frames show a painted
   pointer (Hyprland, labwc) that also keeps an arrow out of the viewport that Stop keeps.
2. **First frame**, with the pointer outside the region on its output, so no hover effect is
   in it. The place is on the bar when it is beside the rectangle (see
   [the user's mouse](#the-users-mouse)), else 16 px outside the rectangle, a third of the way
   down its left side, else the same on its right side, else above or below its middle. The
   pointer then has 450 ms (`awayLingerMs`) to let go of what it called up before the
   capture, such as a browser's link panel. With no such place the pointer stays where it
   is, except where the frames show a painted pointer: it then goes to the bottom right
   corner.
3. **The pointer's visits.** On every desktop the pointer waits at that place outside while
   frames are read (`RunOptions::restAway`) and is in the rectangle only for a wheel step.
   Where the helper has a surface of its own there, the place is on it (see
   [the user's mouse](#the-users-mouse)).
   A pointer that rests on the page makes the page answer it: a browser shows the address
   of a link under it in a panel at the bottom of the viewport, in the very rows the next
   step appends. For a step the pointer comes in to the point `entryInset` inside the edge
   it waits beyond (48 px at the output's scale, at least 24), level with where it waits,
   arrives, is put there again 60 ms later (the application has to see it enter first),
   turns the wheel, stays 30 ms (`leaveDelayMs`) so that the application has the step with
   the pointer on it, and leaves. A browser waits 80 ms after the pointer last moved before
   it shows a link's address, and a visit is shorter. The point is just inside the edge so
   that the pointer's travel stays short: on Plasma a pointer that goes back and forth over
   more than a hundred pixels several times a second is, to KWin, a pointer its user shakes
   to find it, and KWin blew it up to many times its size and delivered the region's frames
   in bursts. Where nothing scrolls at that point (rule 7) the classic places follow, with
   the same visits: the horizontal centre a third of the way down, near the right edge, two
   thirds down. Without a place outside (the rectangle covers its output) the pointer stays
   parked at the centre as before; rows a frame shows above the parked pointer's margin are
   then not taken in place of rows that wait for the next frame (see
   [the edge guard](#what-counts-as-proved)).
4. **Settle by frame cadence.** After a step the picture is settled when no frame in the
   moving band has changed for 120 ms, or for twice the longest recent gap between frames
   (at most 600 ms), whichever is longer, with a hard limit of 1.5 s. The picture also gets
   a response time to begin to change: at least 500 ms, or three times the slowest response
   seen, at most 3 s. A slow guest that delivers a frame every 150 ms would otherwise show a
   mid-animation frame as the settled one. The response time is counted from the wheel
   step, not from the start of the wait: the pointer's 30 ms after the step would otherwise
   let a page that answers at once (Plasma without animated scrolling) answer before the
   wait began, and every step would wait out the whole response time.
5. **Plan a step.** The first step is one notch, and its distance is the pixels per notch.
   Later steps aim for 60% of the moving band and never more than 75%, never so far that
   fewer than 36 informative rows of the reference stay in the overlap, never so far that
   the appended rows start less than 24 rows below a pointer parked on the page (48 where the
   frames show a painted pointer), and at most 64 notches. A pointer that waits outside and
   only visits for the wheel step is not on the page while frames are read and limits
   nothing: with its rest below the rectangle the wheel point is near the bottom edge, and
   that margin once made every step one notch.
6. **Prove stillness before the end.** A step with no movement waits at least 400 ms and is
   repeated, as one notch whose answer gets twice the response time (more notches are only more
   for a busy page to work through before it can answer). Two still steps in a row are the end,
   and only when a frame grabbed from the screen after the last injection agrees with the last
   one seen. With no such frame the end is not proven and the capture stops as `lost_track`.
   Where there is reason to doubt a still step, the end is proved by the end proof (below) instead.
   A still step says only that the picture did not move in the time it was given: a page that
   works through its wheel notches one after another and paints once none is waiting is still
   for as long as it is busy, and two such steps once ended a capture as complete with 1,020 of
   24,000 rows.
   **The end proof** (`ScrollRun::proveEnd`) runs when an earlier still step was followed by
   movement after all, when the last step moved all of the distance it asked, or when the page's
   answer takes longer than the picture's settle time (a short step may then have been seen
   halfway). A page answers its wheel steps in the order they came, so the loop sends one notch
   up and requires the page to move: everything sent before has then been answered. A step up
   that is not answered is sent again twice as large with twice the patience (1, 2 and 4 notches),
   because a view overscrolled at its edge spends its first steps back. Then it sends one notch
   more down than it went up and requires the page to return to exactly the picture the capture
   ended on; where that is too far to place in one frame (a notch near half of a short
   selection's band) it comes down in two steps. Back where it ended is the end of the content.
   A page that is further down goes on as after any step (the time it took becomes the least the
   loop waits for an answer). A page that never answers the step up, or does not come back, ends
   the capture `lost_track` with the note "the page did not answer a wheel step up and back down,
   so the end of the content is not proven". At the end of a capture of a slow page the page may
   therefore move up and back down once, and such a capture takes longer.
7. **No movement at all.** If nothing has scrolled down yet, each place for the wheel gets two
   single notches (four at the first after a way up), then the next: just inside the edge the
   pointer waits beyond, then the centre a third of the way down, near the right edge, two
   thirds down, then `scroll_no_movement`. (Three places where the pointer has no place
   outside.) If the page did scroll up and then does
   not scroll down, up to 8 still steps are allowed (an overscrolled view takes them) before
   the capture ends `lost_track`.
8. **Recovery.** A step that does not match is retried after a fresh settle, and the
   fresh-frame check of rule 6 is made again after that retry. If it still
   does not match or is ambiguous, the loop scrolls back by the same count, re-finds the
   last stitched frame (twice more without a hint if it is not there), halves the step
   cap and goes on. Only a step that appended rows resets the count of failed one-notch
   steps. A step that found its way back to rows already in the image does not: where one
   wheel step moves the picture by more than a viewport and back (a page that lays itself
   out anew under the viewport), the loop would otherwise go round until the time limit,
   and now ends `lost_track` with what was proved. The cap doubles again after three verified steps. If one notch fails
   twice, or the last stitched frame cannot be found, the capture stops with the exact
   image so far.
9. **Stops.** End of content, height cap, Stop, Cancel, 180 s, injector refusal, source
   lost, frame size changed, modifier keys held, a pause of 10 minutes. Stop at any point
   keeps the image; Stop on the way to the top (the bar's button works there too) keeps the
   viewport on screen at that moment (one frame, confirmed by a second look), with
   `topReached: false`.

## The user's mouse

The pointer is the user's the moment they touch it
([#1514](https://github.com/patrick-hudson/put.gd-desktop/issues/1514)). Before, a person
could not reach Stop: the next wheel step pulled the pointer back.

**Where the pointer rests.** Between wheel steps the pointer rests on a surface the helper
owns, outside the rectangle, so that the helper gets the user's motion from the first pixel
as ordinary pointer events (enter, leave, move). That is the bar when it is beside the
rectangle: a point over its state text, never a button, at least 14 px inside the bar so that
a hand's first motion event lands on the bar, and as near the rectangle as that allows, so
the way to the page stays short (Plasma's "shake cursor" effect fires on a pointer that
travels more than about 100 px back and forth several times a second; where that point is
more than 60 px from the rectangle the pad is used instead, outside GNOME). Where
the frames show a painted pointer (Hyprland, labwc), the arrow hangs clear of the rectangle.
Without the bar beside it, a clear 32 px pad of the helper's own sits at the old rest spot,
16 px outside the rectangle (an X11 window, or a layer surface on Plasma, Hyprland and
labwc). GNOME has no pad, because only the Shell places windows there: the pointer rests on
the bar, whose place the resident passes on.

**Who moved it.** The helper knows where and when it last put the pointer. The rest surface
reporting the pointer anywhere else (more than 4 px from that point), or the pointer leaving
it when no visit into the rectangle was sent, is the user. So is, on X11, a pointer that is
not where XTest last put it (`XQueryPointer` before every injection). On Plasma, Hyprland and
labwc, where nothing can ask where the pointer is, a clear layer surface (the watch) covers the
rectangle's output and takes the pointer everywhere except 8 px holes at the points where the
wheel is turned, the bar and the pad: the capture's own pointer is only ever at its rest or in a
hole, so any pointer event on the watch is the user's, also while the pointer is in the
rectangle for a wheel step. The watch is hidden while paused, so the desktop is the user's then.
The user's motion is watched from the start of the capture: before the pointer first arrives at
its rest, a pointer that moves on any of these surfaces while the capture sent nothing in between
is the user.

**A pointer pulled back from under the hand.** Where the desktop cannot be asked where the
pointer is (Wayland), a hand can start to move while the pointer is in the rectangle for a
wheel step, and the capture can move the pointer before it has heard of the hand (an Omarchy
guest delivered the hand's events 100 to 200 ms late). The gate closes the moment the hand is
seen, before anything else. Whether the capture moved the pointer after the hand did is told by
the motion event's own time, the compositor's monotonic milliseconds, against when the capture
last sent a motion (used only where the two clocks agree within a second). An enter has no time
of its own and a leave neither a time nor a place: they pause, and the next motion event
decides. Without a watch (GNOME), whose rest surface sees the hand only after the move back, a
motion within 12 ms of the pointer's arrival at rest was under way when it arrived. The pointer
is then put back once by a relative motion of exactly the displacement the capture caused: from
where it put the pointer to where the hand had it (the last event from before that motion,
waiting at most 400 ms for one from after it; on GNOME, the point the wheel was turned at). A
motion that was already past the gate when the hand was seen is undone with it. A relative
motion keeps whatever the hand did since, however late its events come; KWin's fake input, the
wlroots virtual pointer and Mutter's remote desktop do not accelerate it. The capture pauses.
The helper's log says `pointer put back by`. X11 asks the server before every motion and needs
none of this.

**Pause.** On the user's motion the injection gate closes at once, from the event itself,
before the loop has looked. The loop stops between any two injections, inside a visit and
inside a wait, sends nothing more (no pointer motion, no wheel step), reads no frame, and
keeps its state and the image so far. The pointer stays where the user put it. The bar says
"Paused: you moved the mouse" next to **Resume**, **Stop** and **Cancel**; a pause asked
through `scroll.pause` says "Paused". `scroll.inspect` and the `progress` event carry
`paused: true` and `pausedBy: "pointer"` or `"request"`.

**Resume** (the bar's button, once the mouse has been still for half a second, or
`scroll.resume`) looks at the screen before it goes on. The pointer goes back to its rest,
what it did to the page gets its time to go, and the picture at rest is placed against the
last stitched frame by its pixels alone, never by where the wheel would have taken the page.
Where the page is, or a frame that can be placed further down or up, the capture goes on
from there. Where it cannot be placed and the pause came after one plain wheel step, that
step is scrolled back and the last stitched frame looked for where it was. Otherwise the
capture ends `lost_track`, with the exact image so far and the note "the page moved while
the capture was paused and its place could not be found again". A pause before the first
frame was stitched goes back up to the top. The next step after a pause teaches nothing
about the distance per notch, and an end that was pending is proved again.

**Stop and Cancel while paused.** Stop keeps the image (on the way to the top, the
viewport); Cancel discards it. Esc on X11 and the hotkey again stop it, as at any time.

**Time while paused.** A pause is not counted in the 180 s of the capture. A pause that lasts
10 minutes (`RunOptions::pauseLimitMs`) ends the capture as Stop, with the image kept and the
note "the capture was paused for 10 minutes and ended there", so a capture nobody comes back
to does not hold the session. The resident has no deadline of its own for a running capture,
only for its commands, and those answer at once while paused.

**At the end on X11** the pointer goes back where the user had it before the capture, unless
the user has taken it: it then stays where they put it.

## What counts as proved

A wrong stitch is a valid-looking image, so the stitcher never guesses silently. Each row of
a frame is hashed in 8 column bands, leaving out 4% of the width at each side (scrollbars,
shadows). A second hash of where the edges are lets a row match through a hover tint.

- **Evidence rows.** Flat rows are never evidence: a blank row matches any blank row. A
  candidate offset is scored by the rows with evidence in the overlap: at least 24 of them,
  a score of at least 0.90, and a lead over every rival that is not an adjacent offset.
- **Proof, not consistency.** The matched rows must contain at least 24 different rows (24
  rows of one stripe pattern match any other 24), and the overlap must not be a sliver
  (at least 36 rows and an eighth of the band). The loop never plans a step that keeps less than
  a quarter of the band, so a candidate with a tiny overlap at the far end of the search range is
  what a jump past the whole viewport leaves by coincidence. Rows were once lost this way.
- **A step seen halfway.** After a step that moved less than the hint asked (a short step: the
  last, partial one at the end of the content, or one seen before the page had finished it, whose
  rest comes with the next step), the next step's hint is less sure by what that step lacked, so
  the hint does not pick among tied offsets, and that next step does not teach the distance per
  notch (it moves more than it asks). Without this a table between tables that repeat went
  missing (60 px taken for 200 asked, then 340 for 196), and Chromium on Plasma learned 309 px a
  notch where a notch is 180.
- **Near-periodic content.** Lines of text that differ in a few characters tie under the tolerant
  rule. One candidate wins when nearly all its informative rows are equal bit for bit and
  every rival is mostly not. Content that repeats exactly stays ambiguous. Among tied candidates the loop's hint decides
  only within the wheel's own spread plus 8% of the hint (`tieHintTolerance`); a candidate
  farther off leaves the step ambiguous and the loop goes back. A table that repeats every
  505 px and differs in a heading and a few numbers had a candidate one table less (419 px)
  within a quarter of the hint, and a table went missing; `hintTolerance` (a quarter) stays
  for a candidate that is not proved.
- **Side bars: the search by quarter bands.** A page between two side bars that stay or
  stick has four to six of its eight bands that do not move with the page, and the tolerant
  rule forgives two. Where the search over the eight bands finds no offset, cannot decide,
  finds "changed in place", or finds an offset that two quarter bands contradict (they show
  something in an eighth of the rows and mostly not what they showed), a search by quarter
  bands (`columnSearch`) decides which columns are the page's. The page is what moved
  nearest the pointer, working outward from the quarter the wheel was turned over: as one
  piece by a distance, or, where lines are too short to prove a distance, by the one
  distance at which seven in ten of the rows a quarter shows are equal bit for bit
  (`columnMovedFraction`). A quarter that shows something and stayed where it was ends the
  search, however well it fits another distance (a column of tens digits fits one line up
  and one line down); columns that stayed or moved part of the way are left out, and the
  rows are compared over the rest with the usual score, evidence, different rows, sliver
  and rival rules. A quarter of a row is the same when it is equal bit for bit or equally
  bright to within 6 of 765 (`columnLightTolerance`), because a browser draws the same text
  twice with different antialiasing in a block that scrolls sideways; rows equal bit for bit
  must still be there. Where something moved near the pointer and no offset is proved,
  "changed in place" becomes ambiguous, so the loop goes back and the capture ends honestly
  instead of losing the rows between. An offset the eight bands proved stands unless the
  quarters prove another.
- **Margins.** The side margins decide only when the compared columns cannot tell candidates
  apart (line numbers of a wide editor sit inside them). A margin proves an offset only if its rows moved, not if
  they stayed where they were, as a fixed side bar or a scrollbar does.
- **Sticky header and footer.** Rows that stay put while the middle moves are found from the
  first moving step, emitted once, and left out of the search; the estimate only shrinks. A
  header that comes late is adopted as well: a run of rows at the top that newly stays put
  after scrolling has started (a bar that shows once the title has scrolled away) is taken
  for a header when it shows something (at least `layoutStaticMinRows` rows that are not one
  flat colour) and the rows that should have scrolled into it contradict it, by the rule
  of the bar at the bottom. The frame in which it first shows is matched in its interior,
  the moving band without its first quarter (`edgeTopFraction`). The bar hides rows that are
  in the image already and appended rows come from the bottom, so no row is lost, and the
  late header is in none of the image's rows.
- **Rows near the bottom edge wait (the edge guard).** The last 8% of a frame's moving band
  (at least 24 rows, at most a quarter of the band) is not written out when its frame is
  stitched (`edgeGuardFraction`, `edgeGuardMinRows`). What lies over the edge of a frame is
  not the page: a browser's link-status panel, a window border blended into the last row (the
  viewport a browser reports is one or two rows taller than what the desktop shows of it), a
  floating button. The next frame shows the same rows higher up, away from the edge, and
  they are taken from there; only rows that the next frame no longer shows in its interior
  are written as they were first seen. That fits "rows are never revised" in one way: a row
  is written once, but not necessarily when its frame is stitched. `outputHeight()` counts
  the rows that wait, and at the end they are written as they were seen, so a panel or border
  at the bottom edge is in the image's last rows only, where it is on the screen. Where the
  pointer rests in the rectangle (it has no place outside) rows above its margin are not
  taken in place of waiting rows (`ScrollStitcher::setTrustedFrom`).
- **Outcomes.** A step is appended, no movement, reversed (an elastic bounce, tracking kept),
  ambiguous, no match or a layout change; ambiguous and no match never touch the output or the
  reference. Without a proof, a candidate is taken only when the loop's hint (notches times pixels
  per notch) says the same, and counted as low confidence in the stats.

### The layout rule

The frames keep their size when the page's viewport inside them changes: a window resized
under the rectangle shows the window's background where the page was, or a bar pins itself to
the bottom. The tolerant row rule lets two of the eight bands differ, so without more rows of
both layouts were stitched into one image that ended `end_of_content`. Three signals, each about
a change in the history of a step, not about how a frame looks, end the capture:

- **Columns.** A band of columns that was steady in every earlier proved step (nearly every
  row of the overlap was either where the offset says or where it was) and that is now neither
  in at least nine rows of ten of one step was repainted as a whole. A band that never was
  steady (a minimap, a scrollbar thumb wider than the side margin) stays with the tolerant rule,
  and so does anything that touches a few rows: a hover tint, a caret, a pointer. A band
  with a quarter that moved as one piece by a distance of its own is left to the pace rule
  below: a side bar that sticks is, in the one step in which it sticks, neither moved nor
  stayed.
- **Rows.** A run of rows at the bottom that newly stays where it is, beyond the footer
  estimate, is a bar over the page when the rows that should have scrolled out of it contradict
  it: at least 4 of them, and more than three to one against the rows that did come out of it.
  Blank rows that happen to be alike do not contradict. The rule judges rows the way the
  offset was found, over the quarters the column search kept.
- **A pace of its own.** A column that scrolls with the page at another rate (a code
  editor's minimap at a third of it, a parallax strip) passes the tolerant rule like a side
  bar that stays, since it covers two bands at most, but its rows in the image would be
  neither the page's nor a fixed bar's: no tall image can show it, because by the time the
  page has moved a viewport the strip's row that belongs beside a page row has left the
  screen. It is proved per quarter of a band (`ScrollStitcher::paceOf`): at least nine rows
  in ten of the overlap equal bit for bit at one distance that is neither the page's nor
  none, 24 different rows among them, and fewer than half at the page's distance and where
  they were (`ownPaceMaxWith`), and in two steps in a row (a side bar that sticks or comes
  unstuck moves part of the way in one step only). The step is then
  `StepResult::ownPace`: `LayoutChanged` with its own note, "a part of the selection scrolls
  at a pace of its own (a minimap, a column with a parallax effect), so no single image shows
  it beside the page; the image ends with the rows from before it moved", and the image is
  cut back to the rows from before the first of the two steps. Something that changes
  without moving (a video, an advertisement that rotates) has no such distance and stays with
  the tolerant rule.

Either way (and for a pace of its own) the step is `StepStatus::LayoutChanged`, no later
frame is taken, and `ScrollRun` ends `lost_track` with the note "the layout inside the
selection changed during the capture; the image ends with the rows from before the change"
(the pace rule's note is above). The image is the exact part proved
before the change, never `end_of_content`. `display_changed` stays the frame-size case only
(`RunResult::LayoutChanged`, from `StepStatus::InvalidFrame`): the frames kept their size here,
so the display did not change.

- **Rows can be taken back.** The stitcher no longer only appends. A step in which a bar first
  shows cannot tell it from page, and its rows are already out, so rows count as proved only up
  to the last step that showed no such run, and the image is cut back to there. The stitcher
  calls its `RowTakeBack`; the helper truncates its spool and its row count. The pace rule and
  the late banner below use it too. A sink without a take-back keeps the rows, and the
  stitcher then does not cut back.
- **A bar from the first scroll on is the footer.** The first frame is taken before any wheel
  step, so it lacks a bar that every later frame has. That bar becomes the footer, the rows the
  first step took from under it go back, and the capture completes. A bar that appears after
  rows of frames without it are in the image ends the capture, except a banner that slides
  in over the bottom (below).
- **A banner that slides in at the bottom becomes the footer.** The helper sets
  `StitchOptions::lateBarIsFooter`. A page's own banner that comes up over the bottom after
  the reader has scrolled a while (an appeal for donations, a cookie notice) would end the
  capture under the rule above, because the step that first showed it appended its rows as
  page. It becomes the footer instead when every row can be accounted for: it shows
  something (at least 4 rows that are not one flat colour; the window background under a
  viewport that got shorter is one flat colour and stays a change of layout), it came up in
  the last frame and no earlier, it is no taller than that step was long, and this frame
  shows the page's rows above it. The rows taken from under it are taken back and this
  frame supplies them. A bar that shows nothing, came up over more than one frame, or is
  taller than the last step still ends the capture. A band that appears at the top is
  not judged: it hides rows already in the image, appended rows come from the bottom, and the
  search decides as before.
- **A change on the way to the top is not a change.** No row is stitched while the page goes
  up, so nothing of an earlier layout is in the image: it shows the layout on screen from the
  first stitched frame on, and ends `end_of_content` if the rest is proved.
- **Limits.** A change confined to the 4% side margins is not seen. A scrollbar wider than the
  margin whose thumb appears over most of the overlap in one step ends the capture `lost_track`.
  A change while the page stands still can cost the rows of the last step (cut back further
  than needed; the image is still an exact prefix). A popup that covers most of the overlap in
  a steady band ends the capture `lost_track`.

The thresholds are `StitchOptions` (`src/core/ScrollStitcher.h`):

| Option | Default | Meaning |
| --- | --- | --- |
| `layoutSteadyFraction` | 0.10 | A band is steady while at most this fraction of the overlap's rows is neither moved nor stayed |
| `layoutBrokenFraction` | 0.90 | A steady band with at least this fraction of such rows in one step was repainted |
| `layoutStaticMinRows` | 4 | Rows that contradict a new static run at the bottom before it is called a bar |
| `layoutStaticRatio` | 3 | The contradicting rows must outnumber the rows that did come out of the run by more than this to one |
| `tieHintTolerance` | 0.08 | Among tied candidates the hint decides only within the wheel's spread plus this fraction of the hint |
| `edgeGuardFraction` | 0.08 | The share of the moving band at its end that waits for the next frame (0 turns the guard off) |
| `edgeGuardMinRows` | 24 | At least this many rows wait (at most a quarter of the band) |
| `edgeTopFraction` | 0.25 | The first share of the moving band left out when a header that just came up is matched, and for rows that wait |
| `ownPaceMaxWith` | 0.50 | A quarter has a pace of its own only when fewer than this share of its rows are at the page's distance and where they were; also the share below which a quarter counts as changed in the column search |
| `columnLightTolerance` | 96 | Two quarters of a row are the same line drawn again when as bright within this many sixteenths of the channels' sum (6 of 765) |
| `columnMovedFraction` | 0.70 | A quarter of short lines names a distance when this share of the rows it shows are equal bit for bit there |
| `lateBarIsFooter` | false | A banner that comes up at the bottom late becomes the footer (the helper sets it) |

When the step cannot be proved or recovered the capture ends `lost_track` with the exact
image so far, not an error.

## Output and limits

Rows stream to a private spool file under `<data>/scroll/<id>/` (0700 directory, 0600
file) in final order, so the stitcher holds no image. At the end the spool is mapped,
wrapped as a `QImage` and written as a PNG within a bound, and the spool is deleted. Abandoned
staging is swept at start, as for recordings. Finished files survive helper exit until the
resident has admitted them.

- **Height cap.** The image never exceeds the cap. With a sticky footer the rows reserved
  for it are counted in the cap, and when the cap cannot hold the footer the first
  viewport's prefix is kept (`height_cap`). The cap is the smallest of `preferences.scrollCaptureMaxHeight` (2,000 to 60,000, default
  30,000), `limits.imagePixels / width` and `limits.imageBytes / (4 × width)`; for the editor route also
  `EditorJob::admissionPixels / width` (24 MP, 15,000 px at 1,600 wide). The helper reports the
  height it applied as `appliedMaxHeight`.
- **Region.** On one output and at least 64 px each way (`scroll_region`).
- **Admission.** The resident checks the file (inside the session's directory, regular, owned,
  the declared size and SHA-256, the PNG header and IHDR size, bounds) and re-enters the
  normal path as a `png` clipboard payload; through the editor as a one-file payload. A file that fails is
  `scroll_output_invalid`.
- **Stop and Cancel.** Stop keeps the image. A stop before the first frame was stitched is a
  cancel. Cancel discards the files and leaves no History row. The helper answers a Cancel
  only when its worker has ended, so the resident remembers a Cancel that is on its way and it
  decides the end: other commands are answered `scroll_busy`; a pending Stop is answered
  `cancelled` (the helper too answers a Stop behind such a Cancel `cancelled`, not with a state
  error); a `finished` event is ignored and the image discarded. A Stop whose image was
  admitted before the Cancel arrived has ended the session, and that Cancel gets
  `scroll_not_found`. Closing the control pipe, or the
  helper leaving, ends injection at once and removes the files of an unfinished capture.

## End reasons

`finished` carries `endReason` and `truncated`, and `topReached: false` when the
scroll-to-top step was asked for and did not prove the top; `scroll.inspect` and the MCP
result report them so an agent can tell a complete capture from a partial one.

| `endReason` | `truncated` | Meaning |
| --- | --- | --- |
| `end_of_content` | false | The content stopped moving, proved by a fresh frame, and with scroll-to-top on the top was proved too: the whole scroll area is in the image, top to bottom. (With scroll-to-top off it is everything from where the content was scrolled to.) |
| `top_not_reached` | true | The capture reached the end of the content, but the scroll-to-top step had not proved the top, so the image starts partway down. It is kept and routed, a warning notification "put.gd — scrolling capture incomplete" says "Could not reach the top of the page. The image starts partway down.", and History carries the same note. `topReached` is false and `result.note` says why the way up ended. The rows it has are exact |
| `height_cap` | true | The cap was reached first. The image is kept and routed, a warning notification says "Stopped at 30,000 px. The page continues below.", and History carries the same note. A warning, not a failure: the rows so far are exact |
| `stopped` | true | The user stopped it (the bar, the hotkey again, Esc on X11, `scroll.stop`), or a pause lasted 10 minutes (`result.note` says so) |
| `lost_track` | true | A step could not be matched or recovered, the end could not be proved, the [layout inside the selection changed](#the-layout-rule), or a column of the selection scrolls at a pace of its own. The image so far is exact and `result.note` says why |
| `time_limit` | true | The 180 s limit ended it. The image so far is exact |

A note (`height_cap`, `top_not_reached`) is extra information on the History row. When the upload or the local save failed, or the operation was cancelled or ended partial, the row's status shows that first and the note follows after a `·`.

`topReached: false` goes with any of them except `end_of_content`: a capture that ended by
Stop, the height cap, `lost_track` or the time limit after an unproved top keeps its own
end reason and carries the flag, with no extra warning. The field is absent when the top
was proved and when scroll-to-top is off. The resident refuses a helper result that says
`end_of_content` with `topReached: false`, or `top_not_reached` without it
(`scroll_output_invalid`).

The resident releases the helper as soon as it has validated the PNG, before the image
leaves the upload queue, so a new capture can start while the previous one still waits to
upload.

A capture that keeps nothing fails instead: `scroll_no_movement` (nothing under the
selection scrolled at any park position), `capture_timeout` (no frame in 3 s, or modifier
keys held), `display_changed` (the frame size changed during the capture, compared with the first
frame; the run result is `RunResult::LayoutChanged`; a layout change with the frame size kept is `lost_track`, see [the layout rule](#the-layout-rule)), `scroll_unavailable` with
`injection_unavailable` or `frame_source_unavailable` (the desktop refused the input or the
stream ended), `encoding_failed` or `input_excessive` (the spool or the PNG). The codes
and what to do are in [errors](../user/errors.md#scrolling-capture).

## Known limits

- **Lazy loading and video** change content in place and stop the capture early with an exact
  partial image.
- **Strictly periodic content** can only be crossed on the measured step size: exact when the
  application scrolls a constant distance and a few pixels off when it does not. A blank
  region filling the viewport, with no scrollbar in the selection, looks like the end. Include the scrollbar.
- **Foreign overlays are stitched in.** A notification of another application over the
  rectangle is stitched where it covers at most two of the eight bands, and ends the capture as
  `lost_track` where it covers more, or where it covers nine tenths of the overlap's rows in a
  band that was steady before (see [the layout rule](#the-layout-rule)). put.gd's own notices are handled; the Omarchy
  dismissal is by summary, so a card of another application with the same summary goes with them.
  Fixed parts of a page (a narrow side bar, a floating button) look the same to the stitcher,
  so no rule rejects them.
- **Hover effects** of the pointer, a tooltip, and a browser's link-status bubble can end up
  in the image. The pointer waits outside the rectangle and visits for the wheel step only, and
  the edge guard keeps what lies over the bottom edge out of the rows written in place, so a
  panel is in the image's last rows only, where it is on the screen. With no place outside
  the rectangle the pointer rests on the page and a hover effect can still be in it. The step
  cap keeps the pointer's own row out of what is appended.
- **Hyprland paints the pointer into the frames,** whatever was asked. The loop keeps it out of the
  rectangle wherever a frame may become the image. A Stop in the 60 ms between the pointer's entry and its
  wheel step sends one pointer motion (out of the rectangle) after Stop.
- **The user's mouse.** On GNOME a motion that comes while the pointer is inside the
  rectangle for a wheel step (about 100 ms of every step) is seen only when the pointer is
  back on its rest. The pointer is then put back once by the way from its rest to the point
  the wheel was turned at, which can be off by the hand's way during the visit (at most
  about 60 px at a brisk 600 px/s). GNOME has no watch and no pad, because only the Shell
  places windows there. On the layer-shell desktops the watch sees where the hand was, and
  X11 asks the server.
  A region that covers its whole output has no place outside it: the pointer stays parked on
  the page, no bar shows, and moving the mouse does not pause the capture (Esc on X11, the
  hotkey and `scroll.stop` still stop it). The same is true on GNOME when the Shell found no
  spot for the bar. The bar needs about 630 by 52 px beside the rectangle; a region that
  leaves less on every side of its screen puts the bar on another screen, or hides it with
  one screen, and the pointer then rests on the pad.
- **A build with no region selector has no scroll capture, for any caller.** The Ubuntu
  24.04 DEB on KDE Wayland is built without LayerShellQt for Qt 6 (`cmake/Platform.cmake`),
  so it has no layer-shell selector and no layer-shell surfaces for the ring, the bar and the
  heartbeat. They would be ordinary windows that the compositor places, inside the rectangle
  among other places, and a capture of a named rectangle was admitted there with the panel
  and the helper's own windows in it. The probe reports `selection: none`, `available:
  false`, reason `region_unavailable`; the helper's `prepare` refuses on it, and the
  resident refuses `scroll.prepare` with or without `rect` as `scroll_unavailable`, reason
  `region_unavailable`, before an operation is created; the window's button reports the
  same. A rectangle named through `putgd-cli` or MCP does not help. The AppImage, which
  bundles the selector, works.
- **GNOME: an extension loaded before version 22** has no `scroll` hotkey
  (`helper_outdated` until the next login) and no beat. Scroll capture needs version 22 or later
  (`selectsRegions`): with an older loaded extension every start, tray, window, CLI and MCP
  included, gets the region error (`helper_outdated` while the packaged extension is newer, else
  `region_unavailable`) until the next login.
- **Live web pages**: each of these ends the capture
  early with an honest reason and an exact partial image, and none puts a wrong row in one.
  - A page that lays itself out anew under the viewport while it is scrolled (Chromium on a
    long pull request page lays its sections out as they come into view): one wheel step can
    leave the picture where it was because the section above grew by as much, no offset
    explains the frame, and the capture ends `lost_track` after two tries.
  - A banner that comes up inside a step's own frame and covers more than a tenth of the
    overlap (a bar pinned to the bottom that is there in the step's first frame but not in
    the reference frame): no offset passes, and the late-banner rule judges only a step
    whose offset was found, so the capture ends `lost_track`. A banner that stands in two
    frames running becomes the footer.
  - A step in which the page moves a different distance than its text is redrawn at (the
    browser reports a scroll of 138 px while the column moves 135 px and draws its text anew
    at another sub-pixel place): no whole number of rows makes the two frames' rows equal,
    and equal rows are the proof, so the capture ends `lost_track`.
  - A slow way up when the point where the pointer comes in sits on a link: the browser
    shows its link panel for each visit, the picture is not at rest until the panel has
    gone, and the way up can take a minute or more (on Plasma, 40 to 125 s for such a page).
    It can run into its 90 s limit and end `top_not_reached` for an image that is whole.
  - Side bars that the page pins beside every row are in the image beside every row, as on
    the screen; a browser's own full-page picture shows them once.
  - A bar the page pins to the bottom of the viewport is in the image once, at its end, over
    the page's last rows, when it is there as the capture ends (a bar that appears only at the
    end is such a bar).
  - A page that paints the first notch of a step at once and then works through a long backlog
    before it paints the rest could still be taken for its end. The end proof does not rule that
    case out.
  A page taller than the cap ends `height_cap`.
- **Only vertical scrolling** exists. Horizontal scrolling, a manual-scroll mode, scrolling by key
  presses, window and monitor targets and regions that span outputs are out of scope.

## Agent control

Every step is a session method: `scroll.capabilities`, `scroll.prepare`, `scroll.select`,
`scroll.start`, `scroll.pause`, `scroll.resume`, `scroll.stop`, `scroll.cancel` and
`scroll.inspect`, with the MCP tools
`prepare_scroll_capture`, `control_scroll_capture` and `get_scroll_capture`. See
[CLI](../automation/cli.md#scrolling-capture) and [MCP](../automation/mcp.md#tools-and-effects).
`scroll.prepare` never starts by itself: select (or pass `rect`), then `scroll.start`.

## Verification

The suites and what they prove are in [Testing](../testing/testing.md#scrolling-capture-suites);
the acceptance rows are in the [acceptance matrix](../testing/acceptance-matrix.md#scrolling-capture-rows).
