# Screen recording: microphone and click highlights

Part of [Screen recording](../recording.md). Tracking issue
[#1364](https://github.com/patrick-hudson/put.gd-desktop/issues/1364).

## Microphone

A recording can be narrated. The microphone is recorded alone or mixed with the
system audio into the clip's one audio track. MP4 and WebM carry it; GIF and
WebP stay silent.

### Rules

- **Open only while the toggle is on.** The recording bar has a **Microphone**
  toggle beside **System audio**. The input device is opened when the toggle is
  on (for the level meter, before Record), by the recording pipeline from
  Record to Stop, and by nothing else. It is closed when the toggle goes off,
  at Stop, at Cancel, at a failure and when the helper exits. Listing devices
  asks the sound server and opens none.
- **A level meter shows that it is live.** The bar beside the toggle follows
  the input's peak about 12 times a second, before and during the recording.
  In handle mode (a bar that had to stay inside the recorded area) the meter is
  hidden with the rest of the bar.
- **Off by default, remembered.** The toggle's state is
  `preferences.recordingMicrophone`. The tray, the dashboard's **Record
  screen** and the hotkey start with the remembered state; a change on the
  bar is saved. `recording.prepare` is explicit: it records the microphone only
  with `audio.microphone: true`.
- **Never an output's monitor.** A source whose `device.class` is `monitor`, or
  whose name ends in `.monitor`, is never listed and never opened as a
  microphone: it would record the computer's playback a second time. Where the
  system default source is a monitor (WirePlumber picks one when nothing is
  plugged in; three of the lab builders do), the default input counts as
  missing. put.gd does not pick another input in its place.
- **A microphone that goes away does not end the recording.** The clip goes on,
  silent on that input from there; the bar says **Microphone disconnected** and
  `recording.inspect` reports `progress.microphoneLost`. Nothing of another
  input is recorded in its place: with PipeWire the stream is pinned to its
  device and is never moved. Under PulseAudio itself, which may move it, the
  recording looks at the stream's device every fifth buffer (50 ms apart) and
  drops the stream's audio from the first look that finds another device, so
  up to 50 ms of the other input can be in the clip before the cut. See
  [Capture and mix](#capture-and-mix).
- **Without one, the bar says so.** The toggle reads **No microphone**, its box
  is empty and it cannot be ticked; its tooltip and
  `recordingMicrophone.unavailable` give the reason. What the user chose last
  time (`requested`) is kept apart from what is recorded (`effective`), so the
  choice is there again once a microphone is.
- **The bar follows the sound server while it is at Ready.** Open the bar, see
  **No microphone**, plug one in: within about a second the toggle reads
  **Microphone** and can be ticked, without cancelling. One that is pulled is no
  longer offered; one that comes back is recorded again without a second click
  if it was asked for. The helper asks the sound server for its inputs once a
  second from Ready to Record (listing opens no device) and applies the same
  choice as at `prepare`, so an output's monitor is still never a microphone and
  the device is still opened only while the toggle is on. From Record on the
  device is the pipeline's, and its loss is reported as above. Measured with
  a simulated microphone plugged in, switched on, pulled and plugged in again at
  Ready (`record_run.py --mic late`, 26.1110.4480) on Plasma 6.3.6 and on
  Cinnamon X11: `recording.inspect` showed the microphone 0.3 s after it became
  the default input and its loss 0.7 s after, and the clip recorded afterwards
  holds it.

### Device

`audio.microphoneDevice` on `recording.prepare` names a source; without it the
device is `preferences.recordingMicrophoneDevice` (**Settings → Capture →
Screen recording → Microphone**), and without that the system default input.
The name is the sound server's source name, as `recording.microphones` lists
it: at most 256 characters, no control characters.

`recording.microphones {device?}` (MCP `list_recording_microphones`) answers
from a transient `putgd-record --microphones [device]` run, never from a cache,
because inputs come and go while the resident runs:

```json
{"available": true, "device": {"id": "alsa_input.usb-…", "label": "USB microphone"},
 "devices": [{"id": "alsa_input.usb-…", "label": "USB microphone", "default": true}],
 "defaultIsMonitor": false}
```

When nothing can be opened, `available` is false with `reason` and `message`:

| `reason` | Meaning |
| --- | --- |
| `no_microphone` | No input device is connected |
| `default_is_monitor` | No device was chosen and the system default source is an output's monitor |
| `no_default_microphone` | No device was chosen and the sound server names no default source |
| `microphone_missing` | The chosen device is not connected |
| `microphone_is_monitor` | The chosen name is an output's monitor |
| `sound_server_unavailable` | The sound server did not answer |
| `microphone_elements_missing` | This installation's GStreamer lacks an element the microphone needs |
| `microphone_probe_failed` | The helper's query failed or printed something else |

`recording.capabilities` reports `audio.microphone` from the same query at the
moment it is asked (with `audio.microphoneDevice`, or `audio.microphoneReason`
and `audio.microphoneMessage`). A `prepare` that asks for the microphone when
none is available is refused with `recording_unsupported`, `details.field`
`audio.microphone` and `details.reason` from the table, before anything is
accepted. The remembered toggle is not a request: with it on and no microphone,
the tray, dashboard and hotkey still open a recording, the toggle reads
**No microphone** with an empty box and the reason in its tooltip, the
remembered choice stays as it was, and nothing is recorded from an input.

The list comes from GStreamer's PulseAudio device provider
(`pulsedeviceprovider`, started so that it knows the server's default source),
which serves PulseAudio and PipeWire's pulse layer alike.

### Capture and mix

| Sources | Audio branch of the live pipeline |
| --- | --- |
| System audio only | Unchanged: `pulsesrc device=@DEFAULT_MONITOR@ → audioconvert → audioresample → AAC` |
| Microphone only | `pulsesrc device=<input> → audioconvert → audioresample → F32LE 48 kHz stereo → tap → AAC` |
| Both | Each source as above into `audiomixer` (`latency` 150 ms), then AAC |

Both sources stamp their buffers on the pipeline clock, and `audiomixer` places
them by those times, so the two stay where they were captured relative to the
picture. The mixer waits 150 ms for a late branch and then mixes without it. A
*tap* on each branch (a buffer probe behind the fixed raw format) does three
things: it keeps a runaway device clock out of the mixer (`AudioClockGuard`,
the #129 fault, which would otherwise stall it), it feeds the microphone's
level meter, and it drops the microphone's buffers once the microphone is lost.
The mix is a plain sum: system audio keeps its level whether or not the
microphone is on.

Pause, resume and the adaptive frame rate are untouched by the microphone. The
pause probe sits on the AAC encoder's input, after the mix, so one paused
interval still moves both tracks by the same time; the adaptive rate changes
only video frames.

**The clock.** The microphone source never provides the pipeline's clock
(`provide-clock=false`). An audio source offers a clock that counts the samples
it has read, and a pipeline takes it when nothing nearer the sinks offers one.
PipeWire and image-copy pipelines are given the system clock when they are
built; an X11 pipeline is not, and `ximagesrc` has no clock, so a recording
with the microphone alone ran on the microphone's. When that microphone went
away it read no more samples, its clock stopped, `ximagesrc` waited for a frame
time that never came, and the clip ended where the microphone did while the
recording still said it was running: 6.2 s kept of 14 s recorded on Cinnamon
(Mint 22.3, PipeWire 1.0.5). Without the offer an X11 pipeline runs on the
system audio's clock or the system clock, as a recording without a microphone
does, and the microphone follows it; the same run then keeps 14.4 s of 14.2 s.

**Loss.** Three things can happen to an input that goes away, and each ends the
same way. The source element can fail: its error is not the recording's, the
source sends EOS down its own branch, and the mixer and the muxer go on with
what is left. The sound server would move the stream to another source, the
default, which on a desktop without a microphone is an output's monitor: the
recording would go on with the computer's own sound in place of a voice. So
every `pulsesrc` that opens a microphone (the recording's and the ready
state's meter) carries the stream property `node.dont-reconnect`, which is
what `PA_STREAM_DONT_MOVE` becomes in PipeWire's PulseAudio layer: the session
manager then never moves the stream when its device goes. Measured by removing
a simulated microphone under a pinned and an unpinned `pulsesrc`: the unpinned
stream ended up on the default input every time; the pinned one was left
without a source (PipeWire 1.4.2 with WirePlumber 0.5.8, and 1.6.2 with
0.5.13) or was ended with an error (1.0.5 with 0.4.17, and 1.6.9 with 0.5.18).
Both are handled: the source's error is the microphone's loss, not the
recording's; and a stream connected to nothing gives no buffers, where an open
microphone delivers them all the time, silence included, so two seconds without
one counts as lost.
PulseAudio itself ignores the property: there the tap compares the source's
`current-device` with the one it opened at every fifth buffer, twenty times a
second, and drops everything from the first difference; what the moved stream
delivered in the up to 50 ms before that look is in the clip. A
microphone that cannot be opened at Record (unplugged since the session was
prepared) starts the recording without it and reports the same way.

The clip's audio then has no packets after the loss; the export fills the rest
with silence, as it does for a pause's hole.

**System audio when its output goes.** System audio records the default
output's monitor and is not pinned: when that output is unplugged or switched
off, the sound server moves the stream to the new default output's monitor, and
the recording goes on. Measured on X11, where the system-audio source is the
pipeline's clock (Cinnamon 6.6.9, PipeWire 1.0.5 with WirePlumber 0.4.17,
26.1110.4465), with the output removed 6 s into a recording: with another
output to move to, 14.5 s kept of 14.3 s recorded and the sound continuous; with
none (the sound card's profile off, so the server makes its stand-in
`auto_null`), 14.5 s of 14.4 s; and with a microphone as the default input the
stream went to the new output's monitor, never to the microphone (no trace of
the microphone's signal in the clip). The last holds on Plasma 6.3.6 with
WirePlumber 0.5.8 too. `tools/lab/record_run.py --output-gone-at` is the check.

### What a session reports

`recording.inspect` `progress` gains:

| Field | Meaning |
| --- | --- |
| `audioSources {system, microphone}` | What the clip's audio track holds, from Record on |
| `recordingMicrophone {requested, effective, available, device?, unavailable?, lost?}` | The bar's toggle: what the user asked for, what is recorded, the device's label or why there is none |
| `microphoneLost` | Present once the microphone went away: the sentence the bar shows |

While the session is `ready`, `recordingMicrophone` and `config.audio` change
when an input appears or goes, and `recording.audio {id, microphone?, system?}`
(MCP `set_recording_audio`) sets the bar's two toggles as a click does; an
omitted one stays as it is. Asking for the microphone where none can be opened
is refused with `recording_unsupported` and the reason (the helper looks at the
sound server first, so one plugged in that instant counts); from Record on the
call is refused with `recording_state`. A toggle the user ticks is remembered
for the next recording; one set through `recording.audio` is this session's
only.

`config.audio` is `{system, microphone, microphoneDevice?}`. In review only the
output changes: a silent format drops the track, and going back to MP4 or WebM
brings back exactly the sources that were captured. The review window names the
track **System audio**, **Microphone** or **System audio and microphone**.

### Measured

This pull request's CI packages, installed on each desktop, a simulated
microphone (a null sink remapped into a source; no real microphone), driven by
`tools/lab/record_run.py` over its marker (a white flash with a 1 kHz burst
once a second). Audio minus video, the analyzer's median over 13 to 17 flashes;
one frame at 30 fps is 33.3 ms. The number in brackets is the build
(26.1110.*N*).

| Desktop | Microphone alone, MP4 / WebM | Mixed with system audio, across a 3 s pause | Unplugged 6 s in |
| --- | --- | --- | --- |
| Cinnamon 6.6.9, X11 (Mint 22.3, PipeWire 1.0.5) [4437] | 0 ms / −5 ms | +7 ms, step at the pause 0.3 ms | 14.4 s kept of 14.2 s recorded |
| GNOME 50.6 (Fedora 44, PipeWire 1.6.9) [4436] | +26 ms / −2 ms | +25 ms, step 0.3 ms | 14.8 s of about 14.2 s |
| KDE Plasma 6.3.6 (Debian 13, PipeWire 1.4.2) [4437] | +15 ms / −5 ms | +1 ms, step −0.4 ms | 14.4 s of 14.2 s |
| KDE Plasma 6.6.6 (Kubuntu 26.04, PipeWire 1.6.2) [4437] | +21 ms / +2 ms | +20 ms, step 0.0 ms | 14.9 s of 14.2 s |
| Hyprland 0.56.2 (Omarchy guest, PipeWire 1.6.9) [4459] | −20 ms / −15 ms | −18 ms, step 0.2 ms | 15.8 s of 14.3 s |
| Budgie 10.10.3, labwc 0.20.2 (Fedora 45 guest, PipeWire 1.6.9) [4437] | +37 ms / +73 ms | +35 ms, step −0.1 ms | 15.7 s of 14.2 s |

On every desktop: with the toggle off and the bursts still playing into the
simulated microphone, the clip holds no 1 kHz (peak at most 0.0004 of full
scale, against 0.35 when recorded); after the unplug the recording reports
`microphoneLost`, the clip holds the bursts up to the unplug and none of a tone
that plays on an unrecorded output whose monitor is the default input; a
request for a default input that is an output's monitor is refused with the
reason (not run on the GNOME machine, whose default input is a real device the
lab never opens).

Single flashes sit one frame either side of the median in every marker run,
with or without the microphone: the marker's 30 fps and the capture's frame
grid slip by a whole frame now and then. Within about 5 s of Record, pairs can
sit one or two frames off when the recording starts 2 s after `prepare`; with
6 s between them they do not, so the runs use `--lead 6`. The two guests are
QEMU machines with software rendering: their system-audio recordings sit
about +25 ms (Budgie) and −15 ms (Hyprland) too, so the microphone adds little
to what the guest's own picture path does. The clip can be a little longer than
the driver's count of the time recorded: its first frame is taken while the
start call is still running.

The lab's simulated microphone itself failed on PipeWire 1.0.5: a remapped
source left without a reader sometimes passed pure silence to every later
reader, while the sink's monitor carried the sound (12 of 21 recordings on
Cinnamon; an independent `parec` on the source, joined four seconds into such a
recording, read silence too; the recorder was fed buffers throughout and never
called the microphone lost). `virtual_mic.py` now checks the source for sound
when it makes it and keeps it in use with a loopback into a silent sink: 10 of
10 since.

## Click highlights

A ring opens at the pointer on each button press and fades over 0.45 s, so a
viewer can see what was clicked. Off by default:
`preferences.recordingClickHighlights` (**Settings → Capture → Screen recording
→ Click highlights**) for recordings started from the tray, the dashboard or
the hotkey, and `clickHighlights: true` on `recording.prepare`.

This needs pointer button events from the desktop, and each desktop gives them
differently or not at all. `recording.capabilities` reports what this session
has as `clickHighlights {available, route, drawnBy, targets, reason?,
message?}`, and **Settings → Advanced → Desktop support details** shows it.
`drawnBy` is `recorder` where put.gd draws the ring into the clip and
`compositor` where the desktop draws it on the screen.

| Desktop | `route` | Presses come from | Drawn by | Targets |
| --- | --- | --- | --- | --- |
| Cinnamon and other X11 sessions | `x11-xinput2` | XInput 2.1 raw button events selected on the root window by the helper, on a connection of its own | put.gd, into the clip only | monitor, desktop, region, window |
| GNOME | `gnome-shell-extension` | The put.gd Shell extension (recording protocol 3, extension version 21), see [GNOME](../../desktops/gnome.md) | put.gd, into the clip only | monitor, desktop, region, window |
| Hyprland | `hyprland-mouse-binds` | Non-consuming runtime mouse binds whose Lua callback reports on the event socket, see [Hyprland](../../desktops/hyprland.md) | put.gd, into the clip only | monitor, desktop, region, window |
| KDE Plasma | `kwin-mouseclick-effect` | Nothing reaches put.gd; KWin's own Mouse Click Animation effect is switched on for the recording, see [Plasma](../../desktops/kde.md) | KWin, on the screen and so in the stream | monitor, desktop, region |
| Budgie 10.10 (labwc) | `labwc-mouse-binds` | That a button went down: mouse binds in labwc's `rc.xml` for the length of the recording, which call the resident over the session bus. Where: the recording helper's own capture, which reads the pointer from a cursor session; see [below](#budgie-two-halves) and [Budgie](../../desktops/budgie.md) | put.gd, into the clip only | monitor, desktop, region |

A `prepare` that asks for click highlights where they are unavailable, or for a
target kind the route does not cover, is refused with `recording_unsupported`,
`details.field` `clickHighlights` and `details.reason` (the capability's
reason, or `click_target_unsupported`, with the capability's
`targetReasons[kind]` as the message where it has one). The remembered
preference is not a request: where the desktop has no route the recording
starts without rings.

Two desktops leave window recordings out, each for a reason in the compositor:
on Plasma KWin draws the rings on the screen and a window stream carries the
window alone; on Budgie a window capture has no pointer position (below).
**Desktop support details** says so in its Click highlights row, after the
route: "Not in window recordings:" and the adapter's reason
(`targetReasons.window`). The row's value takes its whole column and its
wrapped height; before 26.1110.4459 it was cut off after one or two short
lines.

### Drawing

Where put.gd draws, the ring is rendered into the frames on the encoder's input
(`src/record/ClickOverlay.*`), after `videorate`, so nothing appears on the
screen and the compositor's capture rules cannot hide it. A ring's frame is
first copied into plain memory and drawn there, never in place: the frame may
share its pixels with the one before it, and with a VA encoder it lives in the
encoder's own surface pool, where a surface mapped to change a few pixels came
back holding nothing else (a green frame with the ring on it, measured with
`vah264lpenc` on Intel). The press's position
is mapped from desktop coordinates into the recorded picture
(`mapClick`): the recorded area for a monitor, desktop or region, and the
window's own rectangle at that press for a window recording; a press outside is
not in the clip. The ring is amber for the main button, blue for the secondary
one and off-white for the middle one, with a dark rim so it reads on light
content, and its size follows the recording's pixel density (22 logical pixels
across at its widest).

A frame that `videorate` repeated shares its pixels with the frame before it,
so a ring frame gets its own copy and is never treated as a repeat: after the
adaptive rate has stepped down, repeats stay out of the encoder (#1045), but
the ring's frames are kept, so a click on a still screen animates in full.
Presses while the recording is paused are not part of the clip.

On X11 the helper reads the presses itself. Elsewhere the desktop reports them
to the resident (`RecordingClicks`, `src/core/RecordingClicks.h`), which starts
watching at Record, keeps watching across a pause and stops when the capture
stops however it ends; each press goes to the helper as a `click {x, y, button,
window?}` command, or as `click {button, pointer: true}` from a route that
knows only that a button went down (Budgie): the helper then places it at its
capture's pointer position. The resident stamps each press when the desktop
reports it (`atUs`, `CLOCK_MONOTONIC`, which the helper shares) and the ring is
drawn at that moment of the clip, however long the command takes to reach the
pipeline; frames that old are still on their way to the encoder. Nothing is watched before Record or after Stop, no
route grabs the pointer and no route consumes an event. When the desktop cannot
start the watch, or the helper has no pointer position for a press it is asked
to place, the recording goes on without rings and `inspect` reports
`progress.clickHighlightsError`. That field is the service's own: the helper's
progress reports, several a second, replace one another and leave it in place
(before 26.1110.4465 each report replaced the whole object and the reason was
gone within the second). Where starting the watch shows on screen, the
watch starts first and the capture once the desktop has answered, within
1.5 s: a labwc reconfigure turns the pointer into the default arrow for about
130 ms (measured on labwc 0.20.2, in a recording across a `SIGHUP`; window
decorations and everything else stayed as they were), so Budgie's binds are in
place a quarter of a second before the first frame.

### Budgie: two halves

labwc, Budgie 10.10's compositor, has no IPC, no script interface and no event
stream. A press reaches put.gd in two halves.

**That a button went down.** labwc's one hook is a `<mousebind>` in `rc.xml`
that runs a command. For the length of a recording the resident adds
(`src/platform/budgie/BudgieClicks.*`) binds on Left, Middle and Right `Press`
in the `Client` context (bare, and with Shift, Ctrl, Alt, Super and Ctrl+Shift:
labwc compares a press's modifiers with a bind's exactly) and in the `Titlebar`
context, each running

    dbus-send --session --type=method_call --dest=org.putgd.App.Budgie
      /org/putgd/Clicks org.putgd.App.Budgie.Click uint32:<button>

the way put.gd's key bindings call `Trigger`: no put.gd path is written and no
put.gd process starts. Then it sends labwc `SIGHUP`, which is what `labwc
--reconfigure` does.

- A bind in the `Client` or `Titlebar` context does not take the press from the
  window: labwc swallows a press only for `Frame` and `All` binds
  (`process_press_mousebinding` in labwc's `src/input/cursor.c`).
- A later bind replaces an earlier one with the same context, button, event and
  modifiers (`mousebind_the_same`, `src/config/mousebind.c`). So each of
  put.gd's binds carries the actions of the bind it stands in for: the user's
  own, copied as written, or labwc's default `Focus` and `Raise` where the file
  asks for `<default />`.
- `rc.xml` belongs to Budgie's `labwc-bridge`, which parses it once, keeps the
  tree in memory and writes it back at the next setting it mirrors
  (`labwc_bridge/config.py`), without anything added since. So the resident
  watches the file: while a recording wants clicks, binds the bridge dropped
  are put back and labwc reads the file again; at every other time, and when
  the resident starts, binds found there are taken out (after a crash, or
  written back by a bridge that happened to read the file during a recording).
  Every element put.gd adds carries `putgd="click-highlights"`, which labwc
  ignores and the bridge's XML round trip keeps, so they are found whatever the
  file's layout; nothing else in the file changes, and removing them gives back
  the user's file byte for byte.

**Where.** The command carries no position, and Wayland tells a client the
pointer's place only over its own surfaces. labwc reports it in one place: an
`ext-image-copy-capture` pointer cursor session, which wlroots feeds from the
output's hardware cursor (`types/ext_image_capture_source_v1/output.c`), and
which goes silent as soon as a capture session asks for a painted cursor,
because wlroots then locks software cursors. So a recording with click
highlights captures **without** a painted cursor and the helper draws the
cursor itself (`ImageCopySource`, `pointer`): the cursor session gives the
position and hotspot, its own capture session the cursor's picture, and each
frame is copied and the cursor blended into the copy. The compositor sends no
frame for a cursor that moves over a still screen, so the helper pushes the
last frame again with the cursor at its new place, at most 60 times a second.
Those cursor-only frames are flagged for the cadence lock
([Wayland recording](wayland.md)), which gives one a slot only at the slot's
own time. Taken for captured frames they held every slot up to 23 ms early
(at 30 frames a second), the screen's next change then always arrived early
for the slot after, was dropped and showed with a later frame: about 31 ms
late on average and up to 48 ms, the picture behind the sound for as long as
the pointer moved. With the rule a change is 8 ms late on average and at most
24 ms, the lock's own rounding to the grid (measured with a screen that changed
every 137 ms under a moving cursor; that test went in the second round of
[removed tests](../../testing/removed-tests.md#second-round)). At 50 and 60 frames a second the slot is no longer than the
cursor's pace, so a cursor-only frame may come 6 and 8 ms before the slot's
time, enough for one to land in every slot.
A press is placed at the pointer position the helper holds when it arrives.
A recording never loses its cursor to this. At `prepare` the capture is
reopened with the compositor's painted cursor, as in a recording without
highlights (then there is no pointer position, no rings, and the first press
puts the reason in `progress.clickHighlightsError`), in two cases: the pointer
is on the output and its picture does not arrive; or no output at all reports
the pointer, which is what a compositor without a hardware cursor looks like.
A pointer that is on another output keeps tracking, and one that comes onto the
recorded output before its picture has been delivered is drawn as a plain arrow
until it has.

What this leaves out, each for a reason in the compositor:

| Not highlighted | Why |
| --- | --- |
| A **window** recording | wlroots' window capture source (`types/ext_image_capture_source_v1/scene.c`) has no `get_pointer_cursor`, so `create_pointer_cursor_session` leaves the session inert (`types/wlr_ext_image_copy_capture_v1.c`); the output's cursor session reports output pixels, and labwc gives no window geometry to map them. `prepare` refuses with `click_target_unsupported` |
| A press on Budgie's panel or another layer-shell surface, or on an X11 override-redirect window | Only an `All` bind matches there (`node_type_contains`, `src/common/node-type.c`), and labwc swallows the press for an `All` bind |
| A press with another modifier combination | labwc compares modifiers exactly; put.gd binds the six sets listed above |
| A press while the application under the pointer hides the cursor | The cursor source reports only a visible hardware cursor; the press gets no ring |
| A compositor without a hardware cursor | No output reports the pointer: the recording keeps the compositor's painted cursor and has no rings; `progress.clickHighlightsError` says there is no pointer position |
| A turned output | Its recording keeps the compositor's painted cursor; `clickHighlightsError` says there is no pointer position |

The clip otherwise matches a Budgie recording without highlights: the same
cursor picture, taken from the compositor's own cursor buffer. Measured in two
region recordings with the pointer at rest at the same place (labwc 0.20.2,
26.1110.4465): 56 dark pixels in each cursor, the drawn one a pixel right and a
pixel down of the painted one, because the cursor session reports the pointer in
whole pixels.

Without a hardware cursor (labwc started with `WLR_NO_HARDWARE_CURSORS=1`, the
same guest and build): the helper logs that the compositor paints the cursor,
the clip shows the cursor at all three press places, no ring is drawn, each
press is answered `pointer_position_unavailable`, and `recording.inspect` holds
"The compositor has reported no pointer position for this recording (labwc
reports it only for an output with a hardware cursor), so clicks cannot be
highlighted".

### Route proofs

| Desktop | Proof |
| --- | --- |
| Cinnamon 6.6.9 (Mint 22.3, physical) | `xinput test-xi2 --root` received `RawButtonPress` and `RawButtonRelease` for clicks made inside another client's window |
| GNOME Shell 50.6 (private headless Shell on Fedora 44) | Ten injected clicks over a GTK4 window, 50 ms to 0 ms long: `notify::user-time` fired for all ten with the button, position and event time; `captured-event` on the stage and an extension's `Clutter.Event.add_filter` saw none (Mutter's own filter, added first, stops events it delivers to a client); polling `global.get_pointer()` every 4 ms saw six and missed every 0 ms click |
| KWin 6.3.6 (Debian 13) | `Effects.supportInformation("mouseclick")` reports `enabled: false` after a load, `enabled: true` after the shortcut and `false` after it again |
| KWin 6.8.2 (Aurora, physical) | With the effect loaded and toggled on over D-Bus, a frame of an output screencast taken while clicking shows the rings; with it off, none. A scripted effect can connect `effects.mouseChanged` but has no `callDBus`; a script has `callDBus` but no button signal (KWin `scripting/scriptedeffect.cpp`, `scripting.cpp`) |
| Hyprland 0.56.2 (Omarchy 4.5 guest) | Four clicks, 50 ms to 0 ms long, left and right, arrived as `custom>>` events with press, release and position; `j/binds` showed the binds `non_consuming`; a terminal under the pointer still received every click; binds removed, 251 before and after |
| labwc 0.20.2 (Fedora 45 Budgie guest) | A `Client` bind with `Focus`, `Raise` and `Execute`: four clicks, 60 ms to 0 ms long, each ran the command and each reached the terminal under the pointer. One natural-scroll toggle in Budgie's settings removed the bind from `rc.xml` (the bridge's rewrite). A cursor session's positions followed the pointer until a capture session with a painted cursor was created, then `leave` and nothing more |
