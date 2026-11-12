# Screen recording: Wayland

Part of [Screen recording](../recording.md).

## Wayland acquisition

On most Wayland sessions the compositor provides the pixels as a PipeWire video
stream and `pipewiresrc` feeds them through `videorate` at the configured
rate into the same encoder, pause and budget logic as X11. Compositors with no
screencast service (Budgie on labwc, Magpie) give the frames through
`ext-image-copy-capture-v1` into `appsrc` instead, with no PipeWire stream. The probe picks one of four ways and
reports it as `acquisition`:

- `kde-screencast`: KWin's own `zkde_screencast_unstable_v1` protocol
  (version 3 or later) on a Plasma session. `kdeScreencast {available,
  version, outputs:[{name, logical}]}` carries the facts. A monitor or
  `desktop` target streams the named output (the first output when the
  identity is empty) and a `region` with a rectangle streams that logical
  rectangle (`stream_region`) with an odd width or height rounded down to
  even, so no crop is needed. KWin only
  advertises the protocol to executables whose application desktop entry
  lists it in `X-KDE-Wayland-Interfaces`; packaging installs
  `putgd-record.desktop` with `Exec=<bindir>/putgd-record` for that, the
  same mechanism `docs/desktops/kde.md` describes for ScreenShot2. Without
  the entry the facts say `kde_screencast_unavailable` and the portal is used.
- `mutter-screencast`: the `org.gnome.Mutter.ScreenCast` service on a GNOME
  session (`mutterScreenCast {available, service}`). A monitor or `desktop`
  target calls `RecordMonitor` with the Qt screen's connector name (the
  primary screen when the identity is empty), a `region` with a rectangle
  calls `RecordArea` with the same even-size rounding;
  `PipeWireStreamAdded` delivers the node. The service
  answers session clients on the GNOME desktops tested; where it refuses,
  the error is reported and the portal remains the way to share.
