# GNOME window capture refuses put.gd's editor — 2026-10-19

This record certifies what pull request #104 (`claude/gnome-editor-refusal`
into `nightly`, issue #103) built and ran. Before the change, the put.gd Shell
extension refused window capture only when the focused window's PID was the
resident's or the views helper's (`SetUiProcess`). The Edit & upload editor
runs as a separate `putgd-ui --edit-image` process, so its window could be
captured (found by #101 on f44).

The extension now also refuses a window whose process is a direct child of the
authenticated resident (parent PID from `/proc/PID/stat`). The design is in
[GNOME](../desktops/gnome.md), "Bridge contract".

## Candidate

| Build | Built by | Artifact (SHA-256, bytes) |
| --- | --- | --- |
| `47df2fa`, CI merge `3a9438c` (`0.1.1-dev.440+g3a9438cca3a2`) | pull request CI run 39261702735 | Fedora 44 RPM `putgd-0.1.1~dev.440+g3a9438cca3a2-1.fc44.x86_64.rpm` `ba4d1774cb523a53b5f087981f11e2d2e9625aafddc9b9062765a1d7f625d66c`, 39,208,309 |
| the same | the same run | AppImage `putgd-0.1.1-dev.440+g3a9438cca3a2-x86_64.AppImage` `3a4cae53eec29003266ee15a89dff64940f7f6514e42d35d84526a0a23f75e1b`, 113,019,384 |

The RPM's `extension.js` (`5c44549ce913685a…`) and `metadata.json` (version 2,
`690be5bcfd1ee057…`) are byte-identical to the branch's.

## Suites

- Pull request CI on `47df2fa` passed:
  - contracts;
  - Linux 24.04 and 26.04;
  - the Windows portable build;
  - the four native packages;
  - the AppImage.
- `gjs -m tests/desktop/test_gnome_ownership.mjs integrations/gnome/extension.js`
  ran on f44 (gjs 1.88.0): all 27 cases passed, including the two new ones:
  - an unregistered editor PID whose parent is the resident is refused before
    layout, with a registered views helper and without one;
  - the views helper is still refused, and an external process (parent 1) and
    a process whose `/proc` entry can't be read are still captured.
- The same suite against nightly's unchanged `extension.js` fails at the new
  editor case.

## Physical desktop

| Host | Session | Desktop lock |
| --- | --- | --- |
| f44 `.70`, Fedora 44, kernel 6.19.10-300.fc44 | GNOME Shell 50.0, mutter 50.0-1.fc44 Wayland, 1920x1080 | 13:49:44Z–13:54:49Z |

### How it ran

1. `dnf install` of the CI RPM. The transaction was the one package. A GDM
   autologin re-login at 13:50:02Z loaded the extension from
   `/usr/share/gnome-shell/extensions/putgd@put.gd`. `gnome-extensions info`
   reported version 2, ACTIVE.
2. **RPM run (13:50:32Z–13:52:12Z):** the resident was `putgd-launcher
   --background`, run as `/usr/bin/putgd`.
3. **AppImage run (13:52:28Z–13:53:58Z):** the resident was `IMAGE
   --background`, run from `/tmp/.mount_putgd-ePglcD/usr/bin/putgd`. It used the
   same loaded extension; the RPM was still installed.

Both residents reported `helper.extensionVersion: 2`, `protocol: 1` and
`available: true`. The runs had private `XDG_RUNTIME_DIR`, `XDG_CONFIG_HOME`,
`XDG_CACHE_HOME` and `XDG_STATE_HOME` under `~/putgd-lab/g103`, and saved
captures locally only.

Each build's own `putgd-cli` drove the steps:

- Window capture was requested through `action.invoke` (`window`), after a
  title-bar click through `tools/lab/host_input.py` focused the target.
- Frames came from the host's loopback VNC tile, the one the wall shows.
- Before the editor step, the views were closed with Alt+F4 and their helper
  had exited. The editor was then the only put.gd window.

### Process tree of the editor

