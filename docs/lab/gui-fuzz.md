# On-screen GUI fuzzer

`tools/lab/gui_fuzz/` drives put.gd's real windows on a real desktop session with kernel input and a seeded random
walk, and watches for crashes, hangs, grabs left behind and inconsistent state that offscreen tests cannot reach.
It runs inside a lab guest as the guest user, with Python 3.12+ and `dbus-python`/`PyGObject`, and has a backend for
each of three desktops:

| Desktop | Guest | Fence source |
| --- | --- | --- |
| KDE Plasma (Wayland) | [Aurora Plasma 6.8 golden](aurora-vm.md#plasma-68-golden) | a KWin script |
| GNOME 50 (Wayland) | `gnome-provisioned-20261016` (Ubuntu 26.04) | a test-only Shell extension |
| Cinnamon (X11) | `cinnamon-x11` (Mint 22.3, Cinnamon 6.6) | the X server's window tree, through `python-xlib` |

It is a lab tool, not acceptance evidence for any desktop.

## Parts

| File | Job |
| --- | --- |
| `desktop.py` | What differs per desktop: the fence, the session processes, pointer acceleration, monitor changes, the clipboard, the portal's file chooser class and how to close a window put.gd did not open. `--desktop kde\|gnome\|cinnamon`, default from `XDG_CURRENT_DESKTOP`. Nothing else in the package names a compositor. |
| `uinput.py` | A virtual keyboard and a relative mouse through `/dev/uinput` (the guest needs it writable). The mouse is flattened so one unit is one pixel: KWin's flat acceleration profile, or `gsettings` `accel-profile` on GNOME and Cinnamon. |
| `fence.py` | KDE: a persistent KWin script that reports every window, popup, layer surface, output and the cursor 25 to 100 times a second. KWin's JS has no timer, so the script calls back over D-Bus and the receiver answers after 35 ms. |
| `fence_gnome.py`, `witness.js`, `shell-extension/` | GNOME: put.gd's region selector, recording frame and countdown are St actors in the Shell, not windows, so the fence reads Mutter's side. `shell-extension/guifuzz@put.gd.test` exports `org.putgd.GuiFuzz.Eval` on the Shell's connection; the fence sends `witness.js` through it once and then polls it about 30 times a second for the Meta windows (stacking order), the Shell's chrome (panel, dock, message banner, overview, modal grab count), put.gd's actors, the monitors and the pointer. Changing `witness.js` needs no new login. |
| `fence_x11.py` | Cinnamon: `_NET_CLIENT_LIST_STACKING`, `_NET_FRAME_EXTENTS`, the override-redirect windows that Qt's menus, tooltips and region selector are, RandR monitors and `QueryPointer`. Cinnamon's panel and notification bubbles are Clutter actors and invisible to X. |
| `world.py` | put.gd's processes and windows, and the fence: the topmost window under a point must be put.gd's own (or its portal file chooser), unless the region selector covers the outputs. |
| `actor.py` | Every input primitive, fenced and logged. A pointer jump that the compositor clamps never clicks. Keys come from an allow-list: no Meta, no Alt+Tab, no Alt+F4, no Ctrl+Alt chords (so no VT switch), no function keys but put.gd's own hotkeys, no Ctrl+Q. |
| `walk.py` | The walk: hotkeys, the tray menu through its dbusmenu, selector drags, clicks, drags, wheel, key chords, held and repeated keys, text, window resizes and moves, monitor hotplug and mode or scale changes, clipboard churn, rapid repeats and `putgd-cli` session methods. Odd seeds run in chaos mode (monitor churn and rapid repeats weighted up). |
| `watch.py` | The watchdog and the findings. |
| `ring.py` | On the QEMU host: a rolling QMP screen record, and `save` to cut the minute around a finding. |

## Findings

A resident crash, a put.gd core dump, a session method that does not answer in 5 s, a region selector or popup still
up after Escape and the cancel of any recording, scroll session or edit, put.gd's tray lost after a shell restart,
`history.counts` against `history.list`, settings revisions, operations stuck in a running state, Qt critical lines
in the resident's log, and focus on a window that is not put.gd's. Each writes `findings/NNN-kind.json` with the last
60 actions, the windows, process sizes, core dumps and the journal tail, and a line in `marks.log` with the time, to
cut the screen record. A fatal finding (resident, the shell or session gone, VT change) stops the walk.

GNOME adds three: a gnome-shell journal line that names put.gd or its extension and reports a script error
(`shell_js_error`), `Main.modalCount` above zero for 8 s with no put.gd overlay up (`shell_modal_stuck`), and a
Shell crash, which ends the session and turns user extensions off; `session_died` carries the put.gd lines of the
journal and the core dump, so a crash that put.gd caused can be told from Mutter's own. `memory.jsonl` also samples
the shell's size, to show a leak in put.gd's extension.

A core dump of a desktop process (`nemo-desktop`, `plasmashell`) is recorded as `desktop_core_dump_<name>`, not as a
put.gd finding. When the focus sits on an app the desktop started for one of put.gd's Open actions (a file with no
handler opens the app store, a link a browser), the watchdog closes it as its title bar would and counts it in
`foreign_windows_closed`; the walk cannot click it.

