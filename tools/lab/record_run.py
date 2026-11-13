#!/usr/bin/env python3
"""Recording runs on a physical desktop: pause/resume A/V and format/motion sweeps, builds interleaved (stdlib only).

Run it on the host from a plain SSH shell, holding the desktop's lock. It reads
the desktop session's display, bus and session type from the session process
(plasmashell, gnome-shell or cinnamon), then:

1. opens `cover_player.py play` (magenta, pinned to --output) and fails closed
   unless the player confirms its placement and `cover_player.py precheck`
   finds the output's rectangle covered on the host's loopback VNC tile;
2. starts --video in the middle of the cover: the looped `--av-marker` clip
   (`marker`), a scrolling pattern (`pattern`), or nothing (`still`, the
   frozen control for motion scores);
3. per round, format and build: starts that build's resident with private
   config and data directories under --run, records the target
   (`recording.prepare`, `start`, optional `pause`/`resume`, `stop`, `saveCopy`,
   then `cancel`) and quits it;
4. measures the working clip and the saved copy: `--motion` always; with the
   marker and sound, `--av-sync`, split at the pause with `pairTimesS` into the
   capture step (last two pairs before the pause against the first three after
   it, with whole 33.3 ms frame slips folded out; `rawCaptureStepMs` keeps them) and
   the export's shift after the pause (`exportMinusWorkingMs`);
5. with --frames, keeps one frame of each saved copy only if `check-frame`
   finds it at least half cover colour; otherwise the frame is deleted
   unopened and so are that recording's clips. A region must therefore
   straddle the video's edge (for example 0,0,960,540 on 1920x1080).

A marker clip with fewer than --min-pairs flash/burst pairs didn't show the
marker, so it is deleted unopened. Clips stay on the host; results.json holds
numbers, identities, host load and the kernel's GPU hang count for the run.

  record_run.py --run DIR --build LABEL=PATH [--build ...] --analyzer APPDIR
                [--video marker|pattern|still] [--target monitor|region|window]
                [--rect X,Y,W,H] [--identity NAME] [--format mp4 ...] [--rounds 3]
                [--output NAME] [--vnc-port 5900] [--lead 2] [--pause-at 8]
                [--pause-for 3] [--after 8] [--prepare-timeout 120] [--frames]
                [--mic off|virtual|NAME] [--system on|off|tone] [--unplug-at S]
                [--click S,X,Y[,BUTTON] ...] [--click-highlights on|off]
                [--click-vnc HOST:PORT] [--delayed-capture desktop:3]
                [--hide-bar]

A window target without --identity records the focused window, which on Wayland
is the cover. On X11 a window is named by its id and a cover started from a shell
does not get the focus (on Cinnamon the desktop window keeps it), so the driver
finds the cover's window by its process id in the window manager's list (`xprop`)
and passes that id. On Hyprland a full-screen window's state passes to the next
window that opens (`misc:on_focus_under_fullscreen` 1), so put.gd's controls would
hide the cover: for a window run, and for a --delayed-capture run (the review
window opens before its second capture), the driver sets that option to 0 and
puts the previous value back when the cover closes.

PATH is an AppImage (extracted once under DIR for its putgd-cli) or a directory
holding `putgd` and `putgd-cli` (an installed package: /usr/bin). APPDIR is an
extracted AppImage whose putgd-record reports `pairTimesS` (#143 or later).
--pause-for 0 records without a pause.

Microphone runs (#1364) never use a real microphone. `--mic virtual` creates a
simulated one with virtual_mic.py (a null sink remapped into a source), plays
the marker's tone bursts into it and asks put.gd to record that device; the
modules are unloaded by their ids when the run ends, also after a failure.
`--mic NAME` records an existing source; `--mic default` records the system
default input, expecting the refusal where that is an output's monitor;
`--mic muted` makes the simulated microphone and plays the bursts into it but
leaves put.gd's microphone off, so the clip must not hold them. `--mic late`
prepares each recording with no microphone to open and plugs the simulated one
in while the bar is at Ready (as the default input), switches it on with
`recording.audio`, pulls it, and plugs it in again: `recording.inspect` must
follow every step (`readyFollow` in the round's entry, with the seconds each
took and, with --frames, a frame of the desktop per step), and the clip that is
then recorded must hold the bursts.
`--hide-bar` hides put.gd's recording bar at Ready (`recording.overlay
{visible: false}`). Where the bar cannot sit outside the recorded area (a whole
monitor on a one-monitor desktop) its Stop handle is in the clip
(`overlay.overlayPlacement` `handle` in the round's entry), and with --frames
the frame gate can take it for something that is not the cover and delete the
clips; that is listed under `problems` now, with this option named.
`--delayed-capture desktop:3` checks that a delayed capture (#1363) and a
recording work in one resident session: before `recording.prepare` and again
after the recording is cancelled it calls `action.invoke` with `delaySeconds`,
and the capture must end `completed` no sooner than its delay, saved to the
run's private archive and at least half cover colour (`delayedCaptures` in the
round's entry). With --frames a frame of the desktop is kept from each
countdown and `countdownPixels` counts what the pill changed at the top centre.
A run with `--unplug-at` and more than one recording (formats, rounds, builds)
plugs the simulated microphone in again before each later one. A run with
`--output-gone-at` is one recording: the private output is not made again, so
more than one --build, --format or round is refused before anything starts.
`--system off` records no system audio; `--system tone` moves the default
output to a private null sink and plays a quiet 440 Hz tone into it, so the
clip must hold the microphone's bursts and that tone. `--system leak` plays the
same tone without recording system audio and makes that output's monitor the
default input: the clip must hold no 440 Hz at all, also after `--unplug-at`
has pulled the microphone and the sound server has moved its stream there. `--unplug-at S` removes
the simulated microphone S seconds into each recording, which must still end
with a playable clip of the whole time recorded. `--output-gone-at S` (with
`--system tone`) removes the private output that system audio is recorded from,
S seconds in, as a headset pulled or a monitor switched off would: with
`--output-fallback` another private output is made the default first, so there
is one to move to; without it nothing is left but what the sound server makes
up, and on a machine with a sound card that needs `--cards-off`, which turns
every card's profile off for the run (no lab sound can then reach a real
output) and puts the profiles and the defaults back. The recording must go on
and the clip must hold the whole time recorded. Each clip's entry then has `tones` (band levels at 440 Hz
and at the marker's 1 kHz) beside the A/V offsets. A round whose clip is more than 1.5 s shorter than the
time recorded, lacks the simulated microphone's bursts, holds them with the microphone off or holds the
unrecorded output's tone lists that under `problems`, and the run exits 1. --real-data-home lets an AppImage use
KWin's screencast protocol (its entry must be in the user's own data dir). The same data directory serves every
build, so one portal approval with restoring allowed covers the whole run.
When `prepare` waits for a portal picker, the driver says so and keeps waiting
up to --prepare-timeout.

Click runs (#1364). Each `--click S,X,Y[,BUTTON]` presses BUTTON (left, middle
or right) at desktop pixel X,Y, S seconds after Record, as real pointer input
through the desktop's VNC tile (the pointer is moved there 2.5 s earlier, so
space presses at least 4 s apart and keep them on the cover's plain colour,
away from the video). The recording is prepared with `clickHighlights: true`
(`--click-highlights off`: without, and then no ring may appear). Afterwards
`putgd-record --click-check` measures the clip at each press's place, in clip
pixels: first when the pointer arrived there (the recording shows the pointer,
and the cover keeps it visible for these runs), then what appeared afterwards
against a frame with the pointer resting. Every press must have a ring (or
none, when off), centred within 4 pixels of the press (within the measured box
where the compositor draws it: KWin adds a label), and showing between one
frame before and 250 ms after the press, counted from the pointer's arrival in
the same clip, so the clip's own start does not matter. `clickCheck` in the
round's result holds the numbers (`delayMs` per ring), and one frame at the
first ring's peak is kept beside the other frames. Not together with a pause.
In a guest, QEMU's VNC is on the host: `--vnc-port 0 --click-vnc 10.0.2.2:59NN`.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import selectors
import signal
import statistics
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import lab_resident  # noqa: E402

RunError = lab_resident.RunError
FRAME_MS = 1000 / 30
# How long before each scripted press the pointer is moved to its place.
CLICK_LEAD_S = 2.5
ANIMATIONS = ('gif', 'webp')
SESSION_VARIABLES = ('DISPLAY', 'XAUTHORITY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_SESSION_TYPE', 'XDG_CURRENT_DESKTOP',
                     'WAYLAND_DISPLAY', 'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION', 'KDE_FULL_SESSION', 'KDE_SESSION_VERSION',
                     'XDG_DATA_HOME', 'HYPRLAND_INSTANCE_SIGNATURE')


def log(message):
    print(time.strftime('%H:%M:%S ') + message, file=sys.stderr, flush=True)


def session_environment():
    """The desktop session's environment for clients started from SSH."""
    uid = os.getuid()
    for process in ('plasmashell', 'gnome-shell', 'cinnamon', 'quickshell', 'budgie-panel'):
        pids = subprocess.run(['pgrep', '-u', str(uid), '-x', process], capture_output=True, text=True).stdout.split()
        if pids:
            break
    else:
        raise RunError('no desktop session process (plasmashell, gnome-shell, cinnamon, quickshell, budgie-panel) for this user')
    pairs = Path('/proc/%s/environ' % pids[0]).read_bytes().decode(errors='replace').split('\0')
    found = dict(pair.split('=', 1) for pair in pairs if '=' in pair)
    runtime = '/run/user/%d' % uid
    env = {'HOME': os.environ['HOME'], 'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'LANG': found.get('LANG', 'C.UTF-8'),
           'USER': os.environ.get('USER', ''), 'XDG_RUNTIME_DIR': runtime}
    env.update({name: found[name] for name in SESSION_VARIABLES if found.get(name)})
    if env.get('XDG_SESSION_TYPE') == 'wayland' and not env.get('WAYLAND_DISPLAY'):
        shown = subprocess.run(['systemctl', '--user', 'show-environment'], capture_output=True, text=True).stdout
        env.update(line.split('=', 1) for line in shown.splitlines() if line.startswith('WAYLAND_DISPLAY='))
    if env.get('WAYLAND_DISPLAY') and not env['WAYLAND_DISPLAY'].startswith('/'):
        env['WAYLAND_DISPLAY'] = runtime + '/' + env['WAYLAND_DISPLAY']
    env['QT_QPA_PLATFORM'] = 'wayland' if env.get('XDG_SESSION_TYPE') == 'wayland' else 'xcb'
    return env, process


