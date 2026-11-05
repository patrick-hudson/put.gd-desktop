# X11 window capture refuses put.gd's editor — 2026-10-19

This record certifies what pull request #106 (`claude/x11-editor-refusal` into
`nightly`, issue #103) built and ran. Before the change, X11 window capture
refused only the resident's own Qt windows and a window whose `_NET_WM_PID` was
the registered views helper's (on the Cinnamon core route, also the capture
helper's). The Edit & upload editor runs as a separate, unregistered
`putgd-ui --edit-image` process, so its window could be captured on Cinnamon and
Plasma X11. #104 fixed the same gap on GNOME.

Both X11 window freezes run in the resident: `X11Capture::freeze` for the Qt
Widgets resident (Plasma X11 and generic X11) and `X11NativeSnapshot::freeze`
for the Cinnamon core resident. Each now also refuses a window whose
`_NET_WM_PID` names a process whose parent (from `/proc/PID/stat`) is the
resident. The design is in [Cinnamon / X11](../desktops/cinnamon.md).

## Candidate

| Build | Built by | Artifact (SHA-256, bytes) |
| --- | --- | --- |
| `85d04f0` (`0.1.1-dev.440+g85d04f0926d7`, tree `a060e5f3`) | manual CI run 39266644046 (`workflow_dispatch` on the branch) | Ubuntu 26.04 DEB `putgd_0.1.1~dev.440+g85d04f0926d7_amd64.deb` `0b8aa778fc375ad6744d79e3b4f0fc857a9a670443ca64a4d27d9a58bafadd06`, 52,270,738 |
| the same | the same run | AppImage `putgd-0.1.1-dev.440+g85d04f0926d7-x86_64.AppImage` `d14dc770e8bb109fcfe67026ee0823d24125171cecb31b3d0f11c25437b2ede8`, 113,019,384 |

