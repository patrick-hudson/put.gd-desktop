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
the whole scroll area, top to bottom, is in the image.

## Workflow

1. **Start.** The tray's **Capture scrolling region**, the main window's button, the
   `scroll` shortcut (unbound by default), `action.invoke {"action":"scroll"}` or the MCP
   `perform_action`. `editor.open {"source":"scroll"}` does the same and opens the result
   in the [editor](image-editor.md) instead of sending it on.
2. **Select.** The recording region selector opens, live as for a recording. The
   ring sits just outside the rectangle, and a small bar (state, height so far, **Stop**,
   **Cancel**) sits outside it too.
3. **Capture.** It starts by itself after the drag. put.gd parks the pointer in the region,
   scrolls the content to its top first (`preferences.scrollCaptureFromTop`, on by
   default), then sends wheel steps down and stitches until the content stops moving.
4. **End.** At the end of the content, at the height cap, on Stop, or when it
   cannot follow the content any more. Stop keeps the image so far; Cancel discards it.
5. **Result.** One PNG, routed by the `scroll` action's output like a region capture
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
  `selection: none` the probe is still `available`: a request that names `rect` is
  captured, and one without it is refused (see [no region selector](#known-limits)).
- `prepare {rect?, maxHeight, maxPixels, fromTop, autoStart, selection}` then `select
  {rect}`, `start`, `stop`, `cancel` and `overlay {visible}`. Replies come only once the
  helper reached the state. `stop` keeps the image, and its reply is the object a later
  `scroll.inspect` returns. `cancel` is accepted while a `start` or `stop` is pending (it
  is never `scroll_busy`); a `start` or `stop` waiting for the helper then ends with the
  session. Once a `cancel` is on its way it decides the end: see
  [Stop and Cancel](#output-and-limits).
- Events, each with the session id: `selected {rect}`; `progress {state: scrollingToTop |
  capturing, height, steps, pixelsPerNotch}`; `finished {path, bytes, sha256, width,
  height, endReason, truncated, appliedMaxHeight, topReached?, stats}`; `cancelled`;
  `failed {error}`.
  After `finished`, `cancelled` or `failed` the helper injects nothing more for that id.
- Deadlines: 10 s for the helper to start, 15 s per command, 30 s for `stop`, 180 s for
  the capture itself. A missed deadline is `scroll_helper_failed` with the check
  (`start_timeout`, `command_timeout`); a helper that dies, answers out of order or
  floods stderr is the same code (`exited`, `crashed`, `protocol`, `build`). A session client
  waits 15 s past those deadlines, so the resident's own answer arrives (40 s for the
  `scroll.*` methods, 45 s for `scroll.stop`).

On GNOME the Shell extension selects the region and draws the ring, and the resident
places the bar through it (`ScrollShell`, a `RecordingShell` of the scroll session). A
capture with no spot for the bar outside the rectangle hides it for the whole capture. The
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
wheel step the loop puts the pointer back where it parked it, because the wheel goes to
whatever is under the pointer.

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
   step 7 decides whether the region scrolls. Where the frames show a painted pointer
   (Hyprland, labwc) the pointer waits outside the rectangle between the wheel steps up,
   so the viewport that Stop keeps has no arrow in it.
2. **First frame**, with the pointer outside the region on its output and off the bar, so no
   hover effect is in it. With no such place the pointer stays where it is, except where
   the frames show a painted pointer: it then goes to the bottom right corner.
3. **Park** the pointer at the horizontal centre, a third of the way down, and put it
   there again before every injection. After it arrives it is put there twice, 60 ms
   apart: the application has to see it enter first.
4. **Settle by frame cadence.** After a step the picture is settled when no frame in the
   moving band has changed for 120 ms, or for twice the longest recent gap between frames
   (at most 600 ms), whichever is longer, with a hard limit of 1.5 s. The picture also gets
   a response time to begin to change: at least 500 ms, or three times the slowest response
   seen, at most 3 s. A slow guest that delivers a frame every 150 ms would otherwise show a
   mid-animation frame as the settled one.
5. **Plan a step.** The first step is one notch, and its distance is the pixels per notch.
   Later steps aim for 60% of the moving band and never more than 75%, never so far that
   fewer than 36 informative rows of the reference stay in the overlap, never so far that
   the appended rows start less than 24 rows below the parked pointer (48 where the frames
   show a painted pointer), and at most 64 notches.
6. **Prove stillness before the end.** A step with no movement waits at least 400 ms and is
   repeated. Two still steps in a row are the end, and only when a frame grabbed from the
   screen after the last injection agrees with the last one seen. With no such frame
   the end is not proven and the capture stops as `lost_track`.
7. **No movement at all.** If nothing has scrolled down yet, each park position gets two
   single notches (four at the first after a way up), then the next: near the right edge,
   then two thirds down, then `scroll_no_movement`. If the page did scroll up and then does
   not scroll down, up to 8 still steps are allowed (an overscrolled view takes them) before
   the capture ends `lost_track`.
8. **Recovery.** A step that does not match is retried after a fresh settle, and the
   fresh-frame check of rule 6 is made again after that retry. If it still
   does not match or is ambiguous, the loop scrolls back by the same count, re-finds the
   last stitched frame (twice more without a hint if it is not there), halves the step
   cap and goes on. The cap doubles again after three verified steps. If one notch fails
   twice, or the last stitched frame cannot be found, the capture stops with the exact
   image so far.
9. **Stops.** End of content, height cap, Stop, Cancel, 180 s, injector refusal, source
   lost, frame size changed, modifier keys held. Stop at any point keeps the image;
   Stop on the way to the top keeps the viewport on screen at that moment (one
   frame, confirmed by a second look), with `topReached: false`.

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
- **Near-periodic content.** Lines of text that differ in a few characters tie under the tolerant
  rule. One candidate wins when nearly all its informative rows are equal bit for bit and
  every rival is mostly not. Content that repeats exactly stays ambiguous.
- **Margins.** The side margins decide only when the compared columns cannot tell candidates
  apart (line numbers of a wide editor sit inside them). A margin proves an offset only if its rows moved, not if
  they stayed where they were, as a fixed side bar or a scrollbar does.
- **Sticky header and footer.** Rows that stay put while the middle moves are found from the
  first moving step, emitted once, and left out of the search; the estimate only shrinks.
- **Outcomes.** A step is appended, no movement, reversed (an elastic bounce, tracking kept),
  ambiguous, no match or a layout change; ambiguous and no match never touch the output or the
  reference. Without a proof, a candidate is taken only when the loop's hint (notches times pixels
  per notch) says the same, and counted as low confidence in the stats.

### The layout rule

The frames keep their size when the page's viewport inside them changes: a window resized
under the rectangle shows the window's background where the page was, or a bar pins itself to
the bottom. The tolerant row rule lets two of the eight bands differ, so without more rows of
both layouts were stitched into one image that ended `end_of_content`. Two signals, each about
a change in the history of a step, not about how a frame looks, end the capture:

- **Columns.** A band of columns that was steady in every earlier proved step (nearly every
  row of the overlap was either where the offset says or where it was) and that is now neither
  in at least nine rows of ten of one step was repainted as a whole. A band that never was
  steady (a minimap, a scrollbar thumb wider than the side margin) stays with the tolerant rule,
  and so does anything that touches a few rows: a hover tint, a caret, a pointer.
- **Rows.** A run of rows at the bottom that newly stays where it is, beyond the footer
  estimate, is a bar over the page when the rows that should have scrolled out of it contradict
  it: at least 4 of them, and more than three to one against the rows that did come out of it.
  Blank rows that happen to be alike do not contradict.

Either way the step is `StepStatus::LayoutChanged`, no later frame is taken, and `ScrollRun`
ends `lost_track` with the note "the layout inside the selection changed during the capture;
the image ends with the rows from before the change". The image is the exact part proved
before the change, never `end_of_content`. `display_changed` stays the frame-size case only
(`RunResult::LayoutChanged`, from `StepStatus::InvalidFrame`): the frames kept their size here,
so the display did not change.

- **Rows can be taken back.** The stitcher no longer only appends. A step in which a bar first
  shows cannot tell it from page, and its rows are already out, so rows count as proved only up
  to the last step that showed no such run, and the image is cut back to there. The stitcher
  calls its `RowTakeBack`; the helper truncates its spool and its row count. A sink without a
  take-back keeps the rows, and the stitcher then does not cut back.
- **A bar from the first scroll on is the footer.** The first frame is taken before any wheel
  step, so it lacks a bar that every later frame has. That bar becomes the footer, the rows the
  first step took from under it go back, and the capture completes. A bar that appears after
  rows of frames without it are in the image ends the capture. A band that appears at the top is
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
| `stopped` | true | The user stopped it (the bar, the shortcut again, Esc on X11, `scroll.stop`) |
| `lost_track` | true | A step could not be matched or recovered, the end could not be proved, or the [layout inside the selection changed](#the-layout-rule). The image so far is exact and `result.note` says why |
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
- **Hover effects** of the parked pointer, a tooltip, and a browser's link-status bubble at the
  bottom edge can end up in the image. The step cap keeps the pointer's own row out of what is appended.
- **Hyprland paints the pointer into the frames,** whatever was asked. The loop keeps it out of the
  rectangle wherever a frame may become the image. A Stop in the 60 ms between the pointer's entry and its
  wheel step sends one pointer motion (out of the rectangle) after Stop.
- **Ubuntu 24.04 DEB on KDE Wayland has no region selector,** because `cmake/Platform.cmake`
  builds the layer-shell selector only when LayerShellQt for Qt 6 exists. The probe reports
  `selection: none` and is still available, so a rectangle named through `putgd-cli` or MCP is
  captured. The window's button reports `region_unavailable`, and a request with no rectangle
  is refused with `scroll_unavailable`, reason `region_unavailable`, before an operation is
  created. The AppImage, which bundles the selector, works.
- **GNOME: an extension loaded before version 22** has no `scroll` shortcut
  (`helper_outdated` until the next login) and no beat. Scroll capture needs version 22 or later
  (`selectsRegions`): with an older loaded extension every start, tray, window, CLI and MCP
  included, gets the region error (`helper_outdated` while the packaged extension is newer, else
  `region_unavailable`) until the next login.
- **Only vertical scrolling** exists. Horizontal scrolling, a manual-scroll mode, scrolling by key
  presses, window and monitor targets and regions that span outputs are out of scope.

## Agent control

Every step is a session method: `scroll.capabilities`, `scroll.prepare`, `scroll.select`,
`scroll.start`, `scroll.stop`, `scroll.cancel` and `scroll.inspect`, with the MCP tools
`prepare_scroll_capture`, `control_scroll_capture` and `get_scroll_capture`. See
[CLI](../automation/cli.md#scrolling-capture) and [MCP](../automation/mcp.md#tools-and-effects).
`scroll.prepare` never starts by itself: select (or pass `rect`), then `scroll.start`.

## Verification

The suites and what they prove are in [Testing](../testing/testing.md#scrolling-capture-suites);
the acceptance rows are in the [acceptance matrix](../testing/acceptance-matrix.md#scrolling-capture-rows).
