# Historical documentation

These pages preserve superseded designs, experiments, plans and source-specific
acceptance records. “Historical” means they describe their recorded version,
not today's implementation or a work list. Start with [overview](../overview.md)
for the current app and follow the replacement links below.

Follow [evidence retention](../evidence-retention.md): age alone never justifies
deletion. Preserve acceptance summaries, exact source/build identities,
reproducible parameters, failures and resolutions, raw evidence references and
cleanup receipts. Compress cold bulk evidence only with verified checksums and
a restore index. Evidence directories stay under [docs/evidence](../evidence/README.md);
none are moved here. Historical Markdown gets an explicit banner; the three JSON
review receipts are preserved byte-for-byte and classified by this index so their
machine-readable content and provenance remain intact.

## Windows

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [windows-vm-lab.md](windows-vm-lab.md) | Windows QEMU/base lifecycle | Before support was dropped on 2026-10-31 | [vm-lab](../vm-lab.md) |

## Flatpak

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [flatpak.md](../flatpak.md) | Unsupported sandbox/package experiment; original path retained for code references | October 2026 experiment; out of scope on 2026-10-31 | [release](../release.md) |

## Memory and process experiments (source49/57/63/114 era)

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [native-listener.md](native-listener.md) | Cinnamon Core/native-listener boundary | 2026-10-09–11 | [memory-strategy](../memory-strategy.md) |
| [gnome-core-resident.md](gnome-core-resident.md) | GNOME Core resident transition | 2026-10-10 | [architecture](../architecture.md) |
| [kde-resident-boundary.md](kde-resident-boundary.md) | KDE resident/selector boundary | 2026-10-10–11 | [architecture](../architecture.md) |
| [resident-ui-process.md](resident-ui-process.md) | Resident/UI split investigation | 2026-10-08–11 | [architecture](../architecture.md) |
| [testing-capture-parity.md](testing-capture-parity.md) | Capture startup regression investigation | 2026-10-09 | [overview](../overview.md) |
| [testing-gnome-memory.md](testing-gnome-memory.md) | GNOME experiment measurements | 2026-10-10 | [memory-strategy](../memory-strategy.md) |
| [testing-kde-memory.md](testing-kde-memory.md) | KDE experiment measurements | 2026-10-10–11 | [memory-strategy](../memory-strategy.md) |
| [testing-live-s3-memory.md](testing-live-s3-memory.md) | Live-S3 post-use memory campaign | 2026-10-11–12 | [memory-strategy](../memory-strategy.md) |
| [testing-live-s3-transport.md](testing-live-s3-transport.md) | Live-S3 transport failure/retest record | 2026-10-11–12 | [overview](../overview.md) |
| [testing-memory-attribution.md](testing-memory-attribution.md) | Hidden-window memory attribution | 2026-10-08 | [memory-strategy](../memory-strategy.md) |
| [testing-native-listener.md](testing-native-listener.md) | Native listener experiment acceptance | 2026-10-09 | [memory-strategy](../memory-strategy.md) |
| [testing-ui-process.md](testing-ui-process.md) | Split-process lifecycle measurements | 2026-10-08–11 | [memory-strategy](../memory-strategy.md) |
| [testing-view-startup.md](testing-view-startup.md) | On-demand view startup timings | 2026-10-08–09 | [memory-strategy](../memory-strategy.md) |
| [testing-view-surfaces.md](testing-view-surfaces.md) | Retained-window backing-store experiment | 2026-10-08 | [memory-strategy](../memory-strategy.md) |
| [testing-memory.md](testing-memory.md) | Physical RSS/allocator experiment ledger; obsolete three-window workload | 2026-10-08–11 | [memory-strategy](../memory-strategy.md) |
| [testing-vm-memory-audit.md](testing-vm-memory-audit.md) | Dated VM dashboard/counter audit and replay | 2026-10-08 | [memory-strategy](../memory-strategy.md) |

## Visual design before putgd::Ui

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [design.md](design.md) | Blue-palette visual/form iterations | 2026-10-08–15 | [visual-language](../visual-language.md) |
| [ui-redesign-experiment.md](ui-redesign-experiment.md) | Issue #40 redesign prototype | 2026-10-15–16 | [visual-language](../visual-language.md) |

## Proton 2026-10-15 audits

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [proton-drive-release-gates.md](proton-drive-release-gates.md) | Source-specific integration and release-gate reconciliation | 2026-10-15 | [proton-drive](../proton-drive.md) |
| [proton-drive-verification.md](proton-drive-verification.md) | Integration investigation and verification ledger | Through 2026-10-15 | [proton-drive](../proton-drive.md) |

## Plans

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [2026-10-07-1926-feat-qt-linux-share-plan.md](plans/2026-10-07-1926-feat-qt-linux-share-plan.md) | Qt Screenshot Sharing for Linux - Plan | 2026-10-07 | [overview](../overview.md) |
| [2026-10-17-2230-feat-screen-recording-plan.md](plans/2026-10-17-2230-feat-screen-recording-plan.md) | Screen recording | 2026-10-17 | [recording](../recording.md) |
| [2026-10-11-p2-followup.md](plans/2026-10-11-p2-followup.md) | Cumulative review follow-up | 2026-10-11 | [architecture](../architecture.md) |
| [2026-10-17-1529-feat-ci-cd-pipeline-redesign-plan.md](plans/2026-10-17-1529-feat-ci-cd-pipeline-redesign-plan.md) | CI/CD pipeline redesign | 2026-10-17 | [ci](../ci.md) |
| [2026-10-12-1657-feat-stable-nightly-release-channels-plan.md](plans/2026-10-12-1657-feat-stable-nightly-release-channels-plan.md) | Stable and Nightly Release Channels - Plan | 2026-10-12 | [release](../release.md) |

## Reviews

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [2026-10-11-cumulative-review.json](reviews/2026-10-11-cumulative-review.json) | Cumulative review machine-readable receipt | 2026-10-11 | [memory-strategy](../memory-strategy.md) |
| [2026-10-11-cumulative-review.md](reviews/2026-10-11-cumulative-review.md) | Cumulative review findings | 2026-10-11 | [memory-strategy](../memory-strategy.md) |
| [2026-10-11-allocator-annex-review.json](reviews/2026-10-11-allocator-annex-review.json) | Cumulative review machine-readable receipt | 2026-10-11 | [memory-strategy](../memory-strategy.md) |
| [2026-10-11-cumulative-review.metadata.json](reviews/2026-10-11-cumulative-review.metadata.json) | Cumulative review machine-readable receipt | 2026-10-11 | [memory-strategy](../memory-strategy.md) |