Every input is logged to `actions.jsonl`, so `--replay actions.jsonl` re-issues it. Process sizes go to
`memory.jsonl` every 15 s.

## Running

```sh
python3 -m gui_fuzz.main --desktop gnome --seed 7 --minutes 6 --out ~/gfz/out/seed7
```

Run from the directory that contains `gui_fuzz/`, with the session's environment taken from the shell process
(`gnome-shell`, `cinnamon` or `plasmashell`) plus put.gd's private `XDG_CONFIG_HOME` and `XDG_DATA_HOME`. Give the
guest two heads (`lab.py launch --heads 2`) and a local-only destination (`archive.keepUploads`, every action's
`destination` `local`), and bind put.gd's hotkeys to `Ctrl+Shift+1` to `Ctrl+Shift+0` with `settings.patch`. Stop it
with SIGINT so the virtual devices are destroyed. Retire a guest whose state is broken.

Per desktop:

- **GNOME.** Install the nightly DEB (Ubuntu 26.04), copy `shell-extension/guifuzz@put.gd.test` to
  `~/.local/share/gnome-shell/extensions/`, add `guifuzz@put.gd.test` beside `putgd@put.gd` in
  `org.gnome.shell enabled-extensions`, `sync` and reset the guest through QMP so the autologin starts a Shell that
  loads both. The golden's `lab` has `sudo` only for `apt-get`: run root commands through
  `sudo -n apt-get -o APT::Update::Pre-Invoke::="sh /tmp/script.sh" update`, which is how `/dev/uinput` is opened
  and `unattended-upgrades` masked (it holds the apt lock and moves the guest to a newer put.gd from `pkg.put.gd`).
  Monitors change with `gdctl set`.
- **Cinnamon.** Install the nightly DEB for Ubuntu 24.04, `apt-get install xclip` (`python3-xlib` ships in the
  golden), open `/dev/uinput` the same way, and set every action's `destination` to `local`: the golden's default
  is `upload`. Monitors change with `xrandr`.
- **Disk.** The GNOME golden boots with 3 GB free. put.gd refuses an import when the data folder has less than its
  reserve (about 1.6 GiB on a 32 GiB disk) and answers `disk_space_insufficient`, so a run on it needs the data folder
  on another disk: attach a scratch qcow2 with QMP `device_add usb-storage` (the q35 bus has no PCIe hotplug),
  format it through the root hook and bind-mount it over `~/putgd-lab/data`. The first GNOME run lost every capture
  import for its last two hours to this before the cause was found.
- **Four vCPUs.** The profiles' `cpus` and `memory_mib` are the lab defaults; edit them in the host's copy of
  `tools/lab/profiles`, not in the repository, when the host is shared.