def split_at_pause(times, offsets, pause_s, guard=0.2):
    before = [o for t, o in zip(times, offsets) if t < pause_s - guard]
    after = [o for t, o in zip(times, offsets) if t > pause_s + guard]
    return before, after


def whole_frames_removed(ms):
    return ms - FRAME_MS * round(ms / FRAME_MS)


def fold_frames(offsets):
    """Offsets moved by whole 33.3 ms frames next to the clip's common phase (their circular mean).

    The 30 fps marker and capture slip by exactly one frame at random pairs, so a
    clip's offsets are often split between two values one frame apart; taking a
    median across that split invents a half-frame step."""
    if not offsets:
        return []
    angles = [2 * math.pi * o / FRAME_MS for o in offsets]
    phase = math.atan2(sum(map(math.sin, angles)), sum(map(math.cos, angles))) * FRAME_MS / (2 * math.pi)
    return [o - FRAME_MS * round((o - phase) / FRAME_MS) for o in offsets]


def capture_step(times, offsets, pause_s, raw=False):
    """The working clip's change at the pause, with whole frames folded out unless raw; None without two pairs before and three after."""
    before, after = split_at_pause(times, offsets if raw else fold_frames(offsets), pause_s)
    if len(before) < 2 or len(after) < 3:
        return None
    step = statistics.median(after[:3]) - statistics.median(before[-2:])
    return round(step if raw else whole_frames_removed(step), 1)


def export_shift(working, exported, pause_s):
    """Median offset after the pause in the saved copy minus the working clip's."""
    after_w = split_at_pause(working['pairTimesS'], working['offsetsMs'], pause_s)[1]
    after_e = split_at_pause(exported['pairTimesS'], exported['offsetsMs'], pause_s)[1]
    if not after_w or not after_e:
        return None
    return round(statistics.median(after_e) - statistics.median(after_w), 1)


def parse_delayed_capture(text):
    action, _, seconds = text.partition(':')
    if action not in ('desktop', 'monitor') or seconds not in ('3', '5', '10'):
        raise argparse.ArgumentTypeError('ACTION:SECONDS is desktop or monitor, and 3, 5 or 10')
    return text


def frame_edges(target, rect, output):
    """Which sides of a recorded frame sit on an output edge, where a desktop panel could show."""
    if target != 'region' or not rect or not output:
        return ['top', 'bottom', 'left', 'right']
    x, y, w, h = rect
    ox, oy, ow, oh = output
    found = []
    if y <= oy: found.append('top')
    if y + h >= oy + oh: found.append('bottom')
    if x <= ox: found.append('left')
    if x + w >= ox + ow: found.append('right')
    return found


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def pillow_motion(path, samples=8):
    """Mean absolute frame difference across an animated image (the analyzer's FFmpeg has no animated WebP decoder)."""
    from PIL import Image, ImageChops, ImageStat
    image = Image.open(path)
    count = getattr(image, 'n_frames', 1)
    if count < 2:
        return 0.0
    picks = sorted({round(i * (count - 1) / (samples - 1)) for i in range(samples)})
    frames = []
    for index in picks:
        image.seek(index)
        frames.append(image.convert('L').copy())
    diffs = [statistics.mean(ImageStat.Stat(ImageChops.difference(a, b)).mean) for a, b in zip(frames, frames[1:])]
    return round(statistics.mean(diffs), 3)


class Build:
    def __init__(self, label, path, run):
        self.label, self.path = label, Path(path).resolve()
        if self.path.suffix == '.AppImage':
            appdir = run / ('appdir-' + label)
            if not (appdir / 'squashfs-root').is_dir():
                appdir.mkdir(parents=True, exist_ok=True)
                subprocess.run([str(self.path), '--appimage-extract'], cwd=appdir, check=True, stdout=subprocess.DEVNULL)
            self.launcher = [str(self.path), '--background']
            self.cli = str(appdir / 'squashfs-root/usr/bin/putgd-cli')
            self.identity = {'kind': 'appimage', 'file': self.path.name, 'sha256': sha256(self.path)}
        else:
            self.launcher = [str(self.path / 'putgd'), '--background']
            self.cli = str(self.path / 'putgd-cli')
            version = subprocess.run([str(self.path / 'putgd-record'), '--version'], capture_output=True, text=True).stdout.strip()
            self.identity = {'kind': 'installed', 'dir': str(self.path), 'version': version}


