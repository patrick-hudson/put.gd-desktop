# Historical documentation

These pages preserve superseded designs, experiments, plans and source-specific
acceptance records. “Historical” means they describe their recorded version,
not today's implementation or a work list. Start with [overview](../design/overview.md)
for the current app and follow the replacement links below.

Follow [evidence retention](../testing/evidence-retention.md): age alone never justifies
deletion. Preserve acceptance summaries, exact source/build identities,
reproducible parameters, failures and resolutions, raw evidence references and
cleanup receipts. Compress cold bulk evidence only with verified checksums and
a restore index. Evidence records live in
[put.gd-evidence](https://github.com/patrick-hudson/put.gd-evidence); none are moved here. Historical Markdown gets an explicit banner; the three JSON
review receipts are preserved byte-for-byte and classified by this index so their
machine-readable content and provenance remain intact.

## Documentation layout

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [2026-11-docs-layout.md](2026-11-docs-layout.md) | Old-path to new-path table for the move into folders and for evidence leaving this repository; use it to resolve an old path | 2026-11-05 (#1222) | [docs/README.md](../README.md) |

## Windows

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [windows-vm-lab.md](windows-vm-lab.md) | Windows QEMU/base lifecycle | Before support was dropped on 2026-10-31 | [vm-lab](../lab/vm-lab.md) |

## Flatpak

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [flatpak.md](flatpak.md) | Unsupported sandbox/package experiment; moved from `docs/flatpak.md` in the [2026-11 layout change](2026-11-docs-layout.md) | October 2026 experiment; out of scope on 2026-10-31 | [release](../release/release.md) |

## Memory and process experiments (source49/57/63/114 era)

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [native-listener.md](native-listener.md) | Cinnamon Core/native-listener boundary | 2026-10-09–11 | [memory-strategy](../design/memory-strategy.md) |
| [gnome-core-resident.md](gnome-core-resident.md) | GNOME Core resident transition | 2026-10-10 | [architecture](../design/architecture.md) |
| [kde-resident-boundary.md](kde-resident-boundary.md) | KDE resident/selector boundary | 2026-10-10–11 | [architecture](../design/architecture.md) |
| [resident-ui-process.md](resident-ui-process.md) | Resident/UI split investigation | 2026-10-08–11 | [architecture](../design/architecture.md) |
| [testing-capture-parity.md](testing-capture-parity.md) | Capture startup regression investigation | 2026-10-09 | [overview](../design/overview.md) |
| [testing-gnome-memory.md](testing-gnome-memory.md) | GNOME experiment measurements | 2026-10-10 | [memory-strategy](../design/memory-strategy.md) |
| [testing-kde-memory.md](testing-kde-memory.md) | KDE experiment measurements | 2026-10-10–11 | [memory-strategy](../design/memory-strategy.md) |
| [testing-live-s3-memory.md](testing-live-s3-memory.md) | Live-S3 post-use memory campaign | 2026-10-11–12 | [memory-strategy](../design/memory-strategy.md) |
| [testing-live-s3-transport.md](testing-live-s3-transport.md) | Live-S3 transport failure/retest record | 2026-10-11–12 | [overview](../design/overview.md) |
| [testing-memory-attribution.md](testing-memory-attribution.md) | Hidden-window memory attribution | 2026-10-08 | [memory-strategy](../design/memory-strategy.md) |
| [testing-native-listener.md](testing-native-listener.md) | Native listener experiment acceptance | 2026-10-09 | [memory-strategy](../design/memory-strategy.md) |
| [testing-ui-process.md](testing-ui-process.md) | Split-process lifecycle measurements | 2026-10-08–11 | [memory-strategy](../design/memory-strategy.md) |
| [testing-view-startup.md](testing-view-startup.md) | On-demand view startup timings | 2026-10-08–09 | [memory-strategy](../design/memory-strategy.md) |
| [testing-view-surfaces.md](testing-view-surfaces.md) | Retained-window backing-store experiment | 2026-10-08 | [memory-strategy](../design/memory-strategy.md) |
| [testing-memory.md](testing-memory.md) | Physical RSS/allocator experiment ledger; obsolete three-window workload | 2026-10-08–11 | [memory-strategy](../design/memory-strategy.md) |
| [testing-vm-memory-audit.md](testing-vm-memory-audit.md) | Dated VM dashboard/counter audit and replay | 2026-10-08 | [memory-strategy](../design/memory-strategy.md) |

## Visual design before putgd::Ui

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [design.md](design.md) | Blue-palette visual/form iterations | 2026-10-08–15 | [visual-language](../design/visual-language.md) |
| [ui-redesign-experiment.md](ui-redesign-experiment.md) | Issue #40 redesign prototype | 2026-10-15–16 | [visual-language](../design/visual-language.md) |

## Proton 2026-10-15 audits

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [proton-drive-release-gates.md](proton-drive-release-gates.md) | Source-specific integration and release-gate reconciliation | 2026-10-15 | [proton-drive](../user/proton-drive.md) |
| [proton-drive-verification.md](proton-drive-verification.md) | Integration investigation and verification ledger | Through 2026-10-15 | [proton-drive](../user/proton-drive.md) |

## Plans

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [2026-10-07-1926-feat-qt-linux-share-plan.md](plans/2026-10-07-1926-feat-qt-linux-share-plan.md) | Qt Screenshot Sharing for Linux - Plan | 2026-10-07 | [overview](../design/overview.md) |
| [2026-10-17-2230-feat-screen-recording-plan.md](plans/2026-10-17-2230-feat-screen-recording-plan.md) | Screen recording | 2026-10-17 | [recording](../design/recording.md) |
| [2026-10-11-p2-followup.md](plans/2026-10-11-p2-followup.md) | Cumulative review follow-up | 2026-10-11 | [architecture](../design/architecture.md) |
| [2026-10-17-1529-feat-ci-cd-pipeline-redesign-plan.md](plans/2026-10-17-1529-feat-ci-cd-pipeline-redesign-plan.md) | CI/CD pipeline redesign | 2026-10-17 | [ci](../release/ci.md) |
| [2026-10-12-1657-feat-stable-nightly-release-channels-plan.md](plans/2026-10-12-1657-feat-stable-nightly-release-channels-plan.md) | Stable and Nightly Release Channels - Plan | 2026-10-12 | [release](../release/release.md) |

## Reviews

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [2026-10-11-cumulative-review.json](reviews/2026-10-11-cumulative-review.json) | Cumulative review machine-readable receipt | 2026-10-11 | [memory-strategy](../design/memory-strategy.md) |
| [2026-10-11-cumulative-review.md](reviews/2026-10-11-cumulative-review.md) | Cumulative review findings | 2026-10-11 | [memory-strategy](../design/memory-strategy.md) |
| [2026-10-11-allocator-annex-review.json](reviews/2026-10-11-allocator-annex-review.json) | Cumulative review machine-readable receipt | 2026-10-11 | [memory-strategy](../design/memory-strategy.md) |
| [2026-10-11-cumulative-review.metadata.json](reviews/2026-10-11-cumulative-review.metadata.json) | Cumulative review machine-readable receipt | 2026-10-11 | [memory-strategy](../design/memory-strategy.md) |

## Verification run records

Dated run records moved from [implementation evidence](../testing/verification.md) on 2026-11-05 (#1222), one file per section, in the order of the old page. Two sections had no date in their heading and take the date of their content: the implementation checkpoints and the native desktop checks.

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [2026-11-05-first-stable-acceptance-v26-1105-3906.md](verification/2026-11-05-first-stable-acceptance-v26-1105-3906.md) | First stable acceptance, `v26.1105.3906` | 2026-11-05 | [verification](../testing/verification.md#current-state) |
| [2026-11-02-history-gallery-blanking-scrolling-and-gap-crash-1151.md](verification/2026-11-02-history-gallery-blanking-scrolling-and-gap-crash-1151.md) | History gallery blanking, scrolling and gap crash (#1151) | 2026-11-02 | [verification](../testing/verification.md#current-state) |
| [2026-11-01-published-nightly-artifact-source-cve-audit-rerun.md](verification/2026-11-01-published-nightly-artifact-source-cve-audit-rerun.md) | Published nightly artifact/source/CVE audit rerun | 2026-11-01 | [verification](../testing/verification.md#current-state) |
| [2026-11-01-link-copied-on-wayland-desktops-with-no-native-adapter-1105.md](verification/2026-11-01-link-copied-on-wayland-desktops-with-no-native-adapter-1105.md) | Link copied on Wayland desktops with no native adapter (#1105) | 2026-11-01 | [verification](../testing/verification.md#current-state) |
| [2026-10-31-published-nightly-physical-native-gates.md](verification/2026-10-31-published-nightly-physical-native-gates.md) | Published nightly physical native gates | 2026-10-31 | [verification](../testing/verification.md#current-state) |
| [2026-10-31-published-nightly-artifact-source-cve-audit.md](verification/2026-10-31-published-nightly-artifact-source-cve-audit.md) | Published nightly artifact/source/CVE audit | 2026-10-31 | [verification](../testing/verification.md#current-state) |
| [2026-10-30-ui-catalog-reference-images-708.md](verification/2026-10-30-ui-catalog-reference-images-708.md) | UI catalog reference images (#708) | 2026-10-30 | [verification](../testing/verification.md#current-state) |
| [2026-10-25-integrated-linux-development-campaign.md](verification/2026-10-25-integrated-linux-development-campaign.md) | Integrated Linux development campaign | 2026-10-25 | [verification](../testing/verification.md#current-state) |
| [2026-10-25-history-open-memory-558.md](verification/2026-10-25-history-open-memory-558.md) | History-open memory (#558) | 2026-10-25 | [verification](../testing/verification.md#current-state) |
| [2026-10-24-login-startup-uninstall-contract-382.md](verification/2026-10-24-login-startup-uninstall-contract-382.md) | Login-startup uninstall contract (#382) | 2026-10-24 | [verification](../testing/verification.md#current-state) |
| [2026-10-22-first-stable-gate-plan.md](verification/2026-10-22-first-stable-gate-plan.md) | First stable gate plan | 2026-10-22 | [verification](../testing/verification.md#current-state) |
| [2026-10-23-native-package-full-acceptance-375-377-382.md](verification/2026-10-23-native-package-full-acceptance-375-377-382.md) | Native package full acceptance (#375, #377, #382) | 2026-10-23 | [verification](../testing/verification.md#current-state) |
| [2026-10-24-native-acceptance-driver-hardening-495.md](verification/2026-10-24-native-acceptance-driver-hardening-495.md) | Native acceptance driver hardening (#495) | 2026-10-24 | [verification](../testing/verification.md#current-state) |
| [2026-10-22-deb-rpm-exact-asset-audit-rehearsal-383.md](verification/2026-10-22-deb-rpm-exact-asset-audit-rehearsal-383.md) | DEB/RPM exact-asset audit rehearsal (#383) | 2026-10-22 | [verification](../testing/verification.md#current-state) |
| [2026-10-22-appimage-exact-asset-audit-rehearsal-383.md](verification/2026-10-22-appimage-exact-asset-audit-rehearsal-383.md) | AppImage exact-asset audit rehearsal (#383) | 2026-10-22 | [verification](../testing/verification.md#current-state) |
| [2026-10-23-appimage-window-icon-on-plasma-523.md](verification/2026-10-23-appimage-window-icon-on-plasma-523.md) | AppImage window icon on Plasma (#523) | 2026-10-23 | [verification](../testing/verification.md#current-state) |
| [2026-10-22-package-install-retries-and-snapshot-368.md](verification/2026-10-22-package-install-retries-and-snapshot-368.md) | Package install retries and snapshot (#368) | 2026-10-22 | [verification](../testing/verification.md#current-state) |
| [2026-10-23-stalled-package-installs-467.md](verification/2026-10-23-stalled-package-installs-467.md) | Stalled package installs (#467) | 2026-10-23 | [verification](../testing/verification.md#current-state) |
| [2026-10-22-weekly-stress-lane-on-k72-360.md](verification/2026-10-22-weekly-stress-lane-on-k72-360.md) | Weekly stress lane on k72 (#360) | 2026-10-22 | [verification](../testing/verification.md#current-state) |
| [2026-10-22-flaky-test-rethink-351.md](verification/2026-10-22-flaky-test-rethink-351.md) | Flaky-test rethink (#351) | 2026-10-22 | [verification](../testing/verification.md#current-state) |
| [2026-10-24-recording-session-contract-410.md](verification/2026-10-24-recording-session-contract-410.md) | Recording session contract (#410) | 2026-10-24 | [verification](../testing/verification.md#current-state) |
| [2026-10-20-recording-closeout-87-166-165-164-89.md](verification/2026-10-20-recording-closeout-87-166-165-164-89.md) | Recording closeout (#87, #166, #165, #164, #89) | 2026-10-20 | [verification](../testing/verification.md#current-state) |
| [2026-10-19-screen-recording-acceptance-sweep-87.md](verification/2026-10-19-screen-recording-acceptance-sweep-87.md) | Screen recording acceptance sweep (#87) | 2026-10-19 | [verification](../testing/verification.md#current-state) |
| [2026-10-19-wayland-recording-a-v-sync.md](verification/2026-10-19-wayland-recording-a-v-sync.md) | Wayland recording A/V sync | 2026-10-19 | [verification](../testing/verification.md#current-state) |
| [2026-10-19-x11-editor-window-refusal.md](verification/2026-10-19-x11-editor-window-refusal.md) | X11 editor window refusal | 2026-10-19 | [verification](../testing/verification.md#current-state) |
| [2026-10-19-gnome-editor-window-refusal.md](verification/2026-10-19-gnome-editor-window-refusal.md) | GNOME editor window refusal | 2026-10-19 | [verification](../testing/verification.md#current-state) |
| [2026-10-19-appimage-wayland-window-icons.md](verification/2026-10-19-appimage-wayland-window-icons.md) | AppImage Wayland window icons | 2026-10-19 | [verification](../testing/verification.md#current-state) |
| [2026-10-19-appimage-screen-recording-on-wayland.md](verification/2026-10-19-appimage-screen-recording-on-wayland.md) | AppImage screen recording on Wayland | 2026-10-19 | [verification](../testing/verification.md#current-state) |
| [2026-10-18-screen-recording-packages.md](verification/2026-10-18-screen-recording-packages.md) | Screen recording packages | 2026-10-18 | [verification](../testing/verification.md#current-state) |
| [2026-10-18-single-name-configuration-and-data-folders.md](verification/2026-10-18-single-name-configuration-and-data-folders.md) | Single-name configuration and data folders | 2026-10-18 | [verification](../testing/verification.md#current-state) |
| [2026-10-18-inherited-capture-output-in-settings.md](verification/2026-10-18-inherited-capture-output-in-settings.md) | Inherited capture output in Settings | 2026-10-18 | [verification](../testing/verification.md#current-state) |
| [2026-10-18-wayland-recording-regions-and-system-audio.md](verification/2026-10-18-wayland-recording-regions-and-system-audio.md) | Wayland recording regions and system audio | 2026-10-18 | [verification](../testing/verification.md#current-state) |
| [2026-10-18-screen-recording.md](verification/2026-10-18-screen-recording.md) | Screen recording | 2026-10-18 | [verification](../testing/verification.md#current-state) |
| [2026-10-17-resident-png-readers.md](verification/2026-10-17-resident-png-readers.md) | Resident PNG readers | 2026-10-17 | [verification](../testing/verification.md#current-state) |
| [2026-10-17-kde-window-frame-check.md](verification/2026-10-17-kde-window-frame-check.md) | KDE window frame check | 2026-10-17 | [verification](../testing/verification.md#current-state) |
| [2026-10-17-x11-clipboard-image-plugins.md](verification/2026-10-17-x11-clipboard-image-plugins.md) | X11 clipboard image plugins | 2026-10-17 | [verification](../testing/verification.md#current-state) |
| [2026-10-17-edit-and-upload.md](verification/2026-10-17-edit-and-upload.md) | Edit & upload | 2026-10-17 | [verification](../testing/verification.md#current-state) |
| [2026-10-16-local-s3-settings-controls.md](verification/2026-10-16-local-s3-settings-controls.md) | Local S3 settings controls | 2026-10-16 | [verification](../testing/verification.md#current-state) |
| [2026-10-16-clipboard-tray-wording.md](verification/2026-10-16-clipboard-tray-wording.md) | Clipboard tray wording | 2026-10-16 | [verification](../testing/verification.md#current-state) |
| [2026-10-16-unified-ui-acceptance.md](verification/2026-10-16-unified-ui-acceptance.md) | Unified UI acceptance | 2026-10-16 | [verification](../testing/verification.md#current-state) |
| [2026-10-13-current-acceptance-index.md](verification/2026-10-13-current-acceptance-index.md) | Current acceptance index | 2026-10-13 | [verification](../testing/verification.md#current-state) |
| [2026-10-15-proton-evidence-reconciliation.md](verification/2026-10-15-proton-evidence-reconciliation.md) | Proton evidence reconciliation | 2026-10-15 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-historical-implementation-checkpoints.md](verification/2026-10-08-historical-implementation-checkpoints.md) | Historical implementation checkpoints | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-current-native-desktop-and-visual-checks.md](verification/2026-10-08-current-native-desktop-and-visual-checks.md) | Current native desktop and visual checks | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-latest-native-and-physical-evidence.md](verification/2026-10-08-latest-native-and-physical-evidence.md) | Latest native and physical evidence | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-simple-setup-and-measured-performance-follow-up.md](verification/2026-10-08-simple-setup-and-measured-performance-follow-up.md) | Simple setup and measured performance follow-up | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-settings-filenames-and-image-ownership-refinement.md](verification/2026-10-08-settings-filenames-and-image-ownership-refinement.md) | Settings, filenames and image ownership refinement | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-memory-continuation.md](verification/2026-10-08-memory-continuation.md) | Memory continuation | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-live-selection-and-post-use-cleanup.md](verification/2026-10-08-live-selection-and-post-use-cleanup.md) | Live selection and post-use cleanup | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-combined-memory-and-live-selection-checkpoint.md](verification/2026-10-08-combined-memory-and-live-selection-checkpoint.md) | Combined memory and live-selection checkpoint | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-startup-cleanup-checkpoint.md](verification/2026-10-08-startup-cleanup-checkpoint.md) | Startup cleanup checkpoint | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-on-demand-ui-implementation-checkpoint.md](verification/2026-10-08-on-demand-ui-implementation-checkpoint.md) | On-demand UI implementation checkpoint | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-current-source-ui-split-native-wave.md](verification/2026-10-08-current-source-ui-split-native-wave.md) | Current-source UI split native wave | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-08-gnome-test-runtime-correction.md](verification/2026-10-08-gnome-test-runtime-correction.md) | GNOME test runtime correction | 2026-10-08 | [verification](../testing/verification.md#current-state) |
| [2026-10-10-live-s3-and-cinnamon-resident-experiment.md](verification/2026-10-10-live-s3-and-cinnamon-resident-experiment.md) | Live S3 and Cinnamon resident experiment | 2026-10-10 | [verification](../testing/verification.md#current-state) |
| [2026-10-10-kde-resident-memory-baseline.md](verification/2026-10-10-kde-resident-memory-baseline.md) | KDE resident memory baseline | 2026-10-10 | [verification](../testing/verification.md#current-state) |
| [2026-10-12-historical-live-s3-regression.md](verification/2026-10-12-historical-live-s3-regression.md) | Historical live-S3 regression | 2026-10-12 | [verification](../testing/verification.md#current-state) |
| [2026-10-14-closed-dropdown-wheel-input-26.md](verification/2026-10-14-closed-dropdown-wheel-input-26.md) | Closed dropdown wheel input — issue #26 | 2026-10-14 | [verification](../testing/verification.md#current-state) |
| [2026-10-14-column-drag-direction-27.md](verification/2026-10-14-column-drag-direction-27.md) | Column drag direction — issue #27 | 2026-10-14 | [verification](../testing/verification.md#current-state) |
| [2026-10-14-storage-driven-tray-feedback-33.md](verification/2026-10-14-storage-driven-tray-feedback-33.md) | Storage-driven tray feedback — issue #33 | 2026-10-14 | [verification](../testing/verification.md#current-state) |
| [2026-10-16-cinnamon-login-startup-24.md](verification/2026-10-16-cinnamon-login-startup-24.md) | Cinnamon login startup — issue #24 | 2026-10-16 | [verification](../testing/verification.md#current-state) |
| [2026-10-16-destination-controls-regression.md](verification/2026-10-16-destination-controls-regression.md) | Destination controls regression | 2026-10-16 | [verification](../testing/verification.md#current-state) |

## Acceptance matrix run records

Dated results moved from the [acceptance matrix](../testing/acceptance-matrix.md) on 2026-11-05 (#1222).

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [acceptance-matrix-records.md](acceptance-matrix-records.md) | The removal of the earlier Fedora 45 Aurora golden and what used it | 2026-11-02 | [acceptance matrix](../testing/acceptance-matrix.md) |

## Remote builder run records

Dated inventories moved from [remote builders](../lab/remote-builders.md) on 2026-11-05 (#1222).

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [remote-builders-records.md](remote-builders-records.md) | Access inventory of the Fedora and Debian hosts; Kubuntu host preflight | 2026-10-16 and 2026-10-17 | [remote builders](../lab/remote-builders.md) |

## Recording run records

Dated observations moved from [capture and export engine](../design/recording/capture-and-export.md) on 2026-11-09 (#1230).

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [recording-encoder-records.md](recording-encoder-records.md) | Skylake GPU hangs behind the encoder order, workstation encoder speeds, AMD hardware encoding check | 2026-10-17 to 2026-11-04 | [capture and export engine](../design/recording/capture-and-export.md) |

## CI run records

Dated measurements and demonstrations moved from [CI coverage](../release/ci.md) on 2026-11-05 (#1222). Most section headings in that file are new labels for what moved; the moved text is unchanged.

| File | What it was | When it was current | What replaced it |
| --- | --- | --- | --- |
| [ci-records.md](ci-records.md) | Hosted run timings, rollout and previous-pipeline measurements, stress lane results, retry and install demonstrations | 2026-10-10 to 2026-10-26 | [CI coverage](../release/ci.md) |
