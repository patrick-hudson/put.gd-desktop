# AppImage Wayland window icons and GNOME own-window refusal — 2026-10-19

This record certifies what pull request #101 (`claude/appimage-wayland-icons`
into `nightly`, issue #88) built and ran. Since #81 the AppImage's windows are
native Wayland clients. A Wayland compositor takes a window's icon from the
desktop entry named by its app ID, and Qt 6.8 cannot send one with the window.
The image had no host entry for `putgd-ui` or `putgd-record`, so KWin and GNOME
showed generic icons.

The pull request makes AppRun write `putgd-ui.desktop` and
`putgd-record.desktop` in a Wayland session while the image runs, and remove
them when it stops. The next launch removes entries that a killed launch left.
The design is described in
[packaging/appimage/README.md](../../packaging/appimage/README.md)
("Runtime behavior").

## Candidates

| Build | Built by | AppImage (SHA-256, bytes) |
| --- | --- | --- |
| `5595ea6`, CI merge `930d228` (`0.1.1-dev.430+g930d228dce6a`), **the candidate** | pull request CI run 39210434511 | `98bba655011c3caa38e9a5714d4da37890c4bab689a2bc4e4c2cb49f8df8590f`, 113,019,384 |
| nightly `9042776` (`0.1.1-dev.419+g9042776eb70e`), the baseline (includes #81) | nightly CI | `1be3fbdcd8502d7c3fe58c8faaeed95e7aeab4def6c1df6e60dbf07d1c8ebec7`, 113,019,384 |

## Suites

- Pull request CI on `5595ea6` passed:
  - contracts;
  - Linux 24.04 and 26.04;
  - the Windows portable build;
  - the three native packages;
  - the AppImage. Its receipt records `postDeploymentAudit.windowIcon:
    usr/share/icons/hicolor/scalable/apps/putgd.svg`.
- `tests/unit/test_portable_appimage.py` passed 28 tests under dash and under
  bash `--posix`, including the 8 new ones:
  - the entries' contents and owner key;
  - Wayland detection from `XDG_SESSION_TYPE` alone;
  - host and user entries kept, with no supervisor;
  - a killed launch's entries replaced while a running launch's are kept;
  - the KDE cache rebuilt after removal;
  - SIGTERM cleanup;
  - the icon audit.
- Mutation checks showed the new tests fail when each of these is disabled:
  - the entry removal;
  - the owner-key filter;
  - the host-entry check;
  - the liveness check.

## Physical desktops

Every run used the image itself (`IMAGE --background`) in the real desktop
session. The runs had private `XDG_RUNTIME_DIR`, `XDG_CONFIG_HOME`,
`XDG_CACHE_HOME` and `XDG_STATE_HOME` under `~/putgd-lab/ai`. `XDG_DATA_HOME`
was the real one, because the compositor reads AppRun's entries from
`~/.local/share/applications`. The image's `putgd-cli` drove the views and
actions. Pointer and keys went through `tools/lab/host_input.py`, and frames came
from the host's loopback VNC tile. Nothing was installed.

| Host | Session | Desktop lock |
| --- | --- | --- |
| f44 `.70`, Fedora 44, kernel 6.19.10-300.fc44 | GNOME Shell 50.0, mutter 50.0-1.fc44 Wayland, 1920x1080 | 06:45:57Z–06:57:32Z |
| d69 `.69`, Debian 13, kernel 6.12.107+deb13 | Plasma 6.3.6, KWin 6.3.6 Wayland, 1920x1080 | 06:57:33Z–07:00:28Z |

### Window icons

- **d69, KWin title bar and task switcher (Alt+Tab):**
  - Baseline: the Settings window and its switcher entry showed KWin's generic
    Wayland icon
    ([title bar](appimage-wayland-icons/d69-baseline-titlebar-generic.png),
    [switcher](appimage-wayland-icons/d69-baseline-switcher-generic.png)).
  - Candidate: both showed the put.gd icon
    ([title bar](appimage-wayland-icons/d69-candidate-titlebar-icon.png),
    [switcher](appimage-wayland-icons/d69-candidate-switcher-icon.png)).
- **f44, GNOME overview and dash:**
  - Baseline: the window's app icon in the overview and its running entry in
    the dash were the generic gear
    ([overview](appimage-wayland-icons/f44-baseline-overview-dash-generic.png)).
  - Candidate: both showed the put.gd icon
    ([overview](appimage-wayland-icons/f44-candidate-overview-dash-icon.png)).
- **Entries while running:** each candidate run had these entries, mode 0600:
  - `putgd-ui.desktop` and `putgd-record.desktop` with `NoDisplay=true`,
    `StartupWMClass` = the app ID, `Icon=/tmp/.mount_…/usr/share/icons/hicolor/scalable/apps/putgd.svg`,
    `Exec` = the AppImage path and `X-PutGD-AppImage-Owner=<AppRun PID> <start time>`;
  - on d69, also the KWin `putgd-appimage-*` and `putgd-record-appimage-*`
    permission entries, now with the same owner key.
- **The views:** `putgd-ui` mapped `libqwayland-generic.so`, `libxdg-shell.so`
  and, on GNOME, `libadwaita.so`.

### Entries removed after the image stops

The record of each candidate run in this section is in the run's
`results.json` (`entries`).

- **After a normal quit** (`ui.quit`): on both hosts, no `putgd*.desktop` was
  left in `~/.local/share/applications`, and none of the run's processes
  remained.
- **After a killed launch:**
  1. The driver sent SIGKILL to the launch's AppRun, then to its `putgd-ui` and
     `putgd`. Nothing of the run remained.
  2. Its entries stayed, naming the dead AppRun (f44 PID 659836, d69 PID 837471).
  3. The next launch removed them and wrote its own (f44 owner 661382, d69 owner
     838578).
  4. After that launch quit, no entry was left.

### GNOME own-window refusal (f44)

The put.gd Shell extension from this branch (`integrations/gnome`, unchanged
from nightly; `extension.js` SHA-256 `6542f3a9…`) was loaded as a temporary user
copy. An autologin re-login at 06:55:08Z made it ACTIVE. Window capture was then
invoked through `action.invoke` with each window focused:

| Focused window | Process | Result |
| --- | --- | --- |
| Settings | `putgd-ui` (views helper, native Wayland) | refused, `capture_target` "Focus an external application window before capture" ([frame](appimage-wayland-icons/f44-refused-settings.png)) |
| History | the same `putgd-ui` | refused, `capture_target` ([frame](appimage-wayland-icons/f44-refused-history.png)) |
| external `gst-launch-1.0 … waylandsink` window | not put.gd | captured, 640x400 PNG saved locally ([capture](appimage-wayland-icons/f44-control-external-window-captured.png)) |
| editor (`putgd-ui --edit-image`) | a second `putgd-ui`, child of the resident | **captured** ([capture](appimage-wayland-icons/f44-editor-window-captured.png)) |

Mutter reports the native views' client PID, so the extension's PID comparison
still refuses Settings and History. No change to `integrations/gnome` was needed
for #88.

The driver's step flag for the external-window control reads FAIL. That is a
driver bug: it compared an empty error code with `None`. The operation record
shows `state: completed`, `local.status: saved`, 57,588 bytes. The driver was
corrected afterwards.

## Not covered and follow-ups

- **GNOME editor window is not refused.** The extension compares the focused
  window's PID with the resident's and the views helper's (`SetUiProcess`). The
  editor runs in a separate `putgd-ui --edit-image` process, so a window capture
  of it succeeds. This does not depend on #81: under XWayland the editor's PID
  differed in the same way. KWin refuses the editor through its `putgd-ui`
  desktop file name. Refusing windows whose process is a direct child of the
  resident (the check the extension already applies to the recording bar)
  would close the gap. That is a separate change.
- **Recorder windows** (`putgd-record`) were not opened; their entry was checked
  only for content and removal.
- **Plasma's task manager** was not checked separately; it already showed the
  put.gd icon in #81's candidate run.
- **Not run:** KWin 6.6 on Wayland (no physical desktop runs it) and an
  integrated install (AppImageLauncher, Gear Lever) alongside the AppImage.
  AppRun skips an ID the host already has, and that skip is tested only in the
  unit suite.

## Lab state and cleanup

- **f44:**
  - No browser, terminal or agent of Patrick's was in the session before either
    re-login (06:55:08Z, 06:57:05Z).
  - The extension user copy was in place from 06:48Z to 06:57Z.
  - After the second re-login, `gnome-extensions info putgd@put.gd` reported it
    does not exist, as before the slot.
  - AppIndicator was ACTIVE, `IsStatusNotifierHostRegistered` was true and the
    document portal was mounted.
  - `enabled-extensions` was unchanged:
    `['background-logo@fedorahosted.org', 'putgd@put.gd', 'appindicatorsupport@rgcjonas.gmail.com']`.
  - The re-login cleared the baseline run's "window could not be opened" notice.
    The driver had caused it by killing the startup view.
- **d69:**
  - SIGKILL of the views helper raised a "window could not be opened" popup,
    which was dismissed.
  - The notification history held only put.gd entries from 06:42–06:49Z
    (another session's slot) and KWin's 06:44Z graphics-reset notice. They were
    left alone.
  - `kbuildsycoca6 --noincremental` ran after the entries were gone.
  - No GPU hang occurred during the slot.
- **Both hosts:**
  - `~/.local/share/putgd`, created by the runs, was removed.
  - The input agent (`putgd-lab-input-ai`) and its workstation forward were
    stopped.
  - Both locks were released with `desktop_lock.py release --verified-clean`;
    the receipts are in `~/putgd-lab/locks/history/`.

## Artifacts

`work/verification/2026-10-19-appimage-wayland-icons/` in the branch worktree
holds:

- the driver (`icons.py`), the input and f44 session scripts, and the host
  configurations;
- per run (`f44/baseline`, `f44/candidate`, `f44/candidate-extension`,
  `d69/baseline`, `d69/candidate`): `results.json` (steps, entries, processes),
  `run.log` and every wall frame;
- the re-login transcripts and the after-cleanup frames.

The CI artifact is in `work/ai/ci-5595ea6/`.
