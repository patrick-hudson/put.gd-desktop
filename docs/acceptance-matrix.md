# Linux acceptance coverage and execution placement

The required Linux test distributions are **Linux Mint, Ubuntu, Kubuntu, Fedora
and Debian**. Cover Cinnamon plus GNOME and KDE/Plasma, and real X11 and Wayland
sessions. Use a combination of the physical build servers and VMs that provides
this coverage on the exact candidate being accepted. This document sets required
coverage; it does not establish that any row has passed.

## Distribution coverage

| Distribution | Desktop/session baseline | Preferred execution placement |
| --- | --- | --- |
| Linux Mint | Cinnamon/X11 | Physical Mint 22.3 Cinnamon/X11 server `.74` (m74) for gates requiring physical Cinnamon; fresh VM on a capable build server for other versions; local VM fallback when needed |
| Ubuntu | GNOME/Wayland, with GNOME/X11 coverage on a release that supplies it | Builder-hosted VM of the required Ubuntu version, or an appropriate physical Ubuntu machine |
| Kubuntu | KDE/Plasma Wayland, with KDE/X11 coverage on a release that supplies it | Builder-hosted VM of the required Kubuntu version, or an appropriate physical Kubuntu machine |
| Fedora | GNOME/Wayland on the existing Fedora 44 server; other declared variants need their matching session/profile | Direct Fedora server desktop when its version matches the gate; VM for missing variants/versions |
| Debian | KDE/Plasma Wayland on the existing Debian 13 server; other declared variants need their matching session/profile | Direct Debian server desktop when its version matches the gate; VM for missing variants/versions |

Across that distribution coverage, require GNOME/Wayland, GNOME/X11,
KDE/Wayland and KDE/X11 results, as well as Mint Cinnamon/X11. Use native distro
sessions and supported packages. This is coverage across supported combinations;
it does not assume that every distro version ships every desktop/session pair.
Choose and record the exact distro release for each row before a campaign.

The current checked-in VM profiles are `cinnamon-x11`, `gnome-wayland`,
`kde-wayland` and `fedora-gnome-wayland` under `tools/lab/profiles/`. They do not
provide GNOME/X11, KDE/X11 or a Debian-specific automated profile. Provision the
missing VM/profile or use an appropriate native machine and retain manual
acceptance evidence. Do not relabel an existing profile as another distro or
session. An unavailable required combination remains a recorded gate.