- `portal`: `org.freedesktop.portal.ScreenCast` (version 2 or later) on the
  session bus, used for everything when neither native path is available
  (never on Budgie or Hyprland, whose route is `wlr-capture` below, even when that
  route is unavailable), and for a `window` target when the resident could not resolve the window (see
  [Window recordings](#window-recordings)). `portal {portalVersion, sourceTypes,
  cursorModes}` carries the portal's facts and `targets` lists `window` only
  when the portal offers it. `prepare` opens a portal session
  (`CreateSession`, `SelectSources`, `Start`) and the compositor shows its
  picker, which is why the `prepare` deadline is long. With portal version 4
  or later the share is requested with `persist_mode 2` and the returned
  restore token is kept in `<XDG_DATA_HOME>/putgd/putgd-record/recording-portal-tokens.json`
  keyed by desktop and target kind, so the next recording of that kind
  restores the share without a picker until the compositor invalidates the
  token (a refused token simply shows the picker again and stores the new
  one). A `region` records the shared monitor cropped to its rectangle
  (`videocrop`, stream-relative), which is chosen before the portal session
  opens (see below).

- `wlr-capture`: on a Budgie or Hyprland session (by the desktop token, as the
  adapter routes; never on bare labwc), always, and the ScreenCast portal is
  never used there. A rotated or flipped output's frames are turned upright before
  `appsrc`, and an odd window or output size is cropped to the even canvas. `putgd-record` opens its own Wayland connection,
  creates an `ext-image-copy-capture` session for one output or one window
  with `paint_cursors`, and a capture thread pushes each frame from a ring of
  four reused `wl_shm` buffers into `appsrc` (format from the compositor's shm
  format, `leaky-type=downstream`). A frame arrives only when the source
  changed; on a still screen the last frame is resent once a second, as
  pipewiresrc's keepalive does, so the timeline keeps advancing. Each buffer
  carries the frame's `presentation_time` (CLOCK_MONOTONIC) and takes the same
  stamping and cadence lock as a PipeWire frame; the stream buffer goes back to
  the ring when that probe copies it. `wlrCapture {available, windows, outputs,
  protocols, reason?, windowReason?, elements}` carries the facts; a missing
  global or a failed connection makes `recording.prepare` return
  `recording_unavailable` with `details.reason` `wlr_capture_unavailable` and the
  protocol named in `message`, and a missing `appsrc`, `videoscale` or
  `videoconvert` the same code with `details.reason` `live_source_missing`,
  never a chooser. Monitor streams the named output; `desktop` only with one
  output (with several, `targets` leaves it out and `targetReasons.desktop` says why);
  a `region` is cropped (`videocrop`, native pixels) from the output it lies
  on, and one spanning outputs is `recording_unsupported`. `window` takes an
  identity from `recording.windows` (ext toplevel identifiers, no rect), or
  the window active at `prepare` when empty, resolved by the resident; `targets` lists it only where the toplevel capture source exists
  (labwc 0.20, Mir 2.29). A window that changes size is scaled with borders
  into its starting size (`videoscale`); one that closes ends the capture (the
  helper's `stopped` event, reason `target_closed`; the session's state becomes
  `reviewing`, `stopReason` `target_closed`) and keeps the clip; an output that
  goes away ends it with `source_lost`. Overlays are
  layer surfaces, as on Plasma; a monitor recording includes the bar.
`windowAcquisition` says how a `window` target is recorded here:
`kde-screencast`, `mutter-screencast`, `wlr-capture`, `portal` or `x11`.

Both PipeWire native paths name a node on the session's PipeWire daemon, so the probe
also requires its socket (`pipewire {socket}`; `PIPEWIRE_REMOTE`,
`PIPEWIRE_RUNTIME_DIR` and `XDG_RUNTIME_DIR` are honoured) and leaves the
portal, which passes its own connection as a descriptor, in charge when it
is missing. Monitor and window identities are optional everywhere (on X11 an
empty monitor identity means the first connected monitor). Reasons for an
unavailable Wayland session are `live_source_missing` (no `pipewiresrc`; on
Budgie and Hyprland no `appsrc`, `videoscale` or `videoconvert`),
`portal_unavailable`, `portal_too_old` or `session_bus_unavailable`, and on
Budgie and Hyprland `wlr_capture_unavailable` (a missing Wayland global or a
failed connection, named in the message). `recording.prepare` returns all of
them as `recording_unavailable` with the reason in `details.reason`.
Cancelling a picker ends the operation as `cancelled`. Cancel, discard, Stop,
an automatic stop and a capture failure close the stream (`Session.Close`,
`Session.Stop` or the protocol's `close`), which ends the share. After a stop
the close is sent before review opens, without waiting for the compositor's
reply.
`tests/unit` covers the portal stream
demarshalling; the PipeWire paths are exercised on the Fedora GNOME and Debian
KDE desktops and `wlr-capture` on the Fedora 45 and Ubuntu Budgie 26.04 goldens.

### Window recordings

On Plasma and GNOME Wayland a `window` target never shows the portal's picker
(#1278). Before the helper starts, the resident resolves the window from the
desktop's window list, which `recording.windows` (MCP `list_recording_windows`)
also returns: `{windows: [{identity, title, appId, rect, active, minimized}],
activeExcluded}`. A target without `identity` records the `active` window; one
with an identity records that window whatever has focus. put.gd's own windows
are never listed, so asking for one, or for the active window while put.gd has
focus, fails `recording_target` (`details.reason` `own_window` or
`no_active_window`). The resolved window is frozen for the recording; `inspect`
shows it as `target.identity` and `window {identity, title, appId, rect}`.

- **KDE**: KWin has no D-Bus call that lists windows, so the resident loads a
  one-shot KWin script (`org.kde.kwin.Scripting`) that reads
  `workspace.windowList()` (normal windows and dialogs) and calls the
  resident's own unique bus name back; a call from anyone but the bus owner of
  `org.kde.KWin` is ignored. It takes about 0.1 s. The identity is KWin's
  window UUID, and the helper streams it with `zkde_screencast_unstable_v1`
  `stream_window` and an embedded pointer. Own windows: the resident, any
  process it started (views helper, editor, recording helper) and the
  `putgd-ui` app ID, the window-capture rule.
- **GNOME**: the put.gd Shell extension's recording protocol 2 adds
  `RecordingWindows` (the Alt+Tab list, the focused window `active`) with
  Mutter's window ids, and the helper calls Mutter's `RecordWindow` with
  `window-id` and an embedded cursor. Own windows: the extension's
  `_residentWindow` rule. The extension's `metadata.json` version rose to 19 with
  this protocol (it is 21 now, with protocol 3 for click highlights): an
  older extension still loaded after a package upgrade makes a window
  recording fail `helper_outdated` instead of falling back to the picker, and
  `recording.windows` answers the same. Its message says to log out and back in,
  or gives the setup state's own message when a login can't help.
- If the window list is unavailable for another reason (the extension is off,
  KWin scripting failed), the resident logs it and the portal picker chooses
  the window, as it does where the helper has no native path.

The stream follows the window, not a screen area. Moving the window changes
nothing in the clip. A resize changes the stream: KWin renegotiates its size,
and Mutter draws the window into a larger fixed buffer and names its place in
each frame's PipeWire crop region, which the pipeline applies with a runtime
`videocrop`. Every frame is then scaled, letterboxed, into the canvas taken
from the window's size at `prepare` (at its output's scale), so the encoder
never sees a size change. `pipewiresrc` before 1.5 (Debian 13 ships 1.4.2)
forgets its negotiated caps and fails KWin's renegotiation with `unhandled
format`; the helper drops that source's EOS, reopens a new `pipewiresrc` on the
same node 0.4 s later (at most 30 times per recording) and the clip goes on.

A covered window keeps recording its own live content, and so does a
minimized one: KWin 6.3, 6.6 and 6.8 and Mutter (GNOME 50) all keep rendering
the window for its stream. The pointer is embedded while it is over the window.
KWin 6.6 and later include the server-side title bar in a window stream, KWin
6.3 does not; a client-side decoration is part of the window everywhere. When
the window closes, KWin sends the stream's `closed` event and Mutter its
session's `Closed` signal: a running capture stops into review with
`stopReason: target_closed` and the clip so far (0.12–0.24 s after the window
closed), and a ready session that never recorded fails
`recording_target_closed`. A source that cannot be reopened stops the same way
with `stopReason: source_lost`.

Acceptance on Kubuntu 26.04 (KWin 6.6.6), Debian 13 (KWin 6.3.6), Aurora
(KWin 6.8.2), Fedora 44 (GNOME 50.6) and Ubuntu 26.04 (GNOME 50.1, VM) with
the PR packages: no picker (ready in 0.23–0.70 s), motion through move,
resize, cover and minimize, a clean end on close, the GNOME version gate before
and after re-login, and unchanged monitor and region recordings. The numbers
and frames are in the
[receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/b14f4c69d381d05a0ebd57215a94bccce01b2322/records/2026-11-08-native-window-recording-1278/README.md).

### Timestamps and A/V sync

`pipewiresrc` frames carry the compositor's own frame time in their PipeWire
header (CLOCK_MONOTONIC: Mutter's paint time, KWin's previous output
presentation). As a live `GstBaseSrc` it shifts every one of them by the first
frame's arrival delay and waits on the clock for faster frames, so before
#86 the whole video track was late by whatever the first frame waited (0 to
90 ms on Plasma, different every recording). The helper now uses a private
`pipewiresrc` subclass without those sync times, runs the pipeline on the
monotonic system clock (pulsesrc slaves to it) and stamps each frame when it
reaches the pipeline, as `ximagesrc` stamps a frame when it reads it. Arrival
rather than the header time is deliberate: KWin's header time runs about 40 ms
ahead of the frame, and recordings stamped with it played the sound 35–48 ms
late on Plasma 6.3, while arrival times agree with the X11 path; Mutter's
frames arrive about 8 ms after their header time, so both agree there.

The working clip's audio starts after its first video frame (the sink monitor
opens after the stream: 20–110 ms on Wayland, 0 on X11). Export cuts audio
from the same clip-timeline instants as video and fills any time before the
first sample with silence; before #86 it rebased audio to its own first
sample, which played every Wayland MP4 or WebM that much early (the −53 ms of
the issue). Measured with `--av-marker`/`--av-sync` through the built-in
speakers, the exported files now read −9 to +7 ms (KDE screencast and portal
on Debian 13 Plasma 6.3, Mutter screen cast and portal on Fedora 44 GNOME 50)
against −12 to −18 ms on Kubuntu 26.04 X11, all with GStreamer's `playbin`
as the player. `ffplay` reads 4–43 ms later on the same captures because
it ignores the audio server's latency, and it doesn't play on GNOME Wayland.
The raw numbers per path are in
[the receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-19-wayland-av-sync.md).

Across a pause the offset now holds: after a resume the working clip moves by
at most 0.6 ms and the export by at most 1.7 ms relative to the working clip
(f44 Mutter and d69 KWin, MP4 and WebM), where nightly `134a9be` stepped the
capture by up to 17 ms and moved every export 10 ms earlier
([receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-20-recording-reliability.md); portal paths: [closeout](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-20-recording-closeout.md#166-pauseresume-av-on-the-portal-paths)).

Lab A/B runs: `PUTGD_RECORD_PIPEWIRE_TIMESTAMPS=compositor` maps each frame's
header time instead of stamping its arrival, and `=legacy` restores the
pipeline before #86 (`pipewiresrc do-timestamp=true` on its own stream clock).
With `PUTGD_RECORD_DEBUG_LOG` set, stopping logs the pipeline clock, the
lateness of video and audio at their encoders, the age of the compositor
frame times (min/mean/max), and `frames: N reached the video encoder, M
leaked at the capture queue`.

The stamping probe copies each PipeWire frame out on the source thread, so the
compositor's stream buffers (KWin offers 2–4) return at once instead of
waiting in the capture queue and the converter; a compositor skips a frame
whenever none is free (#6). KWin 6.7's screencast throttle runs only when
the negotiated `max_framerate` is non-zero, with an integer-millisecond
interval stamped after its synchronous readback, which stretched every
interval to about 20 ms (48 records per second, 31 distinct frames in the
clip before PR #981). KWin offers `maxFramerate` as a range whose minimum is
0/1, so the source is followed by `video/x-raw,max-framerate=0/1`; a
compositor whose range starts above zero refuses the link and the pipeline
retries without the filter. KWin then delivers about 66 records per second
holding 60 distinct contents. A frame identical to the previous kept one
(the same presentation recorded again after a cursor move or a second
render) is dropped before the copy, and the rest take cadence-locked stamps:
each new frame takes the slot one period after the previous stamp as long as
it arrived within 0.7 of a period of that slot, an earlier extra is dropped
and a later one resynchronises to its arrival (`lockedFrameTime`). A frame
the recorder made itself (the image-copy source's cursor-only frame, #1364)
takes a slot only at the slot's own time, so it never takes one from the
screen's next change
([microphone and click highlights](microphone-and-clicks.md)). Stamps
follow arrivals on average, so the audio offset #86 measured stays, and sit
on the grid between resyncs; raw arrivals under the same jitter cost five
frames a second in `videorate`. `PUTGD_RECORD_PIPEWIRE_TIMESTAMPS=arrival`
restores raw arrival stamps for lab A/B runs. On aurora-amd (Plasma 6.7.5)
a 60 fps monitor recording holds 59.6–59.9 distinct frames per second.
The #983 recheck on nightly `26.1031.3384` measured 59.72 / 59.81 distinct
frames/s; PR #981 already contains that fix. See the [receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-31-recording-rate/README.md)
for exact hosts, workloads and limitations.

### Wayland selection, bounds and bar

A `region` without a rectangle (the tray, the dashboard's Record screen and
the hotkey all start one) is chosen **before** any compositor stream or
portal session exists, with the selector screenshots already use, so Escape
or a right click leaves nothing behind: the operation ends `cancelled` and no
share was ever opened.

- **KDE Plasma**: the helper itself shows the layer-shell screenshot selector
  (`LayerShellRegionOverlay`, the same class `putgd-region` runs), which reports
  a logical rectangle only after KWin has rendered its surfaces transparent
  and unmapped them; then `stream_region` (or the portal plus `videocrop`)
  starts for that rectangle, even-sized as above. The helper sends `selecting` while it is
  open.
- **GNOME**: clients cannot place surfaces, so the resident asks the put.gd
  Shell extension (recording protocol 1, `SelectArea`) for the rectangle with
  its St selector, then launches the helper with it; `RecordArea` streams
  that area. Without the extension (or with an older one) a rectless region
  is refused with `region_unavailable` instead of recording the whole monitor.
  GNOME Shell runs the extension it loaded at login until the user logs out,
  so after a package upgrade the old one stays loaded. When the loaded
  `extensionVersion` (from `Hello`) is below the packaged `metadata.json`
  version, the resident refuses with `helper_outdated`, "The put.gd GNOME
  Shell extension was updated; log out and back in to load the new version"
  (or the setup state's own message when a login can't help, such as a failed
  update), rather than asking for an extension that is already enabled (#131), and
  `capabilities` reports `helper.outdated` with both versions.
- Elsewhere on Wayland a region needs `target.rect`.

`recording.select {id, rect}` (MCP `select_recording_region`) completes an
open selector of either kind as a drag would; `recording.cancel` closes it.

The **ready ring** (3 px white, dark hairline, just outside the rectangle),
the **dimming** of everything else while recording or paused and the
**control bar** follow the X11 rules where the compositor allows them:

- KDE: the ring and the dim strips are input-transparent overlay layer
  surfaces (split per output). KWin stacks overlay surfaces in the order they
  map and `raise()` doesn't reorder them, so the strips are created clear
  with the ready ring, before the bar, and Record only darkens them; strips
  mapped at Record covered the bar until #127. The bar is an overlay layer surface placed
  by the X11 rules: below the rectangle, else above it, else at the top or
  bottom of another output, else inside its bottom-right corner, where it
  collapses to the handle from Record on, and placed again on its output when
  its size changes. A rectangle's stream contains none of them unless the
  bar had to sit inside. KWin 6.6 and later also leave every
  window of the client that requested a `zkde_screencast` stream out of that
  stream, so on those versions the bar is excluded even from monitor
  recordings (`kdeScreencast.hidesRequester`, from the KWin version on
  D-Bus); KWin 6.3 does not, and the portal path never does.
- GNOME: once the helper reports `overlayPlacement: compositor`, the resident
  calls the extension's `ShowRecordingFrame {rect, kind, state, pid}`: the
  Shell draws the ring and, while recording or paused, dim strips with a hole
  for the bar as non-reactive actors outside the area, and moves the helper's
  bar window (title `put.gd recording controls`, accepted only when its
  process is a direct child of the authenticated resident) below or above
  the area, onto another monitor, or, failing all, inside the corner. The
  extension follows the bar window's size changes and places it again with
  that first decision, so a widened bar stays on its monitor. A bar left
  inside makes the resident report `handle` and send the helper
  `overlay {handle: true}`: from Record on the bar is the dot-and-Stop handle,
  which the extension keeps in the corner.
  `HideRecordingFrame` removes everything when the clip is under review or
  the session ends; disabling the extension, locking or the resident leaving
  the bus does too. Mutter has no per-window screencast exclusion, so a whole
  single-monitor recording on GNOME contains the handle (or nothing, after
  `recording.overlay {visible: false}`).
- A `window` stream (native or portal) carries only that window:
  `overlayExcluded` is true and no ring is drawn.

`overlayPlacement` reports where the bar went: `outside`, `handle` (no room
outside: X11, KDE and the GNOME Shell keep it in the area's corner and it
shrinks to the dot-and-Stop handle while recording, so the handle is in the
clip), `hidden` (KWin 6.6 leaves it out of its own stream) or `compositor`
(nobody could place it; it may be in the clip). Releases before #108 also
reported `inside` for a full bar left in the clip. `overlayExcluded` is true
only for `outside` and `hidden`.
