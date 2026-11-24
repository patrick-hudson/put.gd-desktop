# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## First stable gate plan — 2026-10-22

No release asset has been accepted yet. The first stable switches to calendar
versions `vYY.MDD.N` first ([#373](https://github.com/patrick-hudson/put.gd-desktop/issues/373)),
so it won't be `v0.1.1`. There is no candidate channel: merging the promotion
tags `vYY.MDD.N` and stages the stable draft, and exact-asset acceptance runs on
that draft before it is published. If acceptance fails, the tag stays unreleased
and the next promotion gets a new version.
[Issue #44](https://github.com/patrick-hudson/put.gd-desktop/issues/44) holds the
current gate table, the proposal for what the first stable requires or defers, and these
work packages:

| Gate | Issue | Status on 2026-10-22 |
| --- | --- | --- |
| Calendar versions `YY.MDD.N`, prerequisite for promotion | [#373](https://github.com/patrick-hudson/put.gd-desktop/issues/373) | design approved 2026-10-22 (one code PR, no `-rc`); implementation in progress |
| AppImage full acceptance A–K, six desktops | [profile](../../testing/acceptance-matrix.md#appimage-full-acceptance-profile), [first pass](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-21-appimage-full-acceptance/README.md) | first pass on nightly 842 failed F (5 desktops), H and J (f44); rerun pending |
| DEB lifecycle, recording rows, live S3 | [#375](https://github.com/patrick-hudson/put.gd-desktop/issues/375) | partial: source144 Cinnamon VM only; no state-preserving upgrade or removal + login on Wayland; schema 5→7 never exercised |
| RPM lifecycle on f44 | [#377](https://github.com/patrick-hudson/put.gd-desktop/issues/377) | partial: repo upgrade 816→828 only |
| GUI/CLI/MCP parity, AE17, fresh-agent walkthrough | [#378](https://github.com/patrick-hudson/put.gd-desktop/issues/378) | partial: 2026-10-13, CLI `dev.143` |
| Physical Cinnamon latency, 10,000-entry history, burst, slow network | [#379](https://github.com/patrick-hudson/put.gd-desktop/issues/379) | latency stale (current63, 121.273 ms p95); history/burst/slow network not started |
| Multi-display geometry in VMs | [#380](https://github.com/patrick-hudson/put.gd-desktop/issues/380) | partial: 3-head Cinnamon X11 region, single-output fractional scale |
| Physical mixed DPI and rotation | [#381](https://github.com/patrick-hudson/put.gd-desktop/issues/381) | blocked: no builder has two outputs; proposed for 0.1.2 |
| Uninstall login-startup entry | [#382](https://github.com/patrick-hudson/put.gd-desktop/issues/382) | closed by Patrick; 11 development-artifact rows pass, legacy exception accepted; PR integration and #384 release checks remain ([evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-24-uninstall-startup-382/README.md)) |
| License, notices, provenance, dependencies | [#383](https://github.com/patrick-hudson/put.gd-desktop/issues/383) | AppImage rehearsal on `v26.1022.968-nightly` ([below](2026-10-22-appimage-exact-asset-audit-rehearsal-383.md#appimage-exact-asset-audit-rehearsal-383--2026-10-22)): provenance passes; notices, complete source, Qt CVEs and codec patents open (#416–#419); DEB/RPM not yet audited |
| Release campaign and stable signing | [#384](https://github.com/patrick-hudson/put.gd-desktop/issues/384) | promote.py dry run passed on `0435ed6a` under the old scheme; repeat after #373 |
| KDE/X11 rows, graphical-store installs | [#385](https://github.com/patrick-hudson/put.gd-desktop/issues/385) | proposed for 0.1.2; GNOME/X11 obsolete (GNOME 50 has no X11 session) |

Supported packages are DEB, RPM and AppImage; other package experiments are out of scope.
