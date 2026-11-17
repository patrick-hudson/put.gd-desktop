# On-screen GUI fuzzer

`tools/lab/gui_fuzz/` drives put.gd's real windows on a real Plasma Wayland session with kernel input and a
seeded random walk, and watches for crashes, hangs, grabs left behind and inconsistent state that offscreen
tests cannot reach. It runs inside a lab guest (the [Aurora Plasma 6.8 golden](aurora-vm.md#plasma-68-golden))
as the guest user, with Python 3.12+ and `dbus-python`/`PyGObject`, which that image ships. It is a lab tool,
not acceptance evidence for any desktop.

## Parts

| File | Job |
| --- | --- |
| `uinput.py` | A virtual keyboard and a relative mouse through `/dev/uinput` (the guest needs `sudo chmod 666 /dev/uinput`). KWin's flat pointer acceleration profile is set for the mouse, so one unit is one pixel. |
| `fence.py` | A persistent KWin script that reports every window, popup, layer surface, output and the cursor 25 to 100 times a second. KWin's JS has no timer, so the script calls back over D-Bus and the receiver answers after 35 ms. |
| `world.py` | put.gd's processes and windows, and the fence: the topmost window under a point must be put.gd's own (or its portal file chooser), unless the region selector covers the outputs. |
| `actor.py` | Every input primitive, fenced and logged. A pointer jump that KWin clamps never clicks. Keys come from an allow-list: no Meta, no Alt+Tab, no Alt+F4, no Ctrl+Alt chords, no function keys but put.gd's own hotkeys, no Ctrl+Q. |
| `walk.py` | The walk: hotkeys, the tray menu through its dbusmenu, selector drags, clicks, drags, wheel, key chords, held and repeated keys, text, window resizes and moves, monitor hotplug and scale changes with `kscreen-doctor`, clipboard churn, rapid repeats and `putgd-cli` session methods. Odd seeds run in chaos mode (monitor churn and rapid repeats weighted up). |
| `watch.py` | The watchdog and the findings. |
| `ring.py` | On the QEMU host: a rolling QMP screen record, and `save` to cut the minute around a finding. |

## Findings

A resident crash, any core dump, a session method that does not answer in 5 s, a region selector or popup still
up after Escape and the cancel of any recording, scroll session or edit, put.gd's tray lost after a shell
restart, `history.counts` against `history.list`, settings revisions, operations stuck in a running state, Qt
critical lines in the resident's log, and focus on a window that is not put.gd's for more than 6 s. Each writes
`findings/NNN-kind.json` with the last 60 actions, the windows, process sizes, core dumps and the journal tail,
and a line in `marks.log` with the time, to cut the screen record. A fatal finding (resident, KWin or the
session gone, VT change) stops the walk.

Every input is logged to `actions.jsonl`, so `--replay actions.jsonl` re-issues it. Process sizes go to
`memory.jsonl` every 15 s.

## Running

```sh
python3 -m gui_fuzz.main --seed 7 --minutes 8 --out ~/gfz/out/seed7
```

Run from the directory that contains `gui_fuzz/`, with the session's environment (the walk reads the plasmashell
process's environment, as `lab.py` does). Give the guest two heads (`lab.py launch --heads 2`) and a local-only
destination (`archive.keepUploads`), and bind put.gd's hotkeys to `Ctrl+Shift+1` to `Ctrl+Shift+0` with
`settings.patch`. Stop it with SIGINT so the virtual devices are destroyed. Retire a guest whose state is broken.

## Traps

- KWin's JS has no `setTimeout`; a QML script with a `Timer` would not load from the guest's home
  (`File name case mismatch`), which is why the fence uses a D-Bus ping-pong.
- Output churn crashes the desktop on its own: `plasmashell` 6.8.2 died in
  `QtWaylandClient::QWaylandXdgSurface::applyConfigure` while `kscreen-doctor` toggled the second output and its scale
  in a loop. That is the shell's fault, not put.gd's; the walk records it and carries on, and put.gd's tray registered
  again with the new shell. DrKonqi's crash viewer then takes the focus: the watchdog kills it by PID.
- A scale change leaves a gap between the outputs, and KWin stops a relative pointer jump at the gap's edge:
  put the second output next to the first with `kscreen-doctor output.Virtual-2.position.<width>,0`.
- A put.gd window can be wider than a small scaled output. Choose click points that are on an output.
- Notification toasts lie over put.gd's windows and take clicks: the fence checks what is topmost.
- A recording or scroll capture waiting for Start keeps its selector and bar up by design; cancel them before
  judging a grab stuck. An edit left open rejects every capture with `editor_active`, whose `details.operationId`
  is the id to cancel (`operation.recent` lists only the last 100 operations).