| Build | Resident | Editor (`putgd-ui --edit-image`) |
| --- | --- | --- |
| RPM | 721475 `/usr/bin/putgd --background`, ppid 1 | 722851, **ppid 721475** |
| AppImage | 724923 `/tmp/.mount_putgd-ePglcD/usr/bin/putgd --background`, ppid 1 | 726190, **ppid 724923** |

In both builds the views helper was also a direct child of the resident (RPM
721487, AppImage 724950). No AppRun or loader sits between the resident and its
helpers.

### Results

Every refused window ended with the operation `failed`, `capture_target`
"Focus an external application window before capture", shown as a GNOME banner.

| Focused window | RPM | AppImage |
| --- | --- | --- |
| Settings (views helper) | refused ([frame](gnome-editor-refusal/f44-rpm-refused-settings.png)) | refused ([frame](gnome-editor-refusal/f44-appimage-refused-settings.png)) |
| History (views helper) | refused ([frame](gnome-editor-refusal/f44-rpm-refused-history.png)) | refused ([frame](gnome-editor-refusal/f44-appimage-refused-history.png)) |
| Edit & upload editor, the only put.gd window | **refused** ([frame](gnome-editor-refusal/f44-rpm-refused-editor.png)) | **refused** ([frame](gnome-editor-refusal/f44-appimage-refused-editor.png)) |
| external `gst-launch-1.0 … waylandsink` window (ppid 1) | captured, 640x400 PNG, 57,403 bytes ([capture](gnome-editor-refusal/f44-rpm-control-external-captured.png)) | captured, 640x400 PNG, 57,227 bytes ([capture](gnome-editor-refusal/f44-appimage-control-external-captured.png)) |

With nightly's extension, #101's run captured this same editor window
([capture](appimage-wayland-icons/f44-editor-window-captured.png)).

## Not covered

- **Recorder windows** (`putgd-record`'s bar and review window) were not opened.
  The rule covers them, because they are the resident's children, but no
  capture of them was attempted.
- **X11 adapters:** `X11Capture` (Cinnamon and Plasma X11) still compares only
  with the registered views helper's `_NET_WM_PID`, so the editor's window isn't
  refused there either. That is outside #103.
- **Flatpak:** a sandboxed resident's PIDs aren't host PIDs. [Flatpak](../flatpak.md)
  change 2 (sandboxed app-ID matching) is still needed there.
- **Multi-monitor and the hotkey path:** not run. Window capture was invoked
  through the CLI, which takes the same `_prepare` path as Ctrl+Shift+2.

## Lab state and cleanup

- No browser, terminal or agent of Patrick's was in the session before either
  re-login (13:50:02Z, 13:54:19Z).
- `dnf remove putgd` was also one package. Afterwards the installed package set
  equaled the pre-slot baseline, and no system or user `putgd@put.gd` was left.
- After re-login #2:
  - `gnome-extensions info putgd@put.gd` reported that it doesn't exist, as
    before the slot;
  - AppIndicator was ACTIVE, `IsStatusNotifierHostRegistered` was true and the
    document portal was mounted;
  - `enabled-extensions` was unchanged:
    `['background-logo@fedorahosted.org', 'putgd@put.gd', 'appindicatorsupport@rgcjonas.gmail.com']`.
- The re-login cleared the refusal banners.
- The runs had created `~/.local/share/putgd`; it was removed, along with
  `~/putgd-lab/g103`. The input agent (`putgd-lab-input-g103`) and its
  workstation forward were stopped.
- The lock was released with `desktop_lock.py release --verified-clean`
  (receipt `~/putgd-lab/locks/history/2026-10-19T13-54-49Z-release.json`).

## Artifacts

`work/verification/2026-10-19-gnome-editor-refusal/` in the branch worktree
holds:

- the driver (`refusal.py`), the input and f44 session scripts, and the host
  configuration;
- per build (`f44/rpm`, `f44/appimage`): `results.json` (steps, process trees,
  identities), `run.log` and every wall frame;
- the install and remove transcripts, the re-login transcripts and the
  after-cleanup frame.

The CI artifacts are in `work/ai/ci/`.