class Driver:
    def __init__(self, args):
        self.args = args
        self.run = Path(args.run).resolve()
        for name in ('config', 'data', 'cache', 'state', 'working', 'out', 'frames', 'logs'):
            (self.run / name).mkdir(parents=True, exist_ok=True)
        os.chmod(self.run, 0o700)
        self.env, self.desktop = session_environment()
        # Every put.gd directory is private to the run: config, data, logs (state) and the AppImage's GStreamer registry (cache).
        self.env.update(XDG_CONFIG_HOME=str(self.run / 'config'), XDG_DATA_HOME=str(self.run / 'data'),
                        XDG_CACHE_HOME=str(self.run / 'cache'), XDG_STATE_HOME=str(self.run / 'state'),
                        PUTGD_RECORD_KEEP_WORKING=str(self.run / 'working'))
        # Lab diagnostics such as PUTGD_RECORD_DEBUG_LOG pass through to the resident and its helper.
        self.env.update({k: v for k, v in os.environ.items() if k.startswith('PUTGD_RECORD_') and k != 'PUTGD_RECORD_KEEP_WORKING'})
        self.real_data = None
        if args.real_data_home:
            # An AppImage's KWin screencast entry only works in the user's own data directory. put.gd's data
            # there is moved into the run afterwards, so it must not exist yet.
            self.real_data = Path(os.environ['HOME']) / '.local/share/putgd'
            if self.real_data.exists():
                raise RunError('%s exists; --real-data-home would mix with it' % self.real_data)
            del self.env['XDG_DATA_HOME']
        self.socket = Path(self.env['XDG_RUNTIME_DIR']) / 'putgd' / 'session.sock'
        self.analyzer = Path(args.analyzer).resolve()
        self.cover = None
        self.resident = None
        self.cover_rect = None
        self.hyprland_focus_before = None
        self.card_state = None
        self.mic_state = None
        self.tone = None

    # -- helpers -------------------------------------------------------------
    def cli(self, build, method, params=None, timeout=60):
        return lab_resident.cli(self, build, method, params, timeout)

    def helper(self, *arguments, timeout=300):
        env = dict(os.environ, APPDIR=str(self.analyzer))
        return subprocess.run([str(self.analyzer / 'usr/bin/putgd-record'), *map(str, arguments)], env=env,
                              capture_output=True, text=True, timeout=timeout)

    # -- simulated microphone (#1364) ------------------------------------------
    def virtual_mic(self, *arguments):
        result = subprocess.run([sys.executable, str(HERE / 'virtual_mic.py'), *arguments], capture_output=True, text=True, env=self.env)
        try:
            return json.loads(result.stdout)
        except ValueError:
            raise RunError('virtual_mic %s: %s' % (arguments[0], (result.stdout + result.stderr).strip()[-300:]))

    def open_microphone(self):
        a = self.args
        if a.mic not in ('virtual', 'muted', 'late') and a.system not in ('tone', 'leak'):
            return None
        cards = self.cards_off() if a.cards_off else None
        self.mic_state = self.run / 'virtual-mic.json'
        create = ['create', '--name', 'putgd_lab_%d' % os.getpid(), '--state', str(self.mic_state)]
        if a.system in ('tone', 'leak'):
            create.append('--system-sink')
        if a.mic == 'off' and a.system == 'tone':
            # Nothing of the lab's but the output system audio is recorded from.
            create.append('--no-microphone')
        if a.mic == 'late':
            # The marker's bursts need their output from the start; the microphone itself is plugged in at Ready.
            create.append('--unplugged')
        if a.mic_default:
            # A desktop with a microphone: the simulated one is the default input.
            create.append('--default')
        if a.system == 'leak':
            # The default input becomes that output's monitor: what a pulled microphone's stream is moved to.
            create.append('--monitor-default')
        state = self.virtual_mic(*create)
        if a.mic in ('virtual', 'muted', 'late'):
            # The marker's bursts are the simulated voice: they play into the microphone's sink, not the speakers.
            a.audio_device = state['sink']
        if a.system in ('tone', 'leak'):
            self.tone = subprocess.Popen([sys.executable, str(HERE / 'virtual_mic.py'), 'tone', '--state', str(self.mic_state),
                                          '--seconds', '3600'], env=self.env, start_new_session=True,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if cards:
            state['cardsOff'] = cards
        return state

    def stop_tone(self):
        try:
            if self.tone and self.tone.poll() is None:
                try:
                    os.killpg(self.tone.pid, signal.SIGTERM)
                    self.tone.wait(10)
                except subprocess.TimeoutExpired:
                    os.killpg(self.tone.pid, signal.SIGKILL)
                    self.tone.wait(5)
        except (ProcessLookupError, subprocess.TimeoutExpired) as error:
            log('the tone player did not end cleanly: %s' % error)

    def close_microphone(self):
        # Whatever the tone does, the simulated devices are removed afterwards, and then the cards come back.
        try:
            self.stop_tone()
        finally:
            try:
                removed = self.virtual_mic('remove', '--state', str(self.mic_state)) if self.mic_state and self.mic_state.is_file() else None
            finally:
                self.cards_on()
        return removed

    def pactl(self, *arguments):
        return subprocess.run(['pactl', *arguments], env=self.env, capture_output=True, text=True).stdout.strip()

    def recording_sources(self):
        """The sources put.gd's recorder reads from now, by name, as the sound server lists its streams."""
        names = {line.split('\t')[0]: line.split('\t')[1] for line in self.pactl('list', 'short', 'sources').splitlines() if '\t' in line}
        found, source, ours = [], None, False
        for line in (self.pactl('list', 'source-outputs') + '\nSource Output #end').splitlines():
            line = line.strip()
            if line.startswith('Source Output #'):
                if ours and source is not None:
                    found.append(names.get(source, 'source %s (gone)' % source))
                source, ours = None, False
            elif line.startswith('Source: '):
                source = line[8:]
            elif 'application.process.binary = "putgd-record"' in line or 'application.name = "putgd-record"' in line:
                ours = True
        return found

    def cards_off(self):
        """For a run that removes its output with nothing to fall back on: every sound card's profile off.

        No real output or input exists for the length of the run, so nothing of the lab's can be moved to one and
        heard. The profiles and the defaults are written to RUN/cards.json first and put back when the run ends;
        after a crash, `pactl set-card-profile NAME PROFILE` for each entry of that file does the same."""
        cards, name = {}, None
        for line in self.pactl('list', 'cards').splitlines():
            line = line.strip()
            if line.startswith('Name: '):
                name = line[6:]
            elif line.startswith('Active Profile: ') and name:
                cards[name] = line[16:]
        self.card_state = {'defaultSink': self.pactl('get-default-sink'), 'defaultSource': self.pactl('get-default-source'), 'cards': cards}
        (self.run / 'cards.json').write_text(json.dumps(self.card_state, indent=1) + '\n')
        for card in cards:
            self.pactl('set-card-profile', card, 'off')
        time.sleep(1.5)
        left = [line.split('\t')[1] for line in self.pactl('list', 'short', 'sinks').splitlines() if '\t' in line]
        self.card_state['sinksWhileOff'] = left
        return self.card_state

    def cards_on(self):
        if not self.card_state:
            return
        state, self.card_state = self.card_state, None
        for card, profile in state['cards'].items():
            self.pactl('set-card-profile', card, profile)
        time.sleep(1.5)
        # The server's defaults as they were, by name: turning a card off makes it forget them.
        if state.get('defaultSink'):
            self.pactl('set-default-sink', state['defaultSink'])
        if state.get('defaultSource'):
            self.pactl('set-default-source', state['defaultSource'])
        restored = {'defaultSink': self.pactl('get-default-sink'), 'defaultSource': self.pactl('get-default-source')}
        if restored != {k: state.get(k) for k in restored}:
            log('the defaults are %s, were %s' % (json.dumps(restored), json.dumps({k: state.get(k) for k in restored})))

    def microphone_device(self):
        a = self.args
        if a.mic == 'virtual':
            return json.loads(self.mic_state.read_text())['source']
        return None if a.mic in ('off', 'default', 'muted', 'late') else a.mic

    def clicks(self):
        """--click entries as (seconds, x, y, button), in time order."""
        entries = []
        for text in self.args.click:
            parts = text.split(',')
            if len(parts) not in (3, 4) or (len(parts) == 4 and parts[3] not in ('left', 'middle', 'right')):
                raise RunError('--click wants S,X,Y[,left|middle|right]: %s' % text)
            entries.append((float(parts[0]), int(parts[1]), int(parts[2]), parts[3] if len(parts) == 4 else 'left'))
        return sorted(entries)

    def press_clicks(self, started, sent):
        """Presses each --click at its time after `started` (monotonic) through the VNC tile; appends what was sent."""
        sys.path.insert(0, str(HERE))
        import rfb
        host, _, port = (self.args.click_vnc or '127.0.0.1:%d' % self.args.vnc_port).rpartition(':')
        client = rfb.RFB(int(port), host=host or '127.0.0.1')
        try:
            for seconds, x, y, button in self.clicks():
                time.sleep(max(0, started + seconds - CLICK_LEAD_S - time.monotonic()))
                client.pointer(x, y)
                moved = time.monotonic() - started
                time.sleep(max(0, started + seconds - time.monotonic()))
                client.pointer_button(x, y, button, True)
                sent.append({'s': round(time.monotonic() - started, 3), 'movedS': round(moved, 3), 'x': x, 'y': y, 'button': button})
                time.sleep(0.06)
                client.pointer_button(x, y, button, False)
        except Exception as error:  # the run goes on; the check then reports the missing rings
            sent.append({'error': '%s: %s' % (type(error).__name__, error)})
        finally:
            client.close()

    def click_watches(self, clip, watches):
        """What changed in the clip at each (from ms, to ms, x, y), as `putgd-record --click-check` reports it."""
        result = self.helper('--click-check', clip, *['%d,%d,%d,%d' % watch for watch in watches])
        try:
            return json.loads(result.stdout).get('watches') or []
        except ValueError:
            raise RunError('click-check: ' + (result.stdout + result.stderr).strip()[-300:])

    def click_check(self, clip, sent, start_call_s, canvas, name, highlights=None):
        """Rings in the clip against the presses sent: place and time, each measured from the pointer's own arrival in the clip."""
        a = self.args
        presses = [entry for entry in sent if 'error' not in entry]
        errors = [entry['error'] for entry in sent if 'error' in entry]
        if not presses:
            return {'ok': False, 'error': errors or 'no press was sent'}
        area = list(a.rect) if a.target == 'region' else list(self.cover_rect)
        # inspect's canvas is [width, height] in clip pixels.
        width = canvas[0] if isinstance(canvas, (list, tuple)) and canvas else (canvas or {}).get('width', area[2])
        scale = width / area[2]
        places = [(round((press['x'] - area[0]) * scale), round((press['y'] - area[1]) * scale)) for press in presses]
        check = {'highlights': a.click_highlights, 'startCallMs': round(start_call_s * 1000), 'presses': presses}
        if errors:
            check['sendErrors'] = errors
        try:
            # 1. When the pointer arrived at each place. The clip's zero lies somewhere inside the start call, so the
            #    span starts a second before the earliest moment the move can be at and ends just before the press.
            spans = [(max(0, int(press['movedS'] * 1000) - 1000), int((press['s'] + start_call_s) * 1000) - 300, *place) for press, place in zip(presses, places)]
            arrivals = self.click_watches(clip, spans)
            check['pointer'] = [{k: w.get(k) for k in ('seen', 'firstMs', 'peak')} for w in arrivals]
            # 2. What appears there afterwards, against a frame with the pointer already resting: the ring.
            rings = []
            for press, place, arrival in zip(presses, places, arrivals):
                if not arrival.get('seen'):
                    rings.append({'seen': False, 'reason': 'the pointer was not seen arriving'})
                    continue
                lead = (press['s'] - press['movedS']) * 1000
                ring = self.click_watches(clip, [(int(arrival['firstMs'] + 400), int(arrival['firstMs'] + lead + 1300), *place)])[0]
                if ring.get('seen'):
                    # How long after the press the ring showed: the press came `lead` after the move.
                    ring['delayMs'] = round(ring['firstMs'] - arrival['firstMs'] - lead)
                rings.append(ring)
        except RunError as error:
            check.update(ok=False, error=str(error))
            return check
        check['rings'] = rings
        seen = [(press, ring) for press, ring in zip(presses, rings) if ring.get('seen')]
        check.update(seen=len(seen), expected=len(presses))
        if a.click_highlights == 'off':
            check['ok'] = not seen and not errors
            return check
        problems = []
        if len(seen) != len(presses):
            problems.append('%d of %d presses have a ring' % (len(seen), len(presses)))
        # A ring put.gd draws is centred on the press. KWin's own effect also writes the button's name beside
        # its ring, so what changed there is centred somewhere inside the measured box, not on the press.
        reach = 48 if (highlights or {}).get('drawnBy') == 'compositor' else 4
        for press, ring in seen:
            if abs(ring['dx']) > reach or abs(ring['dy']) > reach:
                problems.append('ring %.1f,%.1f px from the press at %d,%d' % (ring['dx'], ring['dy'], press['x'], press['y']))
            # One frame early (the pointer's arrival is itself seen a frame late at worst) to a quarter of a second late.
            if not -70 <= ring['delayMs'] <= 250:
                problems.append('ring %d ms after the press at %.2f s' % (ring['delayMs'], press['s']))
        check['ok'] = not problems and not errors
        if problems:
            check['problems'] = problems
        if seen:
            frame = self.run / 'frames' / (name + '-click.png')
            if self.helper('--preview', clip, int(seen[0][1]['peakMs'] * 1000), frame).returncode == 0 and frame.is_file():
                check['frame'] = frame.name
        return check

    def tones(self, clip):
        result = self.helper('--av-sync', clip, '--tone', 440, '--tone', 1000)
        try:
            tones = json.loads(result.stdout).get('tones')
        except ValueError:
            tones = None
        # An analyzer without --tone prints usage; say so instead of leaving the entry empty.
        return tones if tones is not None else {'error': (result.stdout + result.stderr).strip()[-200:] or 'the analyzer reported no tones'}

    def av_sync(self, clip):
        result = self.helper('--av-sync', clip)
        try:
            return json.loads(result.stdout)
        except ValueError:
            return {'error': (result.stdout + result.stderr).strip()[-300:]}

    def motion(self, clip):
        result = self.helper('--motion', clip, 8)
        found = re.search(r'^motion=([0-9.]+)', result.stdout, re.M)
        if found:
            return float(found.group(1))
        if True:
            if clip.suffix == '.webp':
                try:
                    return pillow_motion(clip)
                except Exception as error:  # Pillow missing or unreadable file
                    return 'error: ' + type(error).__name__
            return 'error: ' + (result.stdout + result.stderr).strip()[-200:]

    def keep_frame(self, clip, name, duration_us):
        """One frame from the middle of a saved copy, kept only if check-frame finds it half cover colour."""
        frame = self.run / 'frames' / (name + '.png')
        if clip.suffix == '.webp':
            try:
                from PIL import Image
                image = Image.open(clip)
                image.seek(getattr(image, 'n_frames', 1) // 2)
                image.convert('RGB').save(frame)
            except Exception as error:
                return {'ok': False, 'reason': 'no frame: ' + type(error).__name__}
        elif self.helper('--preview', clip, int((duration_us or 0) / 2), frame).returncode != 0 or not frame.is_file():
            return {'ok': False, 'reason': 'no frame decoded'}
        checked = subprocess.run([sys.executable, str(HERE / 'cover_player.py'), 'check-frame', str(frame),
                                  '--min-fraction', '0.5', '--edges', ','.join(self.output_edges()) or 'none'],
                                 capture_output=True, text=True)
        return json.loads(checked.stdout or '{"ok": false}')

    def output_edges(self):
        """The recorded area's edges that lie on the cover's output edges (all of them for a monitor or window)."""
        return frame_edges(self.args.target, self.args.rect, self.cover_rect)

    def wait_state(self, build, rid, wanted, timeout):
        started = time.monotonic()
        said = False
        while time.monotonic() - started < timeout:
            info = self.cli(build, 'recording.inspect', {'id': rid})
            if (info.get('error') or {}).get('code'):
                raise RunError('recording error: %s' % json.dumps(info['error']))
            if info.get('state') in wanted:
                return info
            if not said and info.get('state') == 'preparing' and time.monotonic() - started > 5:
                log('prepare is waiting (a portal picker may be open on the desktop)')
                said = True
            time.sleep(0.25)
        raise RunError('timed out waiting for %s' % '/'.join(wanted))

    # -- cover ---------------------------------------------------------------
    def open_cover(self):
        a = self.args
        command = [sys.executable, str(HERE / 'cover_player.py'), 'play']
        if a.video == 'marker':
            marker = self.run / 'marker.mkv'
            if not marker.is_file():
                # With a microphone in the run every clip must show motion too: the marker gets a sweeping bar.
                extra = ['--moving'] if a.mic != 'off' or a.system in ('tone', 'leak') or a.marker_motion or a.click else []
                self.helper('--av-marker', marker, 60, *extra).check_returncode()
            command += ['--media', str(marker)]
        elif a.video == 'pattern':
            command += ['--pattern']
        if a.output:
            command += ['--output', a.output]
        if a.video_size:
            command += ['--video-size', a.video_size]
        if a.audio_device:
            command += ['--audio-device', a.audio_device]
        if a.click:
            # labwc reports where the pointer is only while it is shown; the cover otherwise hides it.
            command += ['--pointer']
        self.cover = subprocess.Popen(command, env=dict(self.env, GTK_USE_PORTAL='0', NO_AT_BRIDGE='1'),
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                      stderr=open(self.run / 'logs' / 'cover.log', 'a'), start_new_session=True)
        state = self.cover_line(20)
        if not state.get('ok') or state.get('state') != 'covered':
            raise RunError('cover not placed: %s' % json.dumps(state))
        self.cover_rect = state['rect']
        # --precheck-wait: time to dismiss a banner the desktop put over the cover before it is checked.
        time.sleep(max(1.5, a.precheck_wait))
        if a.vnc_port == 0:
            # A disposable lab guest shows no one's windows and has no loopback tile; the frame gate still applies.
            report = {'skipped': 'no loopback VNC tile (disposable guest)'}
        else:
            precheck = [sys.executable, str(HERE / 'cover_player.py'), 'precheck', '--port', str(a.vnc_port)]
            if a.output:
                precheck += ['--output', a.output]
            checked = subprocess.run(precheck, env=self.env, capture_output=True, text=True, timeout=60)
            report = json.loads(checked.stdout or '{}')
            if checked.returncode != 0:
                raise RunError('cover precheck failed, nothing recorded: %s' % json.dumps(report))
        if a.video != 'still':
            self.cover.stdin.write('media\n'); self.cover.stdin.flush()
            playing = self.cover_line(30)
            if playing.get('state') != 'playing':
                raise RunError('video not playing: %s' % json.dumps(playing))
        return {'placed': state, 'precheck': report, 'video': a.video}

    def cover_line(self, timeout):
        selector = selectors.DefaultSelector()
        selector.register(self.cover.stdout, selectors.EVENT_READ)
        if not selector.select(timeout):
            raise RunError('cover player did not answer within %d s' % timeout)
        line = self.cover.stdout.readline()
        return json.loads(line) if line.strip() else {'ok': False, 'reason': 'cover exited %s' % self.cover.poll()}

    def close_cover(self):
        try:
            if self.cover and self.cover.poll() is None:
                try:
                    self.cover.stdin.write('quit\n'); self.cover.stdin.flush()
                    self.cover.wait(10)
                except (OSError, subprocess.TimeoutExpired):
                    os.killpg(self.cover.pid, signal.SIGTERM)
                    self.cover.wait(10)
        finally:
            # Hyprland's own setting, changed for a window run, goes back whatever happened to the cover.
            if self.hyprland_focus_before is not None:
                self.set_hyprland_focus_option(self.hyprland_focus_before)
                self.hyprland_focus_before = None

    # -- resident ------------------------------------------------------------
    def start_resident(self, build):
        return lab_resident.start_resident(self, build, no_notifications=self.args.no_notifications)

    def stop_resident(self, build):
        lab_resident.stop_resident(self, build)

    # -- one recording -------------------------------------------------------
    def record(self, build, number, fmt):
        a = self.args
        name = '%s-%s-%d' % (build.label, fmt, number)
        result = {'build': build.label, 'round': number, 'format': fmt,
                  'started': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
        if a.unplug_at is not None and a.mic != 'late' and self.mic_state and json.loads(self.mic_state.read_text()).get('plugged') is False:
            # An earlier recording of this run pulled the simulated microphone; this one starts with it again
            # (the same `plug` a --mic late run uses, with its sound check).
            self.virtual_mic('plug', '--state', str(self.mic_state), *(['--default'] if a.mic_default else []))
            result['microphonePluggedInAgain'] = True
        caps = self.start_resident(build)
        copy = self.run / 'out' / ('%s.%s' % (name, fmt))
        try:
            if a.delayed_capture:
                result['delayedCaptures'] = [self.delayed_capture(build, name, 'before')]
            result['acquisition'] = caps.get('acquisition')
            # The first usable live encoder is the one the recording uses.
            result['liveEncoders'] = (caps.get('gstreamer') or {}).get('usableH264Encoders')
            target = {'kind': a.target}
            if a.identity:
                target['identity'] = a.identity
            elif a.target == 'window' and self.env.get('XDG_SESSION_TYPE') == 'x11':
                # X11 names a window by its id; the cover is found by its process, not by the focus.
                target['identity'] = self.cover_x11_window()
            if a.rect:
                target['rect'] = a.rect
            sound = fmt not in ANIMATIONS
            audio = {'system': sound and a.system not in ('off', 'leak')}
            if a.mic not in ('off', 'muted', 'late') and sound:
                audio['microphone'] = True
                if self.microphone_device():
                    audio['microphoneDevice'] = self.microphone_device()
            result['audio'] = audio
            caps_audio = caps.get('audio') or {}
            result['capabilityAudio'] = {k: caps_audio.get(k) for k in ('system', 'microphone', 'microphoneReason', 'microphoneDevice') if k in caps_audio}
            if a.mic not in ('off', 'muted'):
                listed = self.cli(build, 'recording.microphones', {'device': audio['microphoneDevice']} if audio.get('microphoneDevice') else {})
                result['microphones'] = {'available': listed.get('available'), 'reason': listed.get('reason'), 'device': listed.get('device'),
                                         'devices': [d.get('id') for d in listed.get('devices') or []], 'defaultIsMonitor': listed.get('defaultIsMonitor')}
            request = {'target': target, 'format': fmt, 'fps': 30, 'audio': audio}
            if a.click:
                result['clickHighlights'] = caps.get('clickHighlights')
                if a.click_highlights == 'on':
                    request['clickHighlights'] = True
            prepared = self.cli(build, 'recording.prepare', request)
            rid = prepared['recordingId']
            ready = self.wait_state(build, rid, ('ready',), a.prepare_timeout)
            result['canvas'] = ready.get('canvas')
            if a.mic == 'late':
                result['readyFollow'] = self.follow_at_ready(build, rid, name)
            if a.hide_bar:
                # Where the bar cannot sit outside the recorded area (a whole monitor) its Stop handle is in the
                # clip, and the frame gate cannot tell it from a desktop's own panel. Hidden, it stays hidden.
                self.cli(build, 'recording.overlay', {'id': rid, 'visible': False})
                result['barHidden'] = True
            time.sleep(a.lead)
            asked = time.monotonic()
            self.cli(build, 'recording.start', {'id': rid})
            started = time.monotonic()
            sent, clicker = [], None
            if a.click:
                clicker = threading.Thread(target=self.press_clicks, args=(started, sent), daemon=True)
                clicker.start()
            time.sleep(0.5)
            # Where the control bar went once recording: outside the recorded pixels, or a handle inside them.
            placed = self.cli(build, 'recording.inspect', {'id': rid})
            result['overlay'] = {'excluded': placed.get('overlayExcluded'), 'placement': placed.get('overlayPlacement')}
            result['progressAtStart'] = {k: (placed.get('progress') or {}).get(k) for k in ('audioSources', 'audioEncoder', 'recordingMicrophone', 'microphoneLost')}
            if a.unplug_at is not None and self.mic_state:
                time.sleep(max(0, a.unplug_at - 0.5))
                self.virtual_mic('unplug', '--state', str(self.mic_state))
                result['unpluggedAtS'] = round(time.monotonic() - started, 3)
                time.sleep(2.5)
                after = self.cli(build, 'recording.inspect', {'id': rid})
                result['afterUnplug'] = {'state': after.get('state'), 'microphoneLost': (after.get('progress') or {}).get('microphoneLost')}
                time.sleep(max(0, a.pause_at - a.unplug_at - 2.5))
            elif a.output_gone_at is not None and self.mic_state:
                time.sleep(max(0, a.output_gone_at - 0.5))
                if not a.output_fallback:
                    # With nothing to move to, nothing of the lab's may go looking for another output either.
                    self.stop_tone()
                gone = self.virtual_mic('unplug-output', '--state', str(self.mic_state), *(['--fallback'] if a.output_fallback else []))
                result['outputGoneAtS'] = round(time.monotonic() - started, 3)
                result['outputGone'] = {k: gone.get(k) for k in ('fallback', 'defaultSink', 'defaultSource', 'sinks')}
                time.sleep(2.5)
                after = self.cli(build, 'recording.inspect', {'id': rid})
                progress = after.get('progress') or {}
                result['afterOutputGone'] = {'state': after.get('state'), 'error': after.get('error'), 'durationUs': progress.get('durationUs'),
                                             'systemAudioLost': progress.get('systemAudioLost'), 'recordedFrom': self.recording_sources()}
                if after.get('state') != 'recording':
                    raise RunError('the recording is %s after its output went: %s' % (after.get('state'), json.dumps(after.get('error'))))
                time.sleep(max(0, a.pause_at - a.output_gone_at - 2.5))
            else:
                time.sleep(max(0, a.pause_at - 0.5))
            if a.pause_for > 0:
                self.cli(build, 'recording.pause', {'id': rid})
                result['pauseClipS'] = round(time.monotonic() - started, 3)
                time.sleep(a.pause_for)
                self.cli(build, 'recording.resume', {'id': rid})
            time.sleep(a.after)
            if clicker:
                clicker.join(30)
                result['clickHighlightsError'] = (self.cli(build, 'recording.inspect', {'id': rid}).get('progress') or {}).get('clickHighlightsError')
            # The recording clock's share of the time since Record: what the clip must hold.
            result['recordedS'] = round(time.monotonic() - started - max(0, a.pause_for), 3)
            result['microphoneLostAtStop'] = (self.cli(build, 'recording.inspect', {'id': rid}).get('progress') or {}).get('microphoneLost')
            try:
                stopped = self.cli(build, 'recording.stop', {'id': rid}, timeout=60)
            except RunError as error:
                # The recorder ends a clip itself when its byte budget is spent (low disk); keep and measure it.
                stopped = self.cli(build, 'recording.inspect', {'id': rid})
                if stopped.get('state') != 'reviewing':
                    raise
                result['stoppedEarly'] = str(error)
            result.update(load=os.getloadavg()[0], durationUs=stopped.get('durationUs'))
            self.cli(build, 'recording.saveCopy', {'id': rid, 'path': str(copy), 'overwrite': True})
            done = self.wait_state(build, rid, ('reviewing',), 600)
            result['export'] = (done.get('lastExport') or {}).get('status')
            result['overlay'] = {key: done[key] for key in ('overlayExcluded', 'overlayPlacement') if key in done}
            self.cli(build, 'recording.cancel', {'id': rid})
            if a.delayed_capture:
                result['delayedCaptures'].append(self.delayed_capture(build, name, 'after'))
        finally:
            self.stop_resident(build)
            if a.mic == 'late' and self.mic_state and json.loads(self.mic_state.read_text()).get('plugged'):
                # The next round starts without a microphone again.
                self.virtual_mic('unplug', '--state', str(self.mic_state))
        working = self.run / 'working' / (rid + '.mkv')
        result['clips'] = {}
        for kind, clip in (('working', working), ('export', copy)):
            entry = {'file': clip.name}
            if not clip.is_file():
                entry['error'] = 'missing'
            else:
                entry.update(bytes=clip.stat().st_size, motion=self.motion(clip))
                if sound and (a.mic != 'off' or a.system in ('tone', 'leak')):
                    entry['tones'] = self.tones(clip)
                if a.video == 'marker' and sound:
                    measured = self.av_sync(clip)
                    entry.update({k: measured[k] for k in ('pairs', 'medianMs', 'spreadMs', 'offsetsMs', 'pairTimesS', 'error')
                                  if k in measured})
                    # With every audio source off the marker cannot pair by design: that clip is kept.
                    silent = a.system == 'off' and a.mic in ('off', 'muted')
                    if measured.get('pairs', 0) < a.min_pairs and not a.keep_unpaired and not silent:
                        clip.unlink()  # no marker: the clip may show something else
                        entry['destroyed'] = True
            result['clips'][kind] = entry
        if a.click:
            for kind, clip in (('working', working), ('export', copy)):
                if clip.is_file():
                    result['clips'][kind]['clickCheck'] = self.click_check(clip, sent, started - asked, result.get('canvas'), '%s-%s' % (name, kind), result.get('clickHighlights'))
        w, e = result['clips']['working'], result['clips']['export']
        if 'pauseClipS' in result and w.get('pairTimesS'):
            result['captureStepMs'] = capture_step(w['pairTimesS'], w['offsetsMs'], result['pauseClipS'])
            result['rawCaptureStepMs'] = capture_step(w['pairTimesS'], w['offsetsMs'], result['pauseClipS'], raw=True)
            if e.get('pairTimesS'):
                result['exportMinusWorkingMs'] = export_shift(w, e, result['pauseClipS'])
        problems = self.clip_problems(result, sound)
        problems += [problem for capture in result.get('delayedCaptures') or [] for problem in capture.pop('problems')]
        if problems:
            result['problems'] = problems
            log('problems: %s' % '; '.join(problems))
        if a.frames and copy.is_file():
            result['frame'] = self.keep_frame(copy, name, result.get('durationUs'))
            if not result['frame'].get('ok'):
                # The cover didn't fill the recording: its clips may show something else.
                for kind, clip in (('working', working), ('export', copy)):
                    clip.unlink(missing_ok=True)
                    result['clips'][kind]['destroyed'] = True
                problem = "the clip's frame is not the cover (%s); the clips were deleted" % result['frame'].get('reason')
                if (result.get('overlay') or {}).get('overlayPlacement') == 'handle' and not a.hide_bar:
                    problem += ". put.gd's own Stop handle is in this clip (overlayPlacement handle): --hide-bar records without it"
                result.setdefault('problems', []).append(problem)
                log('problems: %s' % problem)
        return result

    def hyprctl(self, *arguments):
        return subprocess.run(['hyprctl', *arguments], env=self.env, capture_output=True, text=True).stdout.strip()

    def hyprland_focus_option(self):
        try:
            return json.loads(self.hyprctl('getoption', 'misc:on_focus_under_fullscreen', '-j')).get('int')
        except ValueError:
            return None

    def set_hyprland_focus_option(self, value):
        # Hyprland with a Lua configuration takes it through `eval`; older ones through `keyword`.
        if self.hyprctl('eval', 'hl.config({ misc = { on_focus_under_fullscreen = %d } })' % value) != 'ok':
            self.hyprctl('keyword', 'misc:on_focus_under_fullscreen', str(value))
        return self.hyprland_focus_option()

    def keep_cover_fullscreen_on_hyprland(self):
        """For a window recording on Hyprland: a window that opens no longer takes the cover's full-screen state.

        With `misc:on_focus_under_fullscreen` at 1 (Omarchy's value) put.gd's recording controls take the
        full-screen state when they open and the cover is tiled out of sight beside them: the presses would
        land on the controls, outside the recorded window, and rightly draw nothing. 0 leaves the cover as it
        is; the previous value is put back when the cover closes."""
        before = self.hyprland_focus_option()
        if before is None:
            raise RunError('Hyprland did not report misc:on_focus_under_fullscreen')
        during = self.set_hyprland_focus_option(0)
        if during != 0:
            raise RunError('misc:on_focus_under_fullscreen stayed at %s' % during)
        self.hyprland_focus_before = before
        return {'onFocusUnderFullscreen': {'before': before, 'during': during}}

    def desktop_frame(self, path):
        """One frame of the desktop through the tile the run's presses and cover check use; False where there is none."""
        a = self.args
        if a.click_vnc:
            host, _, port = a.click_vnc.rpartition(':')
        elif a.vnc_port:
            host, port = '127.0.0.1', a.vnc_port
        else:
            return False
        import rfb
        client = rfb.RFB(int(port), host=host or '127.0.0.1')
        try:
            client.frame()
            time.sleep(0.3)
            client.screenshot(str(path))
        finally:
            client.close()
        return True

    def delayed_capture(self, build, name, when):
        """A delayed capture in the resident session the round's recording uses (#1363 beside #1364).

        `action.invoke {action, delaySeconds}` answers at once, the countdown runs for that long and the capture is
        saved to the run's private archive. It must end `completed` no sooner than its delay, with one saved image
        that is at least half cover colour (otherwise the image is deleted unopened). With --frames one frame of
        the desktop is kept from the countdown (RUN/frames/NAME-countdown-WHEN.png) and one from after it, and
        `countdownPixels` counts what differs between them in the top-centre band where the pill sits."""
        action, _, seconds = self.args.delayed_capture.partition(':')
        seconds = int(seconds)
        entry = {'when': when, 'action': action, 'delaySeconds': seconds, 'problems': []}
        began = time.monotonic()
        accepted = self.cli(build, 'action.invoke', {'action': action, 'delaySeconds': seconds})
        operation = accepted.get('operationId') or (accepted.get('operation') or {}).get('id')
        if not operation:
            raise RunError('action.invoke gave no operation: %s' % json.dumps(accepted)[:300])
        entry['acceptedAfterS'] = round(time.monotonic() - began, 2)
        frames = self.run / 'frames'
        during = frames / ('%s-countdown-%s.png' % (name, when))
        if self.args.frames:
            time.sleep(1.0)
            entry['countdownFrame'] = during.name if self.desktop_frame(during) else None
        info = {}
        while time.monotonic() - began < seconds + 45:
            info = self.cli(build, 'operation.inspect', {'id': operation})
            # `stored` is on the way: the capture is acquired and not yet saved.
            if info.get('state') in ('completed', 'partial', 'failed', 'cancelled', 'interrupted'):
                break
            time.sleep(0.25)
        entry['tookS'] = round(time.monotonic() - began, 2)
        entry['state'] = info.get('state')
        items = info.get('items') or [{}]
        error = info.get('error') or items[0].get('error') or {}
        if error.get('code'):
            entry['error'] = {'code': error.get('code'), 'message': error.get('message')}
        if entry['state'] != 'completed':
            entry['problems'].append('the delayed %s capture %s the recording ended %s (%s)' % (action, when, entry['state'], error.get('code')))
            return entry
        if entry['tookS'] < seconds:
            entry['problems'].append('the delayed %s capture %s the recording took %.1f s of a %d s delay' % (action, when, entry['tookS'], seconds))
        local = items[0].get('local') or {}
        saved = Path(local.get('path') or '')
        entry['local'] = {'status': local.get('status'), 'file': saved.name}
        if local.get('status') != 'saved' or not saved.is_file():
            entry['problems'].append('the delayed capture %s the recording was not saved' % when)
            return entry
        check = ['check-frame', str(saved), '--min-fraction', '0.5']
        with open(saved, 'rb') as image:
            header = image.read(24)
        size = [int.from_bytes(header[16:20], 'big'), int.from_bytes(header[20:24], 'big')]
        entry['size'] = size
        if self.cover_rect and size != list(self.cover_rect[2:]):
            # A desktop of several outputs: only the cover's own output is judged.
            check += ['--rect', ','.join(str(v) for v in self.cover_rect), '--edges', 'none']
        checked = subprocess.run([sys.executable, str(HERE / 'cover_player.py'), *check], capture_output=True, text=True)
        entry['cover'] = json.loads(checked.stdout or '{"ok": false}')
        if not entry['cover'].get('ok'):
            # check-frame deletes what it does not pass.
            saved.unlink(missing_ok=True)
            entry['local']['file'] = None
            entry['problems'].append('the delayed capture %s the recording is not of the cover (%s); deleted unopened' % (when, entry['cover'].get('reason')))
        if self.args.frames and entry.get('countdownFrame'):
            after = frames / ('%s-countdown-%s-after.png' % (name, when))
            time.sleep(0.5)
            if self.desktop_frame(after):
                entry['afterFrame'] = after.name
                entry['countdownPixels'] = self.pill_pixels(during, after)
                if entry['countdownPixels'] == 0:
                    entry['problems'].append('no countdown was on screen during the delay %s the recording' % when)
        return entry

    def pill_pixels(self, during, after):
        """How many pixels of the top-centre band of the cover's output differ between two desktop frames; None without Pillow."""
        try:
            from PIL import Image, ImageChops
        except ImportError:
            return None
        first, second = Image.open(during).convert('RGB'), Image.open(after).convert('RGB')
        if first.size != second.size:
            return None
        x, y, w, h = self.cover_rect or (0, 0, *first.size)
        band = (x + w // 4, y, x + w - w // 4, y + h // 7)
        difference = ImageChops.difference(first.crop(band), second.crop(band)).convert('L').point(lambda v: 255 if v > 48 else 0)
        return sum(1 for v in difference.getdata() if v)

    def follow_at_ready(self, build, rid, name):
        """At Ready: a microphone is plugged in, switched on, pulled and plugged in again; the bar must follow each.

        The session was prepared with no microphone to open. `recording.inspect` must say so, then show the one
        that was plugged in within a few seconds, take `recording.audio {microphone: true}` without a new prepare,
        drop it when it is pulled while keeping that it was asked for, and record from it again, unasked, once it
        is back. With --frames a frame of the desktop is kept at each step (RUN/frames/NAME-ready-STEP.png)."""
        steps = []

        def microphone():
            return (self.cli(build, 'recording.inspect', {'id': rid}).get('progress') or {}).get('recordingMicrophone') or {}

        def wait(want, what, timeout=8):
            began = time.monotonic()
            while time.monotonic() - began < timeout:
                state = microphone()
                if want(state):
                    return state, round(time.monotonic() - began, 2)
                time.sleep(0.2)
            raise RunError('at Ready the bar did not show %s within %d s: %s' % (what, timeout, json.dumps(microphone())))

        def step(label, state, waited=None):
            entry = {'step': label, 'microphone': state, 'waitedS': waited}
            if self.args.frames:
                # The bar has drawn the new state by the time the next frame is on the tile.
                time.sleep(0.6)
                frame = self.run / 'frames' / ('%s-ready-%s.png' % (name, label))
                frame.parent.mkdir(exist_ok=True)
                entry['frame'] = frame.name if self.desktop_frame(frame) else None
            steps.append(entry)

        state = microphone()
        if state.get('available'):
            raise RunError('a microphone is available before one was plugged in: %s' % json.dumps(state))
        step('none', state)
        self.virtual_mic('plug', '--state', str(self.mic_state), '--default')
        state, waited = wait(lambda m: m.get('available') and not m.get('effective'), 'the microphone that was plugged in')
        step('plugged', state, waited)
        self.cli(build, 'recording.audio', {'id': rid, 'microphone': True})
        state, waited = wait(lambda m: m.get('effective'), 'the microphone switched on')
        step('on', state, waited)
        self.virtual_mic('unplug', '--state', str(self.mic_state))
        state, waited = wait(lambda m: not m.get('available') and not m.get('effective') and m.get('requested'), 'that the microphone is gone')
        step('pulled', state, waited)
        self.virtual_mic('plug', '--state', str(self.mic_state), '--default')
        state, waited = wait(lambda m: m.get('available') and m.get('effective'), 'the microphone back, and on without being asked again')
        step('back', state, waited)
        config = (self.cli(build, 'recording.inspect', {'id': rid}).get('config') or {}).get('audio') or {}
        if not config.get('microphone'):
            raise RunError('the session does not record the microphone that is back: %s' % json.dumps(config))
        return steps

    def cover_x11_window(self):
        """The cover's X window id, as `recording.prepare` takes it for a window target on X11."""
        listed = subprocess.run(['xprop', '-root', '_NET_CLIENT_LIST'], env=self.env, capture_output=True, text=True).stdout
        for ident in re.findall(r'0x[0-9a-fA-F]+', listed.partition('#')[2]):
            owner = re.search(r'= (\d+)', subprocess.run(['xprop', '-id', ident, '_NET_WM_PID'], env=self.env, capture_output=True, text=True).stdout)
            if owner and self.cover and int(owner.group(1)) == self.cover.pid:
                return ident
        raise RunError("the cover's window is not in the window manager's list: %s" % (listed.strip() or 'xprop printed nothing'))

    def clip_problems(self, result, sound):
        """What a finished round's clip gets wrong that no offset shows: too short, or the wrong sound in it."""
        a = self.args
        problems = []
        clip_s = (result.get('durationUs') or 0) / 1e6
        # A clip much shorter than the time recorded stopped taking frames part-way (#1364: a clock that
        # stopped with the microphone). A clip the recorder ended itself (low disk) is short by design.
        if not result.get('stoppedEarly') and clip_s < result.get('recordedS', 0) - 1.5:
            problems.append('the clip holds %.1f s of the %.1f s recorded' % (clip_s, result['recordedS']))
        if not sound:
            return problems
        tones = (result['clips'].get('working') or {}).get('tones') or {}

        def peak(hz):
            band = tones.get(str(hz))
            return band.get('maxAmplitude') or 0 if isinstance(band, dict) else None

        marker, tone = peak(1000), peak(440)
        if a.video == 'marker' and marker is not None:
            if a.mic in ('virtual', 'late') and marker < 0.05:
                problems.append("the simulated microphone's bursts are not in the clip (1 kHz peak %.4f)" % marker)
            if a.mic == 'muted' and marker > 0.01:
                problems.append('the microphone is off and its bursts are in the clip (1 kHz peak %.4f)' % marker)
        # The quiet tone is recorded at about 0.017; nothing near that may come from an output that is not recorded.
        if a.system == 'leak' and tone is not None and tone > 0.0085:
            problems.append("an output's sound is in the clip (440 Hz peak %.4f)" % tone)
        if a.output_gone_at is not None and (result.get('afterOutputGone') or {}).get('state') != 'recording':
            problems.append('the recording did not go on after its output went')
        if a.unplug_at is not None and self.mic_state:
            if not (result.get('afterUnplug') or {}).get('microphoneLost'):
                problems.append('the recording did not say that the microphone was lost')
        elif result.get('microphoneLostAtStop'):
            problems.append('the recording says the microphone was lost, and nothing removed it')
        return problems

    def gpu_hangs(self, since):
        out = subprocess.run(['journalctl', '-k', '--since', since, '--no-pager', '-o', 'short-iso'], capture_output=True,
                             text=True).stdout
        return [line for line in out.splitlines() if 'GPU HANG' in line or 'context reset due to GPU hang' in line]

    def main(self):
        a = self.args
        since = time.strftime('%Y-%m-%d %H:%M:%S')
        builds = [Build(*spec.split('=', 1), self.run) for spec in a.build]
        report = {'host': os.uname().nodename, 'kernel': os.uname().release, 'desktop': self.desktop,
                  'session': self.env.get('XDG_SESSION_TYPE'), 'target': a.target, 'rect': a.rect, 'formats': a.format,
                  'video': a.video, 'timing': {'lead': a.lead, 'pauseAt': a.pause_at, 'pauseFor': a.pause_for, 'after': a.after},
                  'builds': {b.label: b.identity for b in builds}, 'analyzer': str(self.analyzer), 'rounds': []}
        out = self.run / 'results.json'
        try:
            # A resident's first start with a fresh configuration can open a window; let that happen before the cover is up.
            for build in builds:
                self.start_resident(build)
                # Quit at once and the window is cut off while it opens: put.gd then leaves a notification
                # ("The window could not be opened") that stays on screen, over the cover.
                time.sleep(3)
                self.stop_resident(build)
            report['microphone'] = self.open_microphone()
            report['cover'] = self.open_cover()
            # A window run, and a run whose second delayed capture comes after the review window has opened.
            if (a.target == 'window' or a.delayed_capture) and self.env.get('HYPRLAND_INSTANCE_SIGNATURE'):
                report['cover']['hyprland'] = self.keep_cover_fullscreen_on_hyprland()
            for number in range(1, a.rounds + 1):
                for fmt in a.format:
                    for build in builds:
                        log('round %d %s %s' % (number, fmt, build.label))
                        try:
                            entry = self.record(build, number, fmt)
                        except (RunError, subprocess.TimeoutExpired) as error:
                            entry = {'build': build.label, 'round': number, 'format': fmt, 'error': str(error)}
                            log('failed: %s' % error)
                        report['rounds'].append(entry)
                        out.write_text(json.dumps(report, indent=1) + '\n')
                        summary = {k: entry.get(k) for k in ('build', 'format', 'round', 'acquisition', 'liveEncoders',
                                                             'captureStepMs', 'exportMinusWorkingMs', 'stoppedEarly', 'error', 'load')}
                        summary['motion'] = [c.get('motion') for c in entry.get('clips', {}).values()]
                        summary['medianMs'] = [c.get('medianMs') for c in entry.get('clips', {}).values()]
                        log(json.dumps(summary))
        except RunError as error:
            report['error'] = str(error)
            log('stopped: %s' % error)
        finally:
            self.close_cover()
            try:
                report['microphoneRemoved'] = self.close_microphone()
            except RunError as error:
                report['microphoneRemoved'] = {'error': str(error)}
            if self.real_data is not None and self.real_data.exists():
                self.real_data.rename(self.run / 'real-data-putgd')
            report['gpuHangs'] = self.gpu_hangs(since)
            out.write_text(json.dumps(report, indent=1) + '\n')
        return 1 if report.get('error') or any(entry.get('problems') for entry in report['rounds']) else 0


def parse_rect(text):
    parts = [int(p) for p in text.split(',')]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError('rectangle must be X,Y,W,H')
    return parts


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--run', required=True)
    parser.add_argument('--build', action='append', required=True, help='LABEL=APPIMAGE or LABEL=BINDIR')
    parser.add_argument('--analyzer', required=True, help='extracted AppImage with a putgd-record that reports pairTimesS')
    parser.add_argument('--video', choices=('marker', 'pattern', 'still'), default='marker')
    parser.add_argument('--target', choices=('monitor', 'region', 'window'), default='monitor')
    parser.add_argument('--rect', type=parse_rect)
    parser.add_argument('--identity')
    parser.add_argument('--format', nargs='+', choices=('mp4', 'webm', 'gif', 'webp'), default=['mp4'])
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--output')
    parser.add_argument('--video-size', help="the video's size on the cover (cover_player's default 1280x720); keep it under half the output")
    parser.add_argument('--audio-device')
    parser.add_argument('--vnc-port', type=int, default=5900, help='0 skips the precheck: disposable lab guests only')
    parser.add_argument('--lead', type=float, default=2)
    parser.add_argument('--pause-at', type=float, default=8)
    parser.add_argument('--pause-for', type=float, default=3)
    parser.add_argument('--after', type=float, default=8)
    parser.add_argument('--prepare-timeout', type=float, default=120)
    parser.add_argument('--min-pairs', type=int, default=8)
    parser.add_argument('--frames', action='store_true', help='keep one cover-checked frame of each saved copy')
    parser.add_argument('--mic', default='off', help="off, virtual (a simulated microphone made for the run), muted (simulated and playing, not recorded), late (simulated, plugged in while the bar is at Ready), default, or a source name")
    parser.add_argument('--system', choices=('on', 'off', 'tone', 'leak'), default='on',
                        help="system audio: recorded, not recorded, a quiet 440 Hz tone on a private default output, or 'leak': that tone playing but not recorded, with the output's monitor as the default input (the clip must hold none of it)")
    parser.add_argument('--unplug-at', type=float, help='remove the simulated microphone this many seconds into each recording')
    parser.add_argument('--output-gone-at', type=float, help='with --system tone: remove the private output this many seconds into each recording')
    parser.add_argument('--output-fallback', action='store_true', help='with --output-gone-at: make another private output the default first')
    parser.add_argument('--mic-default', action='store_true', help='make the simulated microphone the default input (a desktop with a microphone)')
    parser.add_argument('--cards-off', action='store_true', help="turn the sound cards' profiles off for the run and put them back (for --output-gone-at without a fallback)")
    parser.add_argument('--precheck-wait', type=float, default=1.5, help='seconds between placing the cover and checking it')
    parser.add_argument('--no-notifications', action='store_true', help="turn the run's own put.gd notifications off (private config), so none covers the cover")
    parser.add_argument('--marker-motion', action='store_true', help='add a sweeping bar to the marker clip so marker recordings also score motion')
    parser.add_argument('--click', action='append', default=[], metavar='S,X,Y[,BUTTON]',
                        help='press BUTTON (left, middle, right) at desktop pixel X,Y, S seconds into each recording, and check the clip for its ring')
    parser.add_argument('--click-highlights', choices=('on', 'off'), default='on', help='off: record the presses without click highlights; no ring may appear')
    parser.add_argument('--click-vnc', metavar='HOST:PORT', help="the VNC server that shows this desktop (default: this host's --vnc-port)")
    parser.add_argument('--hide-bar', action='store_true',
                        help="hide put.gd's recording bar before Record (recording.overlay), for a whole-monitor clip without its Stop handle")
    parser.add_argument('--delayed-capture', metavar='ACTION:SECONDS', type=parse_delayed_capture,
                        help='in each round, in the resident session that records: a delayed capture (desktop or monitor; 3, 5 or 10 s) before the recording and again after it')
    parser.add_argument('--keep-unpaired', action='store_true', help="keep clips without marker pairs (runs where the microphone is off or unplugged)")
    parser.add_argument('--real-data-home', action='store_true',
                        help="use the user's data directory (an AppImage's KWin screencast entry); moved into --run afterwards")
    args = parser.parse_args(argv)
    if args.output_gone_at is not None and args.rounds * len(args.format) * len(args.build) != 1:
        # The private output is not made again, so a second recording would have nothing to record from.
        parser.error('--output-gone-at removes the run\'s private output for good: one --build, one --format and --rounds 1 per run')
    return Driver(args).main()


if __name__ == '__main__':
    sys.exit(main())
