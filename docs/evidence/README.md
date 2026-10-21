# Verification evidence index

These records certify the source identities and workloads written inside them,
not every later commit. See [retention and restoration](../evidence-retention.md)
for local bulk artifacts and [verification](../verification.md) for limitations.

| Topic | Starting point |
| --- | --- |
| File dialogs through the XDG portal, themed fallback (issue #259) | [Save copy… opening the GTK, KDE and GNOME choosers on m74 Cinnamon X11, d69 Plasma Wayland and f44 GNOME Wayland in dark mode, the themed fallback in Dark and Light; resident unchanged, putgd-record +0.12 MiB with the portal chooser open against +5.4 MiB for Qt's dialog](2026-10-21-file-dialogs-259/README.md) |
| Checked, disabled indicators and the named update channel (issues #300, #305) | [Specimen card before/after in light and dark; Settings → Updates on a stable- and a nightly-tagged build before/after; new gui_theme and gui_settings tests on d69](2026-10-21-checkbox-channel-300-305/README.md) |
| Tray "Update available" entry shown live (issue #304) | [Scheduled check then the tray menu without a restart on d69 Plasma, f44 GNOME and m74 Cinnamon, before/after pairs; m74 notifications now come from xfce4-notifyd (#312)](2026-10-21-tray-update-visible-304/README.md) |
| Update check and AppImage updates on real desktops (issues #303, #295) | [Memory gate on f44 GNOME and d69 Plasma (+0.09/+0.14 MiB), stable scheduled check, one notification and tray entry on m74/f44/d69 (#304: no live tray entry on Plasma/GNOME), nightly without notification; Gear Lever, AppImageLauncher and start at login in a Mint 22.3 guest (#306, #307)](2026-10-21-updates-acceptance-303/README.md) |
| AppImage zsync update information and `.zsync` asset (issue #293) | [Three CI-equivalent AppImage builds on f44; `appimageupdatetool` moves build A to the next commit's B through a local Range server on Fedora 44 and Ubuntu 24.04 (95.5 % reused, 5.6 of 109.5 MiB fetched, B's exact bytes); tagged nightly readback and `collect.py` binding](2026-10-20-appimage-zsync-293/README.md) |
| Review window: the video first, one timeline, contextual mask tools (issue #283) | [d69 Plasma Wayland and m74 Cinnamon X11 before/after in both schemes (video 28.9 % → 70.0 % of the window), MCP end to end with `drive_recording_review`, upload to the S3 fixture, motion 18.3 vs 0.875](2026-10-20-review-window-283/README.md) |
| Review window icon, rail after a theme switch, theme tests and the session bus (issues #264, #261, #251) | [Cinnamon panel button before/after with and without a desktop entry, `_NET_WM_ICON` added; rail fresh vs switched (2 px → identical); gui tests under a light-portal bus before/after](2026-10-20-quick-knockouts-264-261-251/README.md) |
| Spin boxes, instant theme switch, Diagnostics on Advanced, upload wording (issues #233, #237, #238, #239) | [Spin box chevrons in both schemes, pick-to-repaint timing before/after on m74/d69/f44 (1921 → 65 ms, 1744 → 38 ms, 1711 → 38 ms), Settings tabs and rail/File names before/after](2026-10-20-ui-polish-233-237-238-239/README.md) |
| Recording closeout (issues #87, #166, #165, #164, #89) | [Mint 24.04 DEB on physical Cinnamon, two-output Ubuntu 26.04 GNOME guest, k72 Plasma 6.6 DEB and AppImage, portal pause A/V before/after, the pinned lab cover, d69 wall watchdog, GPU hang table](2026-10-20-recording-closeout.md) |
| CI PipeWire 1.6 check, worker-thread stack overflows, debug asset size (issues #144, #156, #162) | [f44 container before/after for the `pipewire` job, f44 worker overflow before/after, size of each packing option and a symbolized release record](2026-10-20-ci-reliability-round8.md) |
| Debug logging and Fedora's `qtlogging.ini` (issue #191) | [f44 before/after under `*.debug=false`, Debian runs with the suite's simulated rule](2026-10-20-logging-fedora-191.md) |
| Recording Stop, pause/resume A/V, bar above the dim, outdated GNOME extension (issues #129, #130, #127, #131) | [Headless Stop reproducer and Kubuntu/GNOME guests, f44 Mutter and d69 KWin pause A/V before/after, d69 bar-over-dim frames](2026-10-20-recording-reliability.md) |
| AppImage recording on Plasma 6.6 Wayland (issue #137) | [Cause (pipewiresrc `min-buffers` vs KWin's 2-4 buffers on PipeWire 1.5.81+), headless reproducer matrix, k72 Plasma 6.6 Wayland `kde-screencast`/portal, d69/f44/m74 and DEB/RPM regressions with A/V and motion](2026-10-20-appimage-plasma66-137.md) |
| Screen recording acceptance sweep (issue #87) | [AppImage A/V on k72/d69/f44 incl. pause/resume, #81 native windows, packaged GNOME extension, MCP on three desktops, Ubuntu 26.04 GNOME/Plasma guests, two-head Wayland guests, physical Plasma 6.6 Wayland on k72, share URLs in Firefox/Chromium](2026-10-19-recording-acceptance-87.md) |
| UI follow-ups: diagnostics upload, failure text, notification buttons, Auto limits, tooltip (issues #158, #152, #151, #157, #149, #175) | [m74 Cinnamon resident notifications and plain-words failure, package upload to the offline S3 fixture; d69/k72 Plasma drag cursor (not reproduced) with Wayland traces; d69 Auto limits and dialog reasons](2026-10-20-ui-followups.md) |
| Settings diagnostics controls and destination drag (issues #122, #133, #134, #135) | [m74 Cinnamon failed upload → Save diagnostics → Show in folder; f44 GNOME 50, d69 Plasma 6.3 and k72 Plasma 6.6 Wayland drags, elided long names, package dialog](2026-10-20-settings-diagnostics.md) |
| Destination reorder in Settings (issue #59) | [Suites and Cinnamon X11 guest: native drag, drop line, Alt+Down, ⋯ menu, `ui.moveDestination`, `settings.reorderProfiles`, restart](2026-10-19-destination-reorder.md) |
| Recording bar audio box and WebM Opus decode (issues #120, #119) | [f44 GNOME, d69 KWin 6.3 Wayland and k72 X11 bar frames with sound on/off; FFmpeg 8.0 parser cause, decode test, Firefox and Chromium](2026-10-19-recording-fixes-119-120.md) |
| Wayland recording A/V sync (issue #86) | [d69 KWin/portal, f44 Mutter/portal and k72 X11: working clip vs export offsets, timestamp A/B, before/after](2026-10-19-wayland-av-sync.md) |
| Recording control bar placement and handle (issue #108) | [k72 X11, d69 KWin 6.3, f44 GNOME and a two-monitor Cinnamon guest: placement, frame checks, motion](2026-10-19-recording-bar-placement.md) |
| Screen recording in every package (issue #50 packaging) | [Installed DEB/RPM and AppImage runs, AppImage size and memory](2026-10-18-recording-packaging.md) |
| Debian 13 release DEB (issue #85) | [CI build and audit, unchanged Ubuntu/Fedora packages, d69 install, recording and uninstall](2026-10-19-debian13-deb.md) |
| AppImage native Wayland windows (Qt xdg-shell, decorations) | [Builder check, d69/f44/k72 desktop acceptance](2026-10-19-appimage-native-wayland.md) |
| AppImage screen recording on Wayland after native windows (nightly `9042776`) | [d69/f44 Wayland and k72 X11 runs, placement, native windows, motion vs frozen controls](2026-10-19-appimage-recording-wayland.md) |
| AppImage Wayland window icons and GNOME own-window refusal (issue #88) | [d69 KWin and f44 GNOME icons, entry cleanup, Settings/History refusal](2026-10-19-appimage-wayland-icons.md) |
| X11 window capture refuses the editor (issue #103, X11 half) | [k72 Plasma X11 DEB and AppImage: editor, Settings and History refused, external captured](2026-10-19-x11-editor-refusal.md) |
| GNOME window capture refuses the editor (issue #103) | [f44 RPM and AppImage: editor, Settings and History refused, external captured](2026-10-19-gnome-editor-refusal.md) |
| Settings save crash on Debian 13 (dangling `QJsonValueRef`) | [Suites, source contract and Debian Plasma before/after](2026-10-19-settings-validate-uaf.md) |
| Single-name configuration and data folders (issue #56) | [Suites, interrupted-move coverage and Cinnamon upgrade check](2026-10-18-issue56-storage-locations.md) |
| Inherited capture output in Settings (issue #58) | [Suites and Cinnamon check](2026-10-18-issue58-inherited-capture-output.md) |
| AppImage image worker on Qt's offscreen platform | [Cause, builder audit and Kubuntu Plasma/X11 before/after](2026-10-19-appimage-image-job.md) |
| Screen recording on Linux (issue #50) | [Suites, X11 and Wayland desktop acceptance](2026-10-18-issue50-screen-recording.md) |
| Resident PNG readers; libpng writer measured and dropped | [Decision, tests and Cinnamon/Debian A/B](2026-10-17-resident-png-codecs.md) |
| X11 region capture compositor settle (issue #54) | [Diagnosis, Cinnamon and physical KDE/X11 acceptance](2026-10-17-x11-compositor-settle.md) |
| Lift/duotone identity and upload feedback | [Current native, provider and physical validation](2026-10-16-lift-duotone-validation.md) |
| Proton experimental integration recovery and resources | [Release gates](../proton-drive-release-gates.md), [P1 recovery](2026-10-15-proton-p1-app-recovery.md), [P2 resources](2026-10-15-proton-p2-resources.md) |
| Integrated process split and memory strategy | [Memory strategy](../memory-strategy.md) |
| Later cross-desktop live-S3 fix and memory checkpoint | [Source114 replay, identities and cleanup](live-s3-memory/source114/README.md) |
| S3 transport failure diagnosis and corrected regression | [Transport report](../testing-live-s3-transport.md) |
| Earlier KDE optimization and cumulative local-memory changes | [KDE reproduction](kde-source63/reproduction.md), [KDE report](../testing-kde-memory.md) |
| Earlier GNOME boundary and Cinnamon regressions | [GNOME report](../testing-gnome-memory.md) |
| Earlier native-listener/capture experiments | [Native listener testing](../testing-native-listener.md) |
| Release package evidence and acceptance classes | [Release runbook](../release.md), [CI coverage](../ci.md) |

Dated `*-raw` directories contain supporting historical receipts. Their size or
age does not establish that they can be deleted. In particular, rejected
candidates and infrastructure failures are part of the explanation for the
accepted changes. Source114's sanitized compressed evidence has its own manifest
and replay validator; preserve that format and its hashes.

GitHub Actions artifacts and private local release receipts are additional
operational evidence, with their own retention limits. A green nightly package
run establishes automated build/package checks, not fresh desktop acceptance.
Never copy credentials, login inputs or unredacted private work into this index.
