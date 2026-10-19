# AppImage native Wayland windows — 2026-10-19

This record certifies what pull request #81 (`claude/appimage-wayland-shell`
into `nightly`, stacked on #80) built and ran. The pull request deploys Qt's
Wayland client plugins in the AppImage, so its Qt windows run natively on
Wayland, as the DEB/RPM do:

- the `xdg-shell` shell integration;
- the `bradient` and `adwaita` decorations;
- the EGL client buffer integration.

The windows covered are the Settings, History and Activity views, the editor
and the KDE region helper. The pull request also adds a post-deployment start
check under a headless weston. The image build is described in
[packaging/appimage/README.md](../../packaging/appimage/README.md)
("Build prerequisites", "Runtime behavior").

## Problem as found

- Nightly `e13fee9`'s CI AppImage carried `libqwayland-generic.so` and
  `libqwayland-egl.so`, but no shell integration besides LayerShellQt's
  `liblayer-shell.so`, which is never a default. Qt 6.8's Wayland platform does
  not start without one.
- With `QT_QPA_PLATFORM` unset, every Qt window in the image fell back to
  XWayland, on Plasma and GNOME alike.
- `putgd-kde-region` needs the Wayland platform, so KDE region capture from the
  AppImage failed with `region_unavailable` on Plasma Wayland.
- Where `QT_QPA_PLATFORM=wayland` was set (the lab harness sets it per profile)
  or no X display was reachable, the windows aborted.

## Candidates

| Build | Built by | AppImage (SHA-256, bytes) |
| --- | --- | --- |
| `ae8519e`, CI merge `3ab5dce` (`0.1.1-dev.369+g3ab5dce8fe67`), **the candidate** | pull request CI run 39195010552 | `a0820560e09348a292fed71cd3c10afe7bcefdba788f5c3d92ba9fe2d0b92179`, 99,461,624 |
| nightly `e13fee9` (`0.1.1-dev.366+ge13fee972459`), the baseline | nightly CI | `228a26e55c9fa8159a464e6aadf781ed6c01f199bcf7d00dbabc2013c17042c0`, 99,236,344 |

The candidate is 225,280 bytes larger. That figure includes #80's
`libqoffscreen.so`. The first AppImage job of run 39195010552 failed before
building anything: pip got a connection reset while `install-qt-action`
installed aqtinstall. The job's re-run passed.

## Suites and builder checks

- Pull request CI on `ae8519e` passed: contracts, Linux 24.04 and 26.04, the
  Windows portable build, the three native packages and the AppImage. The
  AppImage receipt records `postDeploymentAudit.qtWayland`:
  - `headlessCompositor: weston` (Ubuntu 24.04's `weston 13.0.0-4build3`);
  - `imageWorkerStartedOnWayland: true`;
  - `waylandShellIntegration: xdg-shell`;
  - the five plugins under `usr/plugins/wayland-*`.
- `tests/unit/test_portable_appimage.py`: 18 tests passed, including the six
  new `WaylandAuditTest` cases.
- I ran `audit_wayland` against the extracted nightly AppImage. This was one
  short check, in the existing `putgd-appimage-builder:ubuntu24.04-rec`
  container (Ubuntu 24.04, official Qt 6.8.3, weston 13.0.0):
  - Nightly was rejected for the four missing plugins.
  - The same tree plus the four official Qt 6.8.3 plugins passed with
    `xdg-shell`.
  - With an empty `libxdg-shell.so` it was rejected, reporting Qt's loader
    message.
- Headless KDE rehearsal on d69: `kwin_wayland --virtual` without XWayland,
  with a private runtime directory, bus and XDG directories.
  - Nightly: `ui.show` answered `ui_crashed`, and region capture failed with
    `capture_crashed`, because the helper aborted.
  - The nightly tree plus the four plugins ran natively, and
    `putgd-kde-region` mapped `libqwayland-generic.so` and `libxdg-shell.so`.

## Physical desktops

Every run used the image itself (`IMAGE --background`) in the real desktop
session, with private `XDG_CONFIG_HOME`, `XDG_CACHE_HOME` and
`XDG_STATE_HOME`, and a local destination and archive under
`~/putgd-lab/aw/RUN`. Off KDE, `XDG_DATA_HOME` was private too. On KDE it stayed
the real one, because KWin reads AppRun's temporary ScreenShot2 entry from
there, so the KDE runs shared `~/.local/share/putgd` (removed afterwards).
Views and actions were driven through the image's `putgd-cli` (`ui.show`,
`action.invoke`, `editor.open` with a synthetic 960x600 test card).

Pointer input and screenshots went through the host's loopback VNC tile
(`tools/lab/rfb.py`). Which Qt plugins each GUI process used was read from
`/proc/PID/maps`, and its X11 windows from `xwininfo`/`xprop`. Nothing was
installed, and no session was restarted.

| Host | Session | Distro Qt (DEB/RPM) |
| --- | --- | --- |
| d69 `.69`, Debian 13.7, kernel 6.12.107 | Plasma 6.3.6, KWin 6.3.6 Wayland, 1920x1080 | 6.8.2 |
| f44 `.70`, Fedora 44, kernel 6.19.10 | GNOME Shell 50.0, mutter 50.0 Wayland, Xwayland 24.1.9, 1920x1080 | 6.11.2 |
| k72 `.72`, Kubuntu 26.04 | Plasma 6.6.6, KWin 6.6.4 X11, 1920x1080 | 6.10.2 |

### d69, Plasma Wayland