## First runs

Nightly 26.1117.5114 in aurora-amd guests (4 vCPU, 4.5 GB, two heads), 2026-11-17.

| Desktop | Walk time | Seeds | Steps | Result |
| --- | --- | --- | --- | --- |
| Plasma 6.8 | 3.4 h | 34 | 12,969 | no put.gd defect ([#1565](https://github.com/patrick-hudson/put.gd-desktop/pull/1565)) |
| GNOME 50 | 3.75 h | 38 | 14,625 | no put.gd crash, shell error or leaked grab. 2.95 h (30 seeds) on the first setup, 0.8 h (8 seeds) more after its data folder moved to a bigger disk. The resident held 28 MB for 3 h (40 MB after the restart, with a bigger history). `gnome-shell` moved between 600 and 840 MB with no trend |
| Cinnamon 6.6 | 2.4 h | 25 | 9,609 | one put.gd bug: the History expanded preview shrank to 150x145 with no picture when the screen reported no work area ([#1625](https://github.com/patrick-hudson/put.gd-desktop/issues/1625), fixed in [#1626](https://github.com/patrick-hudson/put.gd-desktop/pull/1626)). `nemo-desktop` crashed 5 times on output changes, which is Mint's |

Findings that were not put.gd's: Ubuntu's app store opened for a file with no handler and took the focus (the watchdog
now closes such windows), `shell_modal_stuck` once on GNOME while a portal file chooser was open (not reproduced; the
witness now reports which actors hold the grab), and `nemo-desktop` crashing in `nemo_window_slot_get_window`.

## Traps

- KWin's JS has no `setTimeout`; a QML script with a `Timer` would not load from the guest's home
  (`File name case mismatch`), which is why the KDE fence uses a D-Bus ping-pong.
- Output churn crashes desktop processes on its own: `plasmashell` 6.8.2 died in
  `QtWaylandClient::QWaylandXdgSurface::applyConfigure` while `kscreen-doctor` toggled the second output and its scale
  in a loop, and Mint's `nemo-desktop` (the desktop icons) aborts or segfaults several times an hour while `xrandr`
  changes modes (`nemo_window_slot_get_window` assertion). That is the desktop's fault, not put.gd's; the walk records
  it and carries on. DrKonqi's crash viewer then takes the focus: the watchdog kills it by PID.
- A scale change leaves a gap between the outputs, and KWin stops a relative pointer jump at the gap's edge:
  put the second output next to the first with `kscreen-doctor output.Virtual-2.position.<width>,0`.
- A put.gd window can be wider than a small scaled output. Choose click points that are on an output.
- Notification toasts lie over put.gd's windows and take clicks: the fence checks what is topmost. On Cinnamon the
  bubbles are Clutter actors that the X fence cannot see.
- A recording or scroll capture waiting for Start keeps its selector and bar up by design; cancel them before
  judging a grab stuck. An edit left open rejects every capture with `editor_active`, whose `details.operationId`
  is the id to cancel (`operation.recent` lists only the last 100 operations).
- GNOME: the Shell's window list has no popups as such and put.gd's selector has the type
  `Gjs_putgd_put_gd_extension_RegionSelector`; match its suffix. A Shell actor can report a NaN size, which becomes
  `null` in JSON.
- GNOME logs `pipewire remote error: id:2 no more input formats` when a screen recording starts or stops. The clip
  is valid (a 6.5 s H.264 clip that `gst-discoverer-1.0` reads), so it is Mutter's stream teardown, not a failed
  recording.
- X11: `translate_coords` is called on the root with the window as source to get a window's origin; the other way
  round returns the negated origin. An `xclip` that serves a selection must not inherit the caller's pipes, or
  `subprocess.run` waits for it until the timeout.
- Search the nightly's log for ffmpeg's encoder probes before calling a line critical: `Cannot load libcuda.so.1`
  (nvenc) and the VAAPI lines appear on every recording in a VM.
