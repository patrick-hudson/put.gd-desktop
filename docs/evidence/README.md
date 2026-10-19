# Verification evidence index

These records certify the source identities and workloads written inside them,
not every later commit. See [retention and restoration](../evidence-retention.md)
for local bulk artifacts and [verification](../verification.md) for limitations.

| Topic | Starting point |
| --- | --- |
| Screen recording in every package (issue #50 packaging) | [Installed DEB/RPM and AppImage runs, AppImage size and memory](2026-10-18-recording-packaging.md) |
| AppImage native Wayland windows (Qt xdg-shell, decorations) | [Builder check, d69/f44/k72 desktop acceptance](2026-10-19-appimage-native-wayland.md) |
| AppImage screen recording on Wayland after native windows (nightly `9042776`) | [d69/f44 Wayland and k72 X11 runs, placement, native windows, motion vs frozen controls](2026-10-19-appimage-recording-wayland.md) |
| AppImage Wayland window icons and GNOME own-window refusal (issue #88) | [d69 KWin and f44 GNOME icons, entry cleanup, Settings/History refusal](2026-10-19-appimage-wayland-icons.md) |
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