- **Nightly baseline:** Settings opened on XWayland. `putgd-ui` mapped
  `libqwayland-generic.so` and `libqxcb.so`, and its window was in the X tree.
  Region capture failed with `region_unavailable`
  ([Settings](appimage-native-wayland/d69-nightly-settings-xwayland.png),
  [failure notification](appimage-native-wayland/d69-nightly-region-unavailable.png)).
- **Candidate, views:** Settings and History were native Wayland windows.
  `putgd-ui` mapped `libqwayland-generic.so` and `libxdg-shell.so`, there was no
  X11 window and no decoration plugin, and KWin drew the Breeze frame
  ([Settings](appimage-native-wayland/d69-candidate-settings-native.png)).
- **Candidate, region capture:** `putgd-kde-region` ran natively with
  LayerShellQt's overlay and drew the selection outline while dragging
  ([selecting](appimage-native-wayland/d69-candidate-region-selecting.png)).
  The drag from (560, 300) to (1360, 800) produced an 800x500 PNG that was
  saved locally and copied
  ([capture](appimage-native-wayland/d69-candidate-region-capture-800x500.png),
  SHA-256 `616027e7…`).
- **Candidate, editor:** the editor (`putgd-ui --edit-image`) was native too.
  Save a copy opened modal and centred over it, with the file name focused, and
  was then cancelled
  ([editor](appimage-native-wayland/d69-candidate-editor-save-a-copy.png)).
- **Candidate, forced `QT_QPA_PLATFORM=wayland`:** the view and region helper
  were native, a 400x250 region was captured and copied, and the user journal
  showed no crash.

### f44, GNOME Wayland

- **Nightly baseline:** Settings opened on XWayland. `putgd-ui` mapped
  `libqxcb.so`, its X11 window had class `putgd-ui`, and mutter drew the frame
  ([Settings](appimage-native-wayland/f44-nightly-settings-xwayland.png)). The
  first attempt answered `ui_crashed`. That was my harness: it took the session
  environment from `gnome-shell`, which has no `XAUTHORITY` for mutter's
  Xwayland. The corrected run imports it from `systemctl --user
  show-environment`, as session-launched apps get it. The two cores of that
  attempt (22:54:13 CDT) belong to it.
- **Candidate, views:** Settings was native. `putgd-ui` mapped
  `libqwayland-generic.so`, `libxdg-shell.so` and
  `wayland-decoration-client/libadwaita.so`, and Qt drew adwaita frames
  ([Settings](appimage-native-wayland/f44-candidate-settings-adwaita.png)). The
  GTK3 folder chooser opened over the dimmed native window
  ([chooser](appimage-native-wayland/f44-candidate-gtk-folder-chooser.png)).
- **Candidate, editor:** the editor was native with bradient frames. It clears
  the desktop identity before Qt starts, so Qt does not pick adwaita, as in the
  DEB/RPM. Save a copy opened modal over it
  ([editor](appimage-native-wayland/f44-candidate-editor-bradient-save-a-copy.png)).
- **Candidate, forced `QT_QPA_PLATFORM=wayland`:** the view (adwaita) and the
  editor (bradient) were native, and `coredumpctl` listed no core since the
  start.
- **Not run: GNOME region capture.** It runs in gnome-shell through the put.gd
  Shell extension, which is not installed on f44 and loads only after a
  re-login. This change does not touch that route: the GNOME resident is Qt
  Core, and the selection is drawn by the shell. The resident's capabilities
  reported the helper unavailable, as before.

### k72, Plasma X11

- **Candidate:** unchanged. Settings mapped only `libqxcb.so` and was an X11
  window, and the X11 region overlay captured an 800x500 PNG
  ([selecting](appimage-native-wayland/k72-candidate-x11-region-selecting.png)).

## What changes for users

- On Wayland sessions the image's windows are now native, as with the DEB/RPM,
  and KDE region capture works from the AppImage.
- A Wayland compositor takes a window's icon from the desktop entry named by its
  app ID (`putgd-ui`), not from the window, and Qt 6.8 has no
  `xdg-toplevel-icon` support. With no host entry for the image:
  - KWin's title bar shows its generic Wayland icon.
  - GNOME's overview and dash show a generic icon
    ([overview](appimage-native-wayland/f44-candidate-overview-generic-icon.png)).
  - In the candidate run, Plasma's task manager still showed the put.gd icon;
    with the extracted rehearsal tree it showed the generic one. The difference
    was not investigated.
  - On XWayland the icon came from the window itself. The DEB/RPM install
    `putgd-ui.desktop` and are not affected.

## Not covered and follow-ups

- **Window icons:** a host entry for the AppImage's `putgd-ui` app ID would
  restore them. Options include a temporary entry from AppRun on Plasma, where
  AppRun already supervises, or `StartupWMClass=putgd-ui` in the embedded entry
  for integrated installs. This touches AppRun, which #75 also changes, so it is
  left to a separate change.
- **GNOME own-window refusal:** the extension refuses to capture put.gd's own
  windows by comparing window PIDs. Those windows are now native Wayland clients
  rather than XWayland ones. Mutter should report the same PID, but that check
  has not been run with native windows.
- **Recorder windows (#75):** with #75, `putgd-record`'s control bar, review
  window and KDE layer-shell overlay also go native in the AppImage. #75's
  AppImage README paragraph on the XWayland fallback then needs updating.
- **Not measured:** idle and family memory of the image on Wayland.
- **Not run:** Cinnamon (X11, same path as k72).