`aurora-kde-wayland` (UEFI, `install: appimage`) is the Universal Blue Aurora
(Fedora Atomic KDE) VM; see [Aurora VM](aurora-vm.md). It has no row in the
package matrix above, because Aurora installs no DEB or RPM, and it doesn't
replace the Kubuntu or Debian KDE/Wayland evidence. It is a required VM desktop,
alongside the Ubuntu GNOME VM, in the
[AppImage full-acceptance profile](#appimage-full-acceptance-profile) below. The
Flatpak's acceptance will use it too. The profile has two goldens on `f44`, one
per Fedora base, and the runner takes either through `--golden`:

| Aurora base | Golden | Plasma / KWin | Role |
| --- | --- | --- | --- |
| Fedora 45 (`ghcr.io/ublue-os/aurora:testing`, `testing-45.20261022.1`) | `aurora-testing-45.20261022.1-852d34e885c1` | 6.7.5 / 6.7.5, kernel 7.2.11 | **Required** for the KDE/Wayland AppImage checks. #478 reproduced only on this base. |
| Fedora 44 (`ghcr.io/ublue-os/aurora:stable`, `44.20260908.1`) | `aurora-stable-44.20260908.1-v3-492c3f8945d1` | 6.7.4 / 6.7.4, kernel 7.1.8 | The base the `stable` tag still shipped on 2026-10-22; run it when a pass compares the two bases or a finding is base-specific. |

XWayland or `QT_QPA_PLATFORM=xcb` inside a Wayland session does not establish a
native GNOME/X11 or KDE/X11 result. Verify the actual login session/compositor
and Qt platform, along with the OS and desktop versions, in the evidence.

## Screen recording rows

Recording ([recording](recording.md)) is accepted per package format, because
each format brings its own media stack: the DEBs and the RPM use the distro's
GStreamer and FFmpeg, while the AppImage carries its own and uses only the host's
VA-API, PipeWire and GPU drivers. Each row installs the exact package (or runs
the exact AppImage) and records a moving pattern through the production CLI:
prepare, start, pause, resume, stop, edit (trim and a timed blur), preview,
save copy and Upload to an isolated local destination, for MP4, WebM, GIF and
WebP. It keeps screenshots of the ready, recording and review states, the
capabilities JSON, the installed identity, and each clip's motion score next
to a static control from the same desktop.

| Package | Desktop/session | Placement | What the row must show |
| --- | --- | --- | --- |
| Ubuntu 24.04 DEB | Mint 22.3 Cinnamon/X11 | physical `.74` (m74), or a Cinnamon VM on a builder | `ximagesrc` capture with the distro encoders |
| Ubuntu 26.04 DEB | Kubuntu 26.04 Plasma/X11 | physical `.72` switched to its Plasma X11 session for the check, or a Kubuntu 26.04 X11 VM | `ximagesrc` capture; VA-API live encoding |
| Ubuntu 26.04 DEB | Ubuntu 26.04 GNOME/Wayland, Kubuntu 26.04 Plasma/Wayland | physical `.72` (Plasma 6.6 Wayland, its default session) for Plasma; VMs on a builder for GNOME | Mutter and KWin native streams from the installed helper; the portal path |
| Debian 13 DEB (`putgd-VERSION-debian13-x86_64.deb`) | Debian 13 Plasma/Wayland | physical `.69` | `kde-screencast` from the installed `putgd-record.desktop`; the portal path |
| Fedora 44 RPM | Fedora 44 GNOME/Wayland | physical `.70` | `mutter-screencast` and the portal; a stock install pulling Cisco's OpenH264 through the RPM's Recommends; the actionable `h264_encoder_unusable` message with the `noopenh264` placeholder |
| AppImage | the Kubuntu, Debian and Fedora physical desktops above | physical | every plugin loaded from the image; the host GPU encoder used where it works and the bundled x264 where it does not (stock Fedora); AppRun's KWin entry giving `kde-screencast` on Plasma/Wayland and removed on exit |

Each DEB installs only on the release it was built for. The Ubuntu 24.04 and
26.04 DEBs require FFmpeg 6.1 and 8.0 sonames (`libavcodec60`, `libavcodec62`),
and Debian 13 ships 7.1, so Debian rows install the release `debian13` DEB, built
in a Debian 13 container against `libavcodec61` (issue #85). Before that asset
existed they used a DEB built on the Debian host.

## AppImage full-acceptance profile

One pass of the exact published AppImage on every desktop below, with every item
evidenced on every desktop. A failed item is a finding and gets its own issue; the
pass still reports it as a fail. An item that can't run on a desktop needs a
written reason, and Patrick decides whether the pass counts as complete. Nothing
is skipped silently. The passes so far are
[2026-10-21, nightly 842](evidence/2026-10-21-appimage-full-acceptance/README.md) and
[2026-10-22, nightly 942](evidence/2026-10-22-appimage-full-acceptance-rerun/README.md).

| Desktop | Placement |
| --- | --- |
| Fedora 44 GNOME Wayland | physical `.70` (f44), with the release's own `putgd-gnome50` extension as a user copy |
| Debian 13 Plasma 6.3 Wayland | physical `.69` (d69) |
| Kubuntu 26.04 Plasma 6.6 Wayland | physical `.72` (k72) |
| Linux Mint 22.3 Cinnamon X11 | physical `.74` (m74) |
| Ubuntu 26.04 GNOME Wayland | VM: an overlay of `gnome-provisioned-20261016` on f44 or d69. Remove that golden's old put.gd DEB first, and install the release's extension. |
| Aurora 45 (Universal Blue, Fedora Atomic KDE, Fedora 45 base) | VM: an overlay of `aurora-testing-45.20261022.1-852d34e885c1` on f44 ([Aurora VM](aurora-vm.md#fedora-45-golden)). Required. |
| Aurora 44 (Fedora 44 base) | VM: an overlay of `aurora-stable-44.20260908.1-v3-492c3f8945d1` on f44. Optional second Aurora row; its updater pulls a new `stable` image two minutes after boot unless `uupd.timer` is stopped first ([Aurora VM](aurora-vm.md#automatic-updates)). |

Each desktop uses a private profile: `XDG_CONFIG_HOME`, `XDG_STATE_HOME`,
`XDG_CACHE_HOME` and `XDG_RUNTIME_DIR` of its own. The data home is the user's,
because AppRun registers KWin's permissions and the Wayland icon entries there.
Uploads go to the loopback S3 fixture (`tests/fixtures/s3_server.py`) and never to
a real bucket. Physical desktops need a coordinator lock, a quiet host for the
memory window and the coordinator's OK for each logout.

- **A. Identity:** the AppImage is downloaded from its GitHub release.
  - Its SHA-256 matches `SHA256SUMS`, and `--version-json` reports the release tag.
  - `--appimage-updateinformation` is `zsync|https://app.put.gd/dl/nightly/appimage.zsync` (stable releases: the stable channel).
  - The image root holds exactly one desktop entry, `putgd.desktop`, without `NoDisplay`.
  - Record the OS, desktop and compositor versions, the type of the real login session (`loginctl`, not XWayland) and the Qt platform plugin each put.gd process loads.
- **B. Start:** the AppImage starts from a terminal or the file manager with FUSE, with no extra installs on Aurora.
  - The resident, `putgd-ui` and `session.sock` are up, and `putgd-ui` uses the native platform on Wayland.
  - The tray icon is present, and its menu opens with a real click.
  - The put.gd icon shows in the title bar (where the window decoration draws app icons) and in the task switcher.
- **C. Idle memory:** after a 180 s idle window, the resident's RSS and PSS are under the 40 MiB budget.
  - No GTK, KDE platform theme, `libssl` or `libQt6Network` is mapped in the resident.
- **D. Capture:** region, window and desktop captures each produce a PNG with a history entry.
  - **Edit & upload:** a blur and a solid redaction, checked in the uploaded pixels, then Upload.
  - **Save a copy…** opens the desktop's portal chooser (the themed fallback only where no portal FileChooser exists), and the saved file exists.
  - The link is copied according to the clipboard setting: each link on every desktop, plus the list behaviour on at least one.
- **E. Recording:** a moving pattern, recorded through the production path: prepare, start, pause, resume, stop.
  - In the Review window: a trim and a timed blur.
  - Export MP4, WebM, GIF and WebP, and save one copy through the chooser.
  - Each clip's motion score is compared with a static control recorded on the same desktop.
  - Record the live encoder (VA-API or the bundled x264).
  - On Plasma Wayland, AppRun's KWin entry gives `kde-screencast`, and the entry is removed on exit.
- **F. Notifications:** the link-ready notification appears, and a real click on it opens the link in the desktop's real default browser, which loads the image.
  - The image is a synthetic PNG uploaded public-read to the live S3 test prefix (`put.gd/acceptance/`, key set 2), so `https://put.gd/<key>` serves it. It's deleted afterwards with **Delete from destination**, and the prefix is listed empty.
  - The browser runs in a temporary profile, never one of Patrick's. Where the default `https` handler isn't a browser (the ChatGPT app on d69 and k72), Firefox is made the default for the test only, and `mimeapps.list` is restored byte for byte afterwards.
  - The frame shows the address bar and the image. A recorded `x-scheme-handler/https` handler (#326's recorder) stays as a supporting check of the opener's environment.
- **G. History:** entries show with previews, including a video poster, and **Copy image** puts the same pixels on the clipboard.
  - **Delete from destination…** removes the uploaded object from the fixture, and the row shows "Deleted from destination". The local file is kept by design.
- **H. Settings:** a theme switch (System, Dark, Light) repaints the open window live.
  - **Updates** shows **Channel** on *Nightly*, the build's own (*Stable* and *Nightly* are the only choices since #344), with a status from the live app.put.gd feed.
  - **Check now** works, and the nightly channel shows no notification.
- **I. Start at login:** turn on **Launch at login**, then do a real logout and login. Put.gd must start from the AppImage.
  - Physical desktops: only with the coordinator's OK, and only after checking that none of Patrick's apps are open.
  - VMs: through their autologin.
- **J. Update check:** `appimageupdatetool --check-for-update` against the published zsync gives the correct answer for the feed at that moment. The full N→N+1 update is #296's check.
- **K. Clean exit and cleanup:** quit from the tray, and nothing of put.gd keeps running.
  - The AppImage, the `putgd-appimage-*` and icon entries, the autostart entry, the profile and the caches are removed.
  - VM overlays, tunnels and wall tiles are removed, and locks are released with `--verified-clean`.

Evidence per desktop:
- a desktops × A–K pass/fail table, with versions, memory numbers, encoders and motion scores;
- annotated full-desktop pictures of the tray menu, Settings, a capture in the editor, the Review window, the native **Save a copy…** chooser and the link-ready notification;
- every frame checked before it is kept.

## Choose physical hardware or a VM for each gate

Prefer Patrick's [Fedora and Debian build servers](remote-builders.md), including
their own visible GNOME/Plasma desktops. Build and test directly there when the
host matches the required release/desktop/session. Provision a fresh VM on a
capable builder for missing distributions, versions or sessions. Discover/copy
[immutable golden inputs](remote-golden-images.md) and expose each remote guest
on the [workstation display wall](remote-vm-displays.md). Use local VMs when the
builders cannot run the profile, and record the fallback reason.

**A release gate that requires physical/native hardware must run on the
appropriate physical machine.** A VM result cannot close that gate. Mark the
required form factor before testing; record physical gates as blocked until the
appropriate machine is available. Do not downgrade hardware, display topology,
acceleration or compositor requirements to fit an available VM. VM functional
coverage and physical release evidence remain separately identifiable.

Check KVM, capacity and visible session access on the actual VM host before
launch. A server can still run direct desktop acceptance while its VM readiness
is blocked. Preserve the user's existing host projects/windows; use isolated
payloads, settings and test destinations.

## Ordering, evidence and status

Run a new candidate's rows in parallel, each with its functional and memory gates
(see [AGENTS.md](../AGENTS.md)). A scoped user
waiver applies only to its named task/gate and does not certify the full matrix.

Use the same exact accepted source/build candidate across the declared matrix.
For every result retain distro/release, desktop/compositor version, X11/Wayland
session, Qt platform, physical machine or VM identity, acceleration, installed
and running build identities, scenario parameters, outcome and blocked gates.
Keep graphical interactions visible on the wall and save screenshots/recordings,
including failures, alongside independent artifact/paste witnesses as required.
SSH logs, offscreen tests, a live tile or historical acceptance cannot certify a
new row/candidate. Existing performance, geometry and fresh-agent release gates
remain in [verification](verification.md); preserve their historical evidence.
