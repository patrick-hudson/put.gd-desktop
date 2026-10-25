# Cinnamon AppImage taskbar icon — 2026-10-25

Code: [#570](https://github.com/patrick-hudson/put.gd-desktop/pull/570),
`cd155d03` (based on nightly `a3bf6d1c`). Merge this evidence after the code.

Patrick reported a generic bottom-panel taskbar icon on Mint with an AppImage
integrated through Gear Lever. The reported image showed `26.1024.1185-nightly`;
the tray upload arrow was still present. His installed Gear Lever entry had an
existing readable icon file but no `StartupWMClass`.

## Scope and identities

Physical m74 (`192.168.68.74`, `mint-builder`), Mint 22.3, Cinnamon 6.6.9,
X11 `:0`, kernel `6.14.0-37-generic`. Visible through the existing physical
VNC wall tile (host loopback 5900, workstation tunnel 5974).

This was a targeted launcher/desktop-metadata experiment, **not** a new release
build or exact final-asset acceptance. The unchanged application binaries were
from `putgd-26.1024.1186-dev+gd5b561ddb94f-x86_64.AppImage`, extracted into a new
private `~/work/ico25/squashfs-root`. No C++ compilation was needed or performed.
The original AppImage and prior runs were preserved. [Build manifest](binary-build.json)
and [host, binary, AppRun and desktop-entry SHA-256 identities](identities.json)
identify exactly what was tested. The candidate replaces only `AppRun` with
#570's script. The restored baseline AppRun hash matches source `d5b561ddb94f`.

Lock `taskbar-icon-1025` covered 15:09:00–15:14:37 UTC with a heartbeat.
The desktop was visibly unlocked. Preflight found only Mint Welcome and Nemo's
desktop, and no put.gd resident. The test maximized its own Settings window;
saved VNC crops include that window's left portion and the corresponding panel.
The live desktop remained visible throughout. No credential or cloud operation
was used. `ui.show` and `ui.quit` used the production CLI.

Private config/cache/state/runtime directories were under `~/work/ico25`;
`XDG_DATA_HOME` stayed at the real host value so Cinnamon could read entries.
The app's new data directory was retained under the run's `data-putgd` at cleanup.
Socket preflight passed before every launch: session socket 46 UTF-8 bytes,
X11 socket 17 bytes, session bus 18 bytes, all below the 107-byte limit.

## Saved graphical results

| Scenario | Result | Picture |
| --- | --- | --- |
| Unchanged AppRun, no helper desktop entries | Generic taskbar gear; `WM_CLASS` and `_GTK_APPLICATION_ID` both `putgd-ui`; `_NET_WM_ICON` present | [baseline](baseline.png) |
| #570 AppRun on X11, no integrated menu entry | Correct put.gd taskbar icon; temporary `putgd-ui.desktop` and `putgd-record.desktop` removed after `ui.quit` | [candidate](candidate.png) |
| Baseline AppRun, trial **hidden** entry under a different filename with `StartupWMClass=putgd-ui` | Generic gear remained in this attempt; not evidence that arbitrary hidden entries work | [failed trial](desktop-class.png) |
| Baseline AppRun, **visible** integrable entry under a different filename, `StartupWMClass=putgd-ui`, absolute icon path | Correct put.gd taskbar icon | [menu match](menu-class.png) |

The menu-entry trial adapted the root desktop entry's Exec and Icon paths to the
lab artifact. It did not run Gear Lever itself. The hidden trial was preserved;
the correction removed `NoDisplay=true` and allowed the desktop cache to settle
before launching again. This matches the visible entry that Gear Lever creates.
The independently passing AppRun test uses the exact helper desktop filename.

After verifying the visible menu-entry correction, Patrick's existing local
Gear Lever entry received only `StartupWMClass=putgd-ui`. Its original is backed
up privately in the task's work evidence. His running app was not restarted,
and no claim is made that its already-cached taskbar actor immediately refreshed.

## Automated checks and limitations

- [32 AppRun tests](unit-tests.log) and [8 root-entry tests](root-entry-tests.log)
  passed on m74. The AppRun suite also passed in the workstation's permitted
  short-test slot.
- The new X11 test [fails against baseline AppRun](regression-negative.log) for
  both DISPLAY-only and XDG_SESSION_TYPE-only environments, then passes with the
  candidate. Existing tests cover Wayland, foreign/host entries, stale owners,
  termination cleanup and KDE cache rebuilding. These synthetic tests are not
  graphical proof.
- First extraction tried the preexisting non-executable artifact and returned
  `Permission denied`. It was preserved; correction copied it to
  `~/work/ico25/baseline.AppImage`, made only the copy executable, and extracted
  that. No golden image or prior artifact was modified.
- Not run: a new complete CMake/CTest build, actual Gear Lever install/update,
  final CI AppImage, GNOME/KDE graphical checks, or release functional/memory
  gates. Full current-version desktop acceptance remains outstanding.

[Probe driver](probe.py), per-scenario JSON, hashes and failure images accompany
this summary. Raw run data remains on m74 at `~/work/ico25`; the workstation
private evidence is under `work/taskbar-icon/work/evidence` in the root checkout.
The [cleanup receipt](cleanup.json) reports no leftovers. Temporary entries and
all test residents were removed; unrelated Welcome/Nemo and shared SSH work were
preserved. No disposable VM was created.
