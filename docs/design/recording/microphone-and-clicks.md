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
  `recording.inspect` reports `progress.microphoneLost`.

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
what is left. The sound server can move the stream to another source, the
default, which may be a monitor: the tap compares the source's
`current-device` with the one it opened four times a second and drops
everything from the first difference. Or the stream can stay connected to
nothing, with no error and no buffers: an open microphone delivers buffers all
the time, silence included, so two seconds without one counts as lost. A
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
| Budgie 10.10 (labwc) | `none` | **Not offered**: see below and [Budgie](../../desktops/budgie.md) | | |

A `prepare` that asks for click highlights where they are unavailable, or for a
target kind the route does not cover, is refused with `recording_unsupported`,
`details.field` `clickHighlights` and `details.reason` (the capability's
reason, or `click_target_unsupported`). The remembered preference is not a
request: where the desktop has no route the recording starts without rings.

### Drawing

Where put.gd draws, the ring is rendered into the frames on the encoder's input
(`src/record/ClickOverlay.*`), after `videorate`, so nothing appears on the
screen and the compositor's capture rules cannot hide it. The press's position
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
window?}` command. Nothing is watched before Record or after Stop, no route
grabs the pointer and no route consumes an event. When the desktop cannot start
the watch, the recording goes on without rings and `inspect` reports
`progress.clickHighlightsError`.

### Why Budgie has none

labwc, Budgie 10.10's compositor, has no IPC, no script interface and no event
stream. Two pieces would be needed, and neither is usable:

- **Events.** labwc's one hook is a `<mousebind>` in `rc.xml` that runs a
  command. A `Client`-context bind that keeps `Focus` and `Raise` and adds
  `Execute` does fire on every press without taking it from the window
  (measured on labwc 0.20.2: four clicks, 60 ms to 0 ms long, each ran the
  command and each reached the terminal under the pointer). But `rc.xml`
  belongs to Budgie's `labwc-bridge`, which keeps the file in memory and writes
  it back without foreign entries at the next setting it mirrors (measured: one
  natural-scroll toggle removed the bind), and offers put.gd key bindings only.
  put.gd's Budgie adapter writes through Budgie's settings, never into
  `rc.xml`.
- **Position.** A command run by a mousebind carries no coordinates, and
  Wayland tells a client the pointer's position only over its own surfaces. The
  one place labwc reports it is an `ext-image-copy-capture` pointer cursor
  session, and that goes silent (`leave`) the moment a capture session asks for
  a painted cursor, because wlroots then locks software cursors and its cursor
  source reads only the hardware cursor
  (`types/ext_image_capture_source_v1/output.c`). Every put.gd recording on
  Budgie asks for a painted cursor. Measured on the same guest: positions
  followed the pointer until that session was created, then `leave` and nothing
  more.

So the capability reports `available: false`, `reason: labwc_no_click_events`,
and the Settings switch says the same. The microphone works on Budgie as
elsewhere.

### Route proofs

| Desktop | Proof |
| --- | --- |
| Cinnamon 6.6.9 (Mint 22.3, physical) | `xinput test-xi2 --root` received `RawButtonPress` and `RawButtonRelease` for clicks made inside another client's window |
| GNOME Shell 50.6 (private headless Shell on Fedora 44) | Ten injected clicks over a GTK4 window, 50 ms to 0 ms long: `notify::user-time` fired for all ten with the button, position and event time; `captured-event` on the stage and an extension's `Clutter.Event.add_filter` saw none (Mutter's own filter, added first, stops events it delivers to a client); polling `global.get_pointer()` every 4 ms saw six and missed every 0 ms click |
| KWin 6.8.2 (Aurora, physical) | With the effect loaded and toggled on over D-Bus, a frame of an output screencast taken while clicking shows the rings; with it off, none. A scripted effect can connect `effects.mouseChanged` but has no `callDBus`; a script has `callDBus` but no button signal (KWin `scripting/scriptedeffect.cpp`, `scripting.cpp`) |
| Hyprland 0.56.2 (Omarchy 4.5 guest) | Four clicks, 50 ms to 0 ms long, left and right, arrived as `custom>>` events with press, release and position; `j/binds` showed the binds `non_consuming`; a terminal under the pointer still received every click; binds removed, 251 before and after |
| labwc 0.20.2 (Fedora 45 Budgie guest) | As above: the mousebind fires but is overwritten; the cursor session stops at the first painted-cursor capture |