Pull request CI skips the package jobs when only `src/` and C++ tests change,
so the packages came from a manual run, which selects every job. That run's
Linux test jobs failed on a bug in the new native-snapshot test, fixed in
`23f3872`: the stand-in views helper PID (the editor's plus one) was usually the
reaped child's PID, so the "no `/proc` entry" window was refused through the
excluded set. `23f3872` changes only those two test lines, so the packaged
source is identical.

## Suites

- **Failing first.** On d69 (Debian 13, Qt 6.8.2), the tests-only commit
  `da55682` against nightly's source failed both new cases at the editor
  refusal: `x11_isolated` (`test_x11.cpp:418`) and `x11_native_snapshot`
  (`test_native_snapshot.cpp:34`).
- **Fixed.** At `23f3872` on d69, both passed, and the full `ctest --preset dev` passed (76 of 76, 656 s, 14:23Z–14:34Z, `nice -n 10`).
- Pull request CI on `23f3872` (run 39267432691) passed: contracts, Linux 24.04
  and 26.04, and the Windows portable build. `CI` passed.
- The new cases (`residentChildWindowRejectedWithoutRegistration`) run an
  unregistered `QProcess` child of the test process, started the way `EditorJob`
  starts the editor. Its window is refused with and without a registered views
  helper. The test's parent process and a reaped child's PID stay capturable.

## Physical desktop

| Host | Session | Desktop lock |
| --- | --- | --- |
| k72 `.72`, Kubuntu 26.04.1 LTS, kernel 7.0.0-38-generic | Plasma X11: kwin-x11 6.6.4-0ubuntu1, plasma-workspace 6.6.6-0ubuntu0.1, Xorg 21.1.22-1ubuntu1.2, HDMI-1 1920x1080 | 14:24:24Z–14:29:14Z |

### How it ran

1. `apt-get install ./putgd_…deb` was one package (`apt-get --simulate`
   checked first).
2. **DEB run (14:24:52Z–14:27:02Z):** the resident was `/usr/bin/putgd
   --background`.
3. **AppImage run (14:27:07Z–14:28:40Z):** the resident ran from
   `/tmp/.mount_putgd-eHlKdo/usr/bin/putgd`. The DEB was still installed but
   not running.

Both residents reported version `0.1.1-dev.440+g85d04f0926d7`, commit
`85d04f0`, and the `cinnamon-x11` adapter (`X11SourceAdapter`, the Qt Widgets
route that Plasma X11 uses). The runs had private `XDG_RUNTIME_DIR`,
`XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_STATE_HOME` and `XDG_DATA_HOME`
under `~/putgd-lab/x11r`, and saved captures locally only.

The driver ran each step through `tools/lab/physical.py`:

- Focus went through KWin with `xdotool windowactivate --sync`, an EWMH
  `_NET_ACTIVE_WINDOW` request. `xdotool getactivewindow` confirmed the
  target was active before each capture.
- Each capture step recorded the active window's `_NET_WM_PID`, `WM_CLASS`,
  title and `WM_CLIENT_MACHINE`, plus that process's parent and command line.
- Window capture was invoked with each build's own `putgd-cli`
  (`action.invoke`, `window`), the same prepare path as Ctrl+Shift+2.
- Frames came from the host's loopback VNC tile, the one the wall shows.
- Before the editor step, the views window was closed with `wmctrl -ic`, and
  the views helper exited. The editor was then the only put.gd window.

### Process tree

| Build | Resident | Views helper | Editor (`putgd-ui --edit-image`) |
| --- | --- | --- | --- |
| DEB | 892423 `/usr/bin/putgd --background`, ppid 1 | 892437, ppid 892423 | 894974, **ppid 892423** |
| AppImage | 897655 `…/usr/bin/putgd --background`, ppid 897632 (`AppRun`, dash) | 897667, ppid 897655 | 900245, **ppid 897655** |

On X11 the AppImage's `AppRun` shell stays as the resident's parent, but the
resident starts its helpers directly, so the rule still matches them.

### Results

Every refused window ended with the operation `failed`, `capture_target`
"Focus an external window before invoking window capture". It showed as a
Plasma notification and as a failed History entry.

| Focused window (`_NET_WM_PID`) | DEB | AppImage |
| --- | --- | --- |
| Settings (views helper) | refused ([frame](x11-editor-refusal/k72-deb-refused-settings-after.png)) | refused ([frame](x11-editor-refusal/k72-appimage-refused-settings-after.png)) |
| History (views helper, same window) | refused ([frame](x11-editor-refusal/k72-deb-refused-history-focused.png)) | refused ([frame](x11-editor-refusal/k72-appimage-refused-history-focused.png)) |
| Edit & upload editor, the only put.gd window | **refused** ([frame](x11-editor-refusal/k72-deb-refused-editor-after.png)) | **refused** ([frame](x11-editor-refusal/k72-appimage-refused-editor-after.png)) |
| external `kdialog` (ppid 1) | captured, 404x152 PNG, 10,137 bytes ([capture](x11-editor-refusal/k72-deb-control-external-captured.png)) | captured, 404x152 PNG, 10,137 bytes ([capture](x11-editor-refusal/k72-appimage-control-external-captured.png)) |

The "History refused" frames are taken just before the capture; the
History list there already holds the Settings attempt's failed entry. Both
external captures have the same SHA-256 (`8a243cf1…`): the same dialog, framed
identically.

## Not covered

- **Before-change desktop run:** k72 didn't run an unfixed build. The
  failing-first suites show the gap, and the code path is the one the unit
  cases exercise.
- **Cinnamon core route on a desktop:** `X11NativeSnapshot` (the Cinnamon core
  resident) is covered by `x11_native_snapshot` on Xvfb only. No Mint desktop
  ran.
- **Recorder windows** (`putgd-record`'s bar and review window) were not opened.
  The rule covers them, because they are the resident's children, but no capture
  of them was attempted.
- **Flatpak:** inside the sandbox `_NET_WM_PID` and `/proc` both use the
  sandbox's PID namespace, so the parent check should still match there. It
  wasn't run. As before, another Flatpak's window with a colliding namespace PID
  could also be refused.
- **Multi-monitor and the hotkey path:** not run.

## Lab state and cleanup

- Patrick's apps in the session (Firefox, Codex and others) weren't touched.
  There was no re-login.
- `apt-get remove -y putgd` removed only putgd. Afterwards `dpkg-query -W putgd`
  found nothing, and no `/usr/bin/putgd*`, `~/.local/share/putgd` or
  `~/.config/putgd` was left.
- `~/putgd-lab/x11r` (the private XDG directories and the copied packages) was
  removed.
- `pgrep -u patrick -af 'putgd|host_input|videotestsrc|kdialog'` showed only
  PID 283586. That's a workstation memory collector over SSH that predates the
  lock, not this run's.
- The after-cleanup frame shows no put.gd window or notification. A Snapd
  "Firefox was updated" notification from 22:12 local was there before the
  run and was left alone.
- The lock was released with `desktop_lock.py release --verified-clean`
  (receipt `~/putgd-lab/locks/history/2026-10-19T14-29-14Z-release.json`).

## Artifacts

`work/verification/2026-10-19-x11-editor-refusal/` in the branch worktree
holds:

- the driver (`refusal.py`), `step.sh` and the host configuration (`k72.json`);
- per build (`k72/deb`, `k72/appimage`): `results.json` (steps, active-window
  identities, process trees, operations), `run.log`, `resident.log` and every
  wall frame;
- the install and remove transcripts and the after-cleanup frame.

The CI artifacts are in `work/ai/ci/39266644046/`. The d69 build tree is
`~/work/put.gd-x11child` (logs `ctest-unfixed.log`, `ctest-fixed-targeted2.log`
and `ctest-full.log`).
