# Implementation evidence

## Required Linux coverage

The [acceptance matrix](acceptance-matrix.md) requires Mint, Ubuntu, Kubuntu,
Fedora and Debian, with Cinnamon plus GNOME/KDE and native X11/Wayland coverage.
Prefer the build servers for direct desktop tests and VM hosting; provision
missing profiles, and run physical-hardware release gates on physical machines.
This requirement is separate from the historical results below; missing new
rows and current-candidate evidence remain gates.

## Current state

Each row states only what the linked records say, with their dates. A new record
updates its row. Evidence for every record is indexed in
[put.gd-evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/main/records/README.md),
and the [history index](../history/README.md#verification-run-records) lists all
the moved records. **Unclear** marks a row where records disagree or no record
says which result is latest; nothing there counts as a pass.

| Area | Latest record | Result | Open or blocked gates |
| --- | --- | --- | --- |
| Stable release, exact draft files | [first stable acceptance, 2026-11-05](../history/verification/2026-11-05-first-stable-acceptance-v26-1105-3906.md) | Passed: stable scan and exact-asset audit; nightly desktop check 65 of 65 cells, one row from a rerun (#1214); install, upgrade, removal and fresh install of all four native packages; every graded Cinnamon performance limit; a two-head VM capture; AMD hardware encoding on the exact AppImage. | Full History traversal not measured to completion (#1066). The record says older records and the gates they call open predate it; it does not say which of those gates it closes. |
| Exact-asset, source and CVE audit | [audit rerun, 2026-11-01](../history/verification/2026-11-01-published-nightly-artifact-source-cve-audit-rerun.md), [first stable acceptance, 2026-11-05](../history/verification/2026-11-05-first-stable-acceptance-v26-1105-3906.md) | Nightly `v26.1101.3698-nightly`: zero errors, 180 findings, 31 accepted, zero blocking. The stable scan and exact-asset audit passed on the draft. | The nightly record is not legal, live-service, graphical or stable acceptance. **Unclear:** #419 (codec patents), #470, #471 and #475 were open on 2026-10-22 ([AppImage audit, 2026-10-22](../history/verification/2026-10-22-appimage-exact-asset-audit-rehearsal-383.md), [DEB/RPM audit, 2026-10-22](../history/verification/2026-10-22-deb-rpm-exact-asset-audit-rehearsal-383.md)); no later record mentions them. |
| Physical native gates, published nightly | [physical native gates, 2026-10-31](../history/verification/2026-10-31-published-nightly-physical-native-gates.md) | `v26.1031.3384-nightly`: five of six recipes passed on m74 and k72; Aurora stopped at probe preflight. Warm drawable paint (#1063) and History PageDown (#1064) failed their limits; those issues and #379 are closed, and the failures remain the results for that build. | Guarded-browser proof, physical GNOME window witnessing and Aurora Atomic probe admission were blocked (#1060–#1062), and populated Copy image/video/delete unassessed (#1066). #44 stayed open. **Unclear:** no later record says whether #1060–#1062 are resolved. |
| Physical Cinnamon performance | [first stable acceptance, 2026-11-05](../history/verification/2026-11-05-first-stable-acceptance-v26-1105-3906.md), [physical native gates, 2026-10-31](../history/verification/2026-10-31-published-nightly-physical-native-gates.md) | The stable record: every graded limit passed. | Full History traversal (#1066), as above. |
| Native package full acceptance (A–Q) | [native full acceptance, 2026-10-23](../history/verification/2026-10-23-native-package-full-acceptance-375-377-382.md), [#495 drivers, 2026-10-24](../history/verification/2026-10-24-native-acceptance-driver-hardening-495.md), [first stable acceptance, 2026-11-05](../history/verification/2026-11-05-first-stable-acceptance-v26-1105-3906.md) | `v26.1023.1058-nightly`: a historical nightly pass, with item P failing on m74 (#490) and findings #491–#495. The stable record lists install, upgrade, removal and fresh install. | **Unclear:** no later record reruns the whole A–Q profile or mentions #490. The driver record says it establishes no positive real logout/login or full package/release acceptance. |
| AppImage full acceptance (A–K) | [first stable gate plan, 2026-10-22](../history/verification/2026-10-22-first-stable-gate-plan.md), [AppImage audit, 2026-10-22](../history/verification/2026-10-22-appimage-exact-asset-audit-rehearsal-383.md) | First pass on nightly 842 failed F on five desktops and H and J on f44. A later A–K re-acceptance on six desktops is linked from the audit record, without its result. | **Unclear:** no record here states the result of a full A–K pass. The [matrix](acceptance-matrix.md#appimage-full-acceptance-profile) links the passes' evidence. |
| Linux update profile (U1–U16) | None | No record here states a result. | **Unclear:** the [matrix](acceptance-matrix.md#linux-update-profile-u1u16) points at evidence folders named `*-linux-update-acceptance`. |
| Screen recording | [#410, 2026-10-24](../history/verification/2026-10-24-recording-session-contract-410.md), [recording closeout, 2026-10-20](../history/verification/2026-10-20-recording-closeout-87-166-165-164-89.md), [#87 sweep, 2026-10-19](../history/verification/2026-10-19-screen-recording-acceptance-sweep-87.md) | #410: Cinnamon X11 and GNOME Wayland guests and physical Debian KDE Wayland passed six changed-behaviour cases and 180-second RSS/PSS windows. Closeout: all four formats on physical Mint Cinnamon X11, a two-output GNOME VM and physical Plasma 6.6 Wayland. | AMD and NVIDIA encode (#138) and the headphone and Bluetooth listening test (#212) were blocked or moved; native physical GNOME prerequisites blocked (#410); the d69 portal run and the k72 re-login and power-cycle checks pending (#164). **Unclear:** the stable record later reports AMD hardware encoding working; nothing later covers NVIDIA. |
| Resident and family memory | [integrated campaign, 2026-10-25](../history/verification/2026-10-25-integrated-linux-development-campaign.md), [#558, 2026-10-25](../history/verification/2026-10-25-history-open-memory-558.md) | Targeted campaign complete on CI source `6b7949f5`. History-open family PSS 46,641,152 B on physical Cinnamon. | Native Fedora post-burst RSS and GNOME46 AppImage whole-family RSS failed; History content endpoints unassessed; #558 not closed. **Unclear:** the stable record speaks only to graded Cinnamon limits, so no later record says whether these remain. |
| History gallery (#1151) | [#1151, 2026-11-02](../history/verification/2026-11-02-history-gallery-blanking-scrolling-and-gap-crash-1151.md) | With PR #1151's DEB on Mint Cinnamon X11, no sampled frame showed a blanked tile and the crash was gone; the scroll paint test also passes on live GNOME Wayland. | Plasma not run. Synthetic history, not live storage. |
| GUI, CLI and MCP parity, AE17, fresh-agent walkthrough (#378) | [first stable gate plan, 2026-10-22](../history/verification/2026-10-22-first-stable-gate-plan.md), [integrated campaign, 2026-10-25](../history/verification/2026-10-25-integrated-linux-development-campaign.md), [physical native gates, 2026-10-31](../history/verification/2026-10-31-published-nightly-physical-native-gates.md) | Partial on 2026-10-13 with CLI `dev.143`. | Open: the 2026-10-25 record does not close #378, and the 2026-10-31 record does not complete agent acceptance. |
| Multi-display geometry (#380, #381) | [first stable gate plan, 2026-10-22](../history/verification/2026-10-22-first-stable-gate-plan.md), [integrated campaign, 2026-10-25](../history/verification/2026-10-25-integrated-linux-development-campaign.md), [first stable acceptance, 2026-11-05](../history/verification/2026-11-05-first-stable-acceptance-v26-1105-3906.md) | #380 partial: three-head Cinnamon X11 region and single-output fractional scale; the stable record adds a two-head VM capture. | #381 physical mixed DPI and rotation blocked: no builder had two outputs; proposed for 0.1.2. The 2026-10-25 record does not close #380 or #381. |
| KDE/X11 rows, graphical-store installs (#385) | [first stable gate plan, 2026-10-22](../history/verification/2026-10-22-first-stable-gate-plan.md) | Proposed for 0.1.2; the record calls GNOME/X11 obsolete because GNOME 50 has no X11 session. | **Unclear:** the [matrix](acceptance-matrix.md#distribution-coverage) still lists GNOME/X11 on a release that supplies it. |
| Login-startup uninstall (#382) | [#382 contract, 2026-10-24](../history/verification/2026-10-24-login-startup-uninstall-contract-382.md) | Five native and six AppImage lifecycle rows passed on development artifacts; #382 closed by Patrick's decision, with the legacy exception. | Published-nightly lifecycle verification continues under #384. **Disagreement:** [native full acceptance, 2026-10-23](../history/verification/2026-10-23-native-package-full-acceptance-375-377-382.md) and the [matrix's item Q](acceptance-matrix.md#lifecycle-items-lq) describe the `TryExec` choice as undecided; the later #382 record says it was decided. |
| Clipboard on Wayland without a native adapter (#1105) | [#1105, 2026-11-01](../history/verification/2026-11-01-link-copied-on-wayland-desktops-with-no-native-adapter-1105.md) | COSMIC and Budgie uploads copied the link with matching object hashes; Pantheon completed with the problem `source_unsupported`. | Budgie's clipboard read and image write not run on a real desktop; GNOME, KDE and X11 guests not rerun. |
| Historical UI catalog comparison (#708; retired by PR #1326) | [#708, 2026-10-30](../history/verification/2026-10-30-ui-catalog-reference-images-708.md) | 218 states matched in both schemes, one exempt (#778); CI reproduced them. Synthetic widget evidence, not desktop acceptance. | Historical result only; the current [catalog check](../design/ui-catalog.md) renders without pixel comparison. |
| CI installs and flaky tests | [#351, 2026-10-22](../history/verification/2026-10-22-flaky-test-rethink-351.md), [#360, 2026-10-22](../history/verification/2026-10-22-weekly-stress-lane-on-k72-360.md), [#368, 2026-10-22](../history/verification/2026-10-22-package-install-retries-and-snapshot-368.md), [#467, 2026-10-23](../history/verification/2026-10-23-stalled-package-installs-467.md) | #351, #368 and #467 passed their CI, with demonstrations behaving as designed. The weekly lane was installed on k72 and completed a full run. | Not covered: `linux` and `sanitizers` retries (#389), an AppImage snapshot (#500), the Qt ASan download (#502). The weekly run failed `capture_process_integration` once (#398), and its tracker token was missing on 2026-10-22. |
| AppImage icons and editor window refusal | [#523, 2026-10-23](../history/verification/2026-10-23-appimage-window-icon-on-plasma-523.md), [X11 refusal, 2026-10-19](../history/verification/2026-10-19-x11-editor-window-refusal.md), [GNOME refusal, 2026-10-19](../history/verification/2026-10-19-gnome-editor-window-refusal.md), [Wayland icons, 2026-10-19](../history/verification/2026-10-19-appimage-wayland-window-icons.md) | Icon correct on k72, d69 and neon User; editor windows refused on Kubuntu Plasma X11 and Fedora GNOME Wayland. | Not covered: neon Unstable, Cinnamon, recorder windows. A launch whose `XDG_DATA_HOME` differs from the compositor's loses KDE capture (#531). |
| Build, cold capture, live S3, release and fresh-agent (#3, #4, #5) | [acceptance index, 2026-10-13](../history/verification/2026-10-13-current-acceptance-index.md), [live-S3 regression, 2026-10-12](../history/verification/2026-10-12-historical-live-s3-regression.md) | Each row of the 2026-10-13 index gives its scope and limit. Runtime `0.1.1-dev.114` passed all three live-S3 regression workloads on 2026-10-12. | The index's own limits. Later records cite their own CI runs; none restates these rows. |
| Proton | [Proton reconciliation, 2026-10-15](../history/verification/2026-10-15-proton-evidence-reconciliation.md) | Experimental packaged candidate `a5112bc`: Cinnamon-first functional and memory acceptance, then GNOME and KDE, with Fedora and AppImage smoke checks. | No tagged release, full hosted CI, physical or current-main certification claimed as of 2026-10-15. |
| Focused development checks, 2026-10-14 to 2026-10-18 | The [history index](../history/README.md#verification-run-records): #26, #27, #33, #24, #36, #40, #51, #56, #58, destination and S3 controls | Each record gives its own scope and result. | Several say later graphical or memory acceptance is pending; no later record closes or reopens them individually. |

Full release readiness requires the current [acceptance matrix](acceptance-matrix.md)
and [release runbook](../release/release-runbook.md). The
[original plan](../history/plans/2026-10-07-1926-feat-qt-linux-share-plan.md) and its closed
[bootstrap issue](https://github.com/patrick-hudson/put.gd-desktop/issues/1) are historical context.
[Lab storage safeguards](../lab/lab-storage.md) gate capacity and verified retention.

## Blocked gates

These are the gates the records leave open or blocked, as they last stated them.
The [stable record](../history/verification/2026-11-05-first-stable-acceptance-v26-1105-3906.md) says everything older predates that
release, including gates it describes as open, without listing which it closed.
An item here is therefore not proven open today; check its issue before relying on
it. Nothing here counts as a pass.

| Gate | Last stated | As stated |
| --- | --- | --- |
| Full History traversal (#1066) | 2026-11-05 | Not measured to completion on the stable draft. |
| Populated Copy image/video/delete (#1066) | 2026-10-31 | Unassessed at the current AT-SPI boundary. |
| Guarded-browser proof, physical GNOME window witnessing, Aurora Atomic probe admission (#1060–#1062) | 2026-10-31 | Blocked. |
| First stable gate table (#44) | 2026-10-31 | #44 stays open. |
| Physical mixed DPI and rotation (#381) | 2026-10-22 | Blocked: no builder has two outputs; proposed for 0.1.2. |
| Multi-display geometry in VMs (#380) | 2026-10-25 | Not closed by that record; the stable record later adds a two-head VM capture. |
| GUI/CLI/MCP parity, AE17, fresh-agent walkthrough (#378) | 2026-10-31 | Partial; agent acceptance not complete. |
| KDE/X11 rows, graphical-store installs (#385) | 2026-10-22 | Proposed for 0.1.2. |
| Native Fedora post-burst RSS, GNOME46 AppImage whole-family RSS | 2026-10-25 | Failures. |
| History content endpoints | 2026-10-25 | Unassessed despite approved thresholds. |
| History-open parallel desktops, paint-response timing, published assets (#558) | 2026-10-25 | Pending. |
| Native physical GNOME prerequisites | 2026-10-24 | Blocked; guest proof is not physical or full-release acceptance. |
| NVIDIA encode (#138) | 2026-10-20 | Blocked or moved. The stable record covers AMD only. |
| Headphone and Bluetooth listening test (#212) | 2026-10-20 | Blocked or moved. |
| Microphone capture | 2026-10-18 | Uncovered. |
| Published-nightly login-startup lifecycle, AppImage replacement with real channel metadata (#384) | 2026-10-24 | Continues under #384. |
| License and legal review (#383, #511, #384) | 2026-11-01 | The audit is artifact and source evidence, not legal acceptance. #419's patent decision has no later statement. |

The performance, geometry and fresh-agent release gates the
[acceptance matrix](acceptance-matrix.md#ordering-evidence-and-status) refers to are
the rows above. A release gate that needs physical hardware stays blocked until the
appropriate machine has run it; a VM result cannot close it.

## Where new records go

A new acceptance record is a directory published to
[put.gd-evidence](https://github.com/patrick-hudson/put.gd-evidence) with
`python3 tools/maintenance/evidence.py publish <dir>`. Its `README.md` is the
acceptance summary. This page changes only when the current state or a blocked gate
changes. Dated records that used to be on this page are in
[the history index](../history/README.md#verification-run-records); see
[evidence retention](evidence-retention.md).
