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
  screen** and the shortcut start with the remembered state; a change on the
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
  `recording.inspect` reports `progress.microphoneLost`. Its stream is never
  moved to another input: see [Capture and mix](#capture-and-mix).
- **Without one, the bar says so.** The toggle reads **No microphone** and is
  off; its tooltip and `recordingMicrophone.unavailable` give the reason.

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
the tray, dashboard and shortcut still open a recording, the toggle shows
ticked and unavailable with the reason in its tooltip, and nothing is recorded
from an input.

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
`current-device` with the one it opened twenty times a second and drops
everything from the first difference. A
microphone that cannot be opened at Record (unplugged since the session was
prepared) starts the recording without it and reports the same way.

The clip's audio then has no packets after the loss; the export fills the rest
with silence, as it does for a pause's hole.

### What a session reports

`recording.inspect` `progress` gains:

| Field | Meaning |
| --- | --- |
| `audioSources {system, microphone}` | What the clip's audio track holds, from Record on |
| `recordingMicrophone {requested, effective, available, device?, unavailable?, lost?}` | The bar's toggle: what the user asked for, what is recorded, the device's label or why there is none |
| `microphoneLost` | Present once the microphone went away: the sentence the bar shows |

`config.audio` is `{system, microphone, microphoneDevice?}`. In review only the
output changes: a silent format drops the track, and going back to MP4 or WebM
brings back exactly the sources that were captured. The review window names the
track **System audio**, **Microphone** or **System audio and microphone**.

### Measured

Physical GNOME Wayland (Fedora 44, Mutter 50.6, `mutter-screencast`, OpenH264
and `avenc_aac`), scratch build of the #1364 branch at `32a9a763e` plus the
change, a simulated microphone (a null sink remapped into a source; no real
microphone), region 1280×720, driven by `tools/lab/record_run.py`:

| Run | Result |
| --- | --- |
| Microphone only, MP4 | Bursts 16.0 ms after the flash (13 pairs; working clip 15.7 ms); motion score 18.1 |
| Microphone and system audio, 25 s, MP4, two runs | 12 ms and 4 ms (25 pairs each); both tones present |
| The same across a 3 s pause, MP4 / WebM | Median −12 ms / −2.7 ms; step at the pause 0.4 ms / 0.0 ms; export against working clip −0.7 ms / −0.2 ms |
| Toggle off, bursts still playing into the simulated microphone | No 1 kHz in the clip (level 0.000); the system tone present |
| Simulated microphone unloaded 6 s in | Recording went on to 18.8 s, 565 frames, none dropped; microphone audio to 6.8 s, silence after; exports and decodes |

One frame at 30 fps is 33.3 ms. Single pairs sit one frame either side of the
median in every marker run, with or without the microphone: the marker's 30 fps
and the capture's frame grid slip by a whole frame now and then. Within about
5 s of Record, pairs can sit one or two frames off when the recording starts
2 s after `prepare`; with 6 s between them they do not, so the acceptance runs
use `--lead 6`. Both capture branches' timestamps were exactly contiguous in
every run (the helper's debug log prints them).

## Click highlights

A ring opens at the pointer on each button press and fades over 0.45 s, so a
viewer can see what was clicked. Off by default:
`preferences.recordingClickHighlights` (**Settings → Capture → Screen recording
→ Click highlights**) for recordings started from the tray, the dashboard or
the shortcut, and `clickHighlights: true` on `recording.prepare`.

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
`progress.clickHighlightsError`. Where starting the watch shows on screen, the
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
A press is placed at the pointer position the helper holds when it arrives.
A recording never loses its cursor to this: when the pointer is on the output
at `prepare` and its picture does not arrive, the capture is reopened with the
compositor's painted cursor (then there is no pointer position and no
highlights); a pointer that comes onto the output before its picture has been
delivered is drawn as a plain arrow until it has; and an output without a
hardware cursor already has the cursor in the frames the compositor hands
out.

What this leaves out, each for a reason in the compositor:

| Not highlighted | Why |
| --- | --- |
| A **window** recording | wlroots' window capture source (`types/ext_image_capture_source_v1/scene.c`) has no `get_pointer_cursor`, so `create_pointer_cursor_session` leaves the session inert (`types/wlr_ext_image_copy_capture_v1.c`); the output's cursor session reports output pixels, and labwc gives no window geometry to map them. `prepare` refuses with `click_target_unsupported` |
| A press on Budgie's panel or another layer-shell surface, or on an X11 override-redirect window | Only an `All` bind matches there (`node_type_contains`, `src/common/node-type.c`), and labwc swallows the press for an `All` bind |
| A press with another modifier combination | labwc compares modifiers exactly; put.gd binds the six sets listed above |
| A press while the application under the pointer hides the cursor, or on an output without a hardware cursor plane | The cursor source reports only a visible hardware cursor. With no position ever reported, `inspect` says so in `progress.clickHighlightsError` |
| A turned output | Its recording keeps the compositor's painted cursor; `clickHighlightsError` says there is no pointer position |

The clip otherwise matches a Budgie recording without highlights: the same
cursor picture, taken from the compositor's own cursor buffer, at the same
place.

### Route proofs

| Desktop | Proof |
| --- | --- |
| Cinnamon 6.6.9 (Mint 22.3, physical) | `xinput test-xi2 --root` received `RawButtonPress` and `RawButtonRelease` for clicks made inside another client's window |
| GNOME Shell 50.6 (private headless Shell on Fedora 44) | Ten injected clicks over a GTK4 window, 50 ms to 0 ms long: `notify::user-time` fired for all ten with the button, position and event time; `captured-event` on the stage and an extension's `Clutter.Event.add_filter` saw none (Mutter's own filter, added first, stops events it delivers to a client); polling `global.get_pointer()` every 4 ms saw six and missed every 0 ms click |
| KWin 6.3.6 (Debian 13) | `Effects.supportInformation("mouseclick")` reports `enabled: false` after a load, `enabled: true` after the shortcut and `false` after it again |
| KWin 6.8.2 (Aurora, physical) | With the effect loaded and toggled on over D-Bus, a frame of an output screencast taken while clicking shows the rings; with it off, none. A scripted effect can connect `effects.mouseChanged` but has no `callDBus`; a script has `callDBus` but no button signal (KWin `scripting/scriptedeffect.cpp`, `scripting.cpp`) |
| Hyprland 0.56.2 (Omarchy 4.5 guest) | Four clicks, 50 ms to 0 ms long, left and right, arrived as `custom>>` events with press, release and position; `j/binds` showed the binds `non_consuming`; a terminal under the pointer still received every click; binds removed, 251 before and after |
| labwc 0.20.2 (Fedora 45 Budgie guest) | A `Client` bind with `Focus`, `Raise` and `Execute`: four clicks, 60 ms to 0 ms long, each ran the command and each reached the terminal under the pointer. One natural-scroll toggle in Budgie's settings removed the bind from `rc.xml` (the bridge's rewrite). A cursor session's positions followed the pointer until a capture session with a painted cursor was created, then `leave` and nothing more |
