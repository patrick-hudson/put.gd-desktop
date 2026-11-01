# Working as an agent

Read [AGENTS.md](../AGENTS.md), [overview](overview.md), [build](build.md),
[architecture](architecture.md), [configuration](configuration.md), [CLI](cli.md),
and [current evidence](verification.md).
Read [resident memory strategy](memory-strategy.md) before changing process boundaries,
codec loading or idle retention. The overview and architecture describe the current
design. Implement changes in dependency order and record blocked acceptance separately
from passing tests.

## Source and verification map

| Concern | Implementation | Tests / instructions |
| --- | --- | --- |
| Immutable operations, artifacts, naming, archive routing | `src/core/`, `src/app/OperationService.*` | `tests/unit/`, `tests/integration/` |
| Configuration and revisions | `src/core/SettingsService.*`, `schemas/config.schema.json`, `config/development.example.json` | `tests/integration/test_config_reload.cpp`, [configuration](configuration.md) |
| Persistent history and remote deletion | `src/core/HistoryStore.*`, `src/app/HistoryService.*` | `tests/integration/`, [CLI](cli.md) |
| Provider contract | `src/core/StorageProvider.h`, `src/core/StorageReceipt.h` | `tests/integration/test_storage_contract.cpp`, [providers](storage-providers.md) |
| S3 / explicit credential resolution | `src/storage/s3/`, `src/secrets/` | `tests/integration/`, [credentials](credentials.md), [S3](s3.md) |
| Backblaze B2 native API | `src/storage/b2/` | `tests/integration/test_b2.cpp`, [B2](b2.md) |
| Proton Drive helper adapter | `src/storage/proton/` | `tests/integration/test_proton.cpp`, `tests/integration/test_proton_transport.cpp`, [Proton Drive](proton-drive.md) |
| Settings/activity/history widgets and login startup | `src/ui/`, `src/app/LoginStartup.*` | `tests/gui/`, `tests/unit/test_login_startup.cpp`, [user guide](user-guide.md) |
| Disposable desktop VM lab / development package | `tools/lab/`, `packaging/linux/` | `tests/desktop/test_lab.py`, [VM lab](vm-lab.md), [remote hosts](remote-builders.md) |
| UI process lifecycle and total memory | `src/app/UiProcessController.*`, `src/ui/UiApplicationController.*`, `src/ui/UiSession.*`, `src/ui/ViewState.*` | `tests/integration/test_ui_process.cpp`, `tests/integration/test_ui_split_services.cpp`, [protocol](ui-process-protocol.md), [state](ui-process-state.md) |
| Session JSON transport | `src/app/LocalServer.*`, `src/cli/main.cpp` | `tests/integration/test_cli.cpp`, [CLI](cli.md) |
| Optional stdio MCP | `tools/mcp/` | `tests/integration/test_mcp.py`, [MCP](mcp.md) |
| Screen recording, helper supervisor and private review | `src/core/RecordingModel.*`, `src/app/RecordingService.*`, `src/app/RecordingBackend.h`, `src/app/RecordingHelperBackend.*`, `src/record/` | `tests/unit/test_recording_model.cpp`, `tests/integration/test_recording_service.cpp`, `tests/integration/test_recording_helper.cpp`, `tests/integration/test_recording_pipeline.cpp`, [recording](recording.md) |
| Edit & upload editor and agent editing | `src/editor/`, `src/ui/EditorWindow.*`, `src/ui/ImageEditor.*`, `src/core/EditorJob.*`, `src/core/SanitizedPng.*` | `tests/unit/test_editor.cpp`, `tests/gui/test_editor_window.cpp`, `tests/integration/test_editor_job.cpp`, [image editor](image-editor.md) |
| File-manager “Upload with put.gd” | `src/app/FileManagerIntegration.*`, `src/app/file-manager-script.sh`, `src/app/file-manager-launcher.sh`, `src/filemanager/` | `tests/unit/test_file_manager_integration.cpp`, `tests/unit/test_file_manager_launcher.py`, [file-manager upload](file-manager-upload.md) |
| Core listener/native tray and ephemeral capture | `src/platform/NativeSession.h`, `src/ui/NativeTray.*`, `src/platform/x11/`, `src/capture/` | `tests/integration/`, [architecture](architecture.md), [desktop gates](acceptance-matrix.md) |
| Resident/post-use memory and capture latency | `tests/performance/`, `src/app/OperationService.*` | [memory strategy](memory-strategy.md); synthetic lab results are separate from physical budgets |
| Surface driver and desktop outcomes | `tools/lab/surface_driver/` | `tests/desktop/test_surface*.py`, [prerequisites](surface-driver-prerequisites.md) |
| Automatic nightly desktop checks | `tools/lab/nightly_desktops.py` | `tests/desktop/test_surface_nightly_desktops.py`, [lab sharing](remote-builders.md#sharing-the-lab-between-concurrent-sessions) |
| UI catalog and reference images | `tests/gui/ui_catalog.cpp`, `tests/gui/catalog-references/` | [UI catalog](ui-catalog.md), [visual language](visual-language.md) |
| Integer widths / member layout | `tools/diagnostics/layout/` | [read-only probe and replay](../tools/diagnostics/layout/README.md); sizes alone do not prove RSS savings |
| Build, dependency pins, sanitizers | `CMakePresets.json`, `cmake/`, `tools/verify_dependencies.py` | [build](build.md), [thread checks](thread-sanitizer.md) |
| Commit/build identity and package versions | `cmake/BuildVersion.cmake`, `src/core/BuildInfo.h` | `tests/cmake/version_contract.cmake`, `tests/integration/test_version.cpp`, `tests/gui/test_version.cpp`, [versioning](versioning.md) |

Use `cmake --preset dev`, `cmake --build --preset dev`, and `ctest --preset dev`.
After each commit, run the ordinary build before reporting that commit as the
running version. Retain `--version`, `build-info.json` and binary hashes with test
receipts. Archive/VM builds must carry the exact exported commit/tree/count as
documented in versioning.md; unknown provenance must remain labeled unknown.
Provision the pinned official AWS SDK following build.md; preserve dynamic distro Qt
linking and the Qt 6.4.2 API floor. Passwordless `sudo -n` setup is authorized
on the five builders; ask the user when administrator authentication is required.
Run the MCP contract suite separately as documented in mcp.md. Never interpret a fixture,
offscreen Qt test, or successful capability query as proof of a real desktop workflow.

## Acting and debugging

Discover the running session's capabilities and accepted configuration before acting.
An action has the configured local-save or publication effect; uploads may create public
links and clipboard copies. Use synthetic content and an isolated destination for tests.
Keep operation IDs, inspect eventual per-item outcomes, and reuse retry/copy services.
Unknown publication must be reconciled through the saved identity; never invent a link or
start another upload to conceal uncertainty. A failed local save can be retried separately.

History reads local metadata and requires no cloud listing. It can expose stored display
names and direct URLs to an explicitly requesting client; exclude these from default
diagnostic exports. Pagination and filters are documented in cli.md. Remote deletion
uses the history entry's original receipt and destination. Prepare selected IDs, review
actual effects, then confirm that exact ordered selection and returned acknowledgement.
Remote deletion preserves history/local copies; cache/history clearing never deletes remote
objects. Inspect each deletion outcome, including unknown and retained-version states.
See s3.md for version safeguards, residual identical-content races, and cache limits.

Discover the exact active config path/revision through inspection. Apply expected-revision
patches when editing alongside a GUI or another agent. A conflict requires rereading and
rebasing the intended edit. Invalid external JSON preserves accepted settings: correct the
file, then reload. Save external documents atomically in the same directory and cooperate
with `config.json.lock`; an editor ignoring the lock can race app writes. Dirty GUI drafts
survive external acceptance and show conflicts; discard/reload or reconcile the draft before saving.
Validation/reload must cause no capture, provider network request, or secret-store prompt.
See configuration.md for complete fields, routing, snapshots, and development import.

To add a provider, implement and statically register the provider-neutral interface;
keep SDK types inside its adapter. Validate non-secret settings without IO, describe
capabilities truthfully, persist opaque references and original destination receipts,
and await durable deletion checkpoints before mutations. Add synthetic contract tests
for conditional naming, ambiguous upload reconciliation, serialization identity, history
round trips, replacement/missing/denied deletes, and lost deletion replies.
[storage-providers.md](storage-providers.md) specifies callbacks and failure semantics.
Do not teach shared code to decode URLs or provider object keys.

## Desktop and release gates

Use the [Linux acceptance matrix](acceptance-matrix.md): Mint, Ubuntu, Kubuntu,
Fedora and Debian; Cinnamon, GNOME and KDE; native X11 and Wayland coverage.
Choose physical hosts and VMs per gate, prefer builders, provision missing
profiles, and keep physical-hardware release gates on physical machines.

Prefer the [Fedora, Debian, Kubuntu, Mint and Aurora physical servers](remote-builders.md)
for builds, VMs and native graphical acceptance on their GNOME/Plasma/Cinnamon
desktops; use local VMs only when the required remote host/profile
is unavailable or unsuitable, and record why. Passwordless `sudo -n` setup is
authorized on all five servers. Preserve their existing projects and desktops;
keep the workstation SSH key local. Check KVM, capacity and ports on the actual
VM host before launch. A physical host's OS/desktop is not a substitute for a
different version in the declared acceptance matrix. Take and heartbeat the desktop
lock before GUI work; release it after verified cleanup. Never wait for another
session's reply while holding a lock: notify the affected session and coordinator
of the exact PIDs, intended action and a deadline at least 10 minutes ahead;
silence means go after that deadline. Before any logout or display-server restart,
check for Patrick's own apps. If any are present, ask Patrick first; his apps are
never leftovers, and silence never means go.
The automatic nightly desktop check runs from a d69 timer across the five builders
and reports to pinned issue #1003; release lab leases before 07:15 UTC. See
[Sharing the lab between concurrent sessions](remote-builders.md#sharing-the-lab-between-concurrent-sessions).

Every graphical run must stay visible at the workstation's display wall and
produce saved screenshots or recordings, including failure evidence, tied to its
host/session and source/build identity. Remote guest displays need their own
loopback VNC tunnel/tile; a physical-host tile does not automatically show a VM.
Retain the scenario's independent paste/artifact and QMP witnesses as required.

Use the shared C++/Qt contracts without introducing Node.js into guest test
requirements. The GNOME extension ownership fixture uses the GNOME desktop's
existing GJS runtime; run it there and retain its separate receipt when other
hosts lack GJS. Its Shell dependencies are mocked, so native graphical cases
remain required. See [memory strategy](memory-strategy.md) for the current process
boundaries and measurement policy.

Before guest builds or graphical campaigns, run the read-only [storage preflight](lab-storage.md) and retain its JSON receipt. Budget expected build growth on both guest and host filesystems. Keep current and one rollback build; archive metadata and check references before removing obsolete builds. Never retire a disk without checking the complete backing chain and live QEMU references.

Before launching an isolated test, count the complete resolved Unix socket path
in UTF-8 bytes, including any harness-added directories and filenames. Linux's
limit is 107 bytes excluding NUL. Keep private run roots short; descriptive
evidence directory names belong in the later export. Preserve a failed attempt
before repeating it with a corrected path and the unchanged measurement protocol.
Run `python3 tools/diagnostics/check_socket_paths.py --ui-output RUN_1 RUN_2`
with the complete per-run output paths; it exits 2 if any socket would be too long.
For other harnesses, use `--socket` with their complete socket paths. The check
does not create directories or replace the app's ownership/permission checks.

Restore locked lab sessions yourself through their visible masked console fields
using the existing protected bootstrap login input. Verify its owner, private
permissions and regular-file identity; keep the value only in memory and out of
command lines, screenshots and all logs. Inspect the unlocked desktop before
collecting graphical acceptance. Ask the user for help only after checking that
the existing protected input is missing or rejected. Cloud `credentials.txt` is
never a source for desktop logins.

The lab interface is `python3 tools/lab/lab.py`; use [testing](testing.md) and [VM lab](vm-lab.md) for exact package/provisioning/snapshot commands. A real desktop scenario remains unverified until its saved artifact, independent paste and QMP witness pass.
For memory work, build and test the current-version Cinnamon, GNOME and KDE
profiles in parallel, each with its functional and memory checks. Acceptance
requires current-source native functional evidence on Cinnamon/X11, GNOME/Wayland
and KDE/Wayland. Wait for build/GUI activity to finish before
quiet memory samples. Use one frozen source revision/archive across distro-native
builds, and retain package, app, CLI, S3 module and helper hashes. The lab rejects
missing install receipts and running app bytes that differ from the installed
package, including an old process surviving an upgrade. Record every blocked or
failing profile; a physical Cinnamon memory win does not close its sibling gates.
Never present an older VM resident as evidence for the current build. Stop
superseded VMs once their packages/evidence are copied; keep needed build caches
powered off and protect immutable goldens. See [VM lab](vm-lab.md) for lifecycle and identity checks.

For each declared compositor collect actual capture/output bytes and paste into an
independent application, with app/CLI/MCP operation IDs, desktop/backend/Qt versions,
display layout/scales and expected/actual dimensions/hashes. VM manifests also record
ISO hash, acceleration, resources, portal/helper versions and build revision. Use isolated
synthetic artifacts, QMP witnesses for every display head, and disable host–guest clipboard
sharing. Desktop verification covers single and mixed-display setups, cancellations, focus changes,
clipboard image/text/files, settings edits, history after restart, guarded deletion, and
installed-package startup. Physical Cinnamon measurements supplement VM evidence.

No credentials, real clipboard/source content, authorization headers or source filenames
belong in default diagnostics or review packets. Keep intentional synthetic captures in
explicit acceptance artifacts. Read verification.md for what actually passed and gates
still pending; package/release readiness waits for those gates, notices and checksums.

## Memory strategy and historical experiments

The current [memory strategy](memory-strategy.md) describes the Qt Core resident,
on-demand UI/capture helpers, lazy S3 worker and 40 MiB idle target.
[Historical designs and experiment records](history/README.md) preserve earlier
boundaries and measured workloads; their results do not certify a newer binary.
Retain failed attempts and timing limits; never restart an experiment clock or
weaken a guard to make a memory change pass.

## Evidence retention and housekeeping

Follow [evidence retention](evidence-retention.md). Keep source identities,
parameters, acceptance summaries, original failures and cleanup receipts readable.
Cold bulk logs and transfer archives may be compressed only after verified
round-trip checks, with an index explaining how to restore their original paths.
Do not remove test source because its old results are redundant. Archive local
work separately from sanitized, committed evidence; never publish private work
or infer that a compressed artifact is safe to include in a review or release.

## Release and CI work

Read [release.md](release.md), [versioning.md](versioning.md) and [ci.md](ci.md)
before changing version identity, package formats or workflow coverage. Build
with normal presets; DEB/RPM use their own distro Qt/SDK ABI, while AppImage
dynamically bundles its locked runtime. `--version-json` is a headless identity
command on current product entrypoints; inspect an older binary's capabilities
before assuming it supports the new option.

Packaging artifacts and historical evidence replay are not current desktop proof. Keep
immutable Linux goldens, visible consoles, bounded socket paths
and exact source/package identities. A release tag must match a clean checkout;
never relabel a prior binary with the current HEAD or historical results. Stable
publication follows exact-asset acceptance. Nightlies use a separate
automated build/package class and do not establish current desktop certification.
Retain curated logs/parameters without credentials or private login/payload data.

`nightly` is the default branch and trunk; `main` is the release branch. Branch
from `nightly` and open pull requests against it; never push directly to either
branch. Hotfixes target `main` and merge back to `nightly` through a pull request.
The `CI` summary is the only required check; a cancelled or skipped-by-failure job
is not a pass. Documentation changes go in a separate docs-only pull request,
linked to their code pull request and merged after it. Docs-only CI runs
`contracts`; `docs/build.md` and `docs/proton-drive.md` are package inputs and
select every job.

Promotion uses `tools/releases/promote.py --open` to open a
`release: promote nightly YY.MDD.N` pull request from `nightly` into `main`.
Its merge commit tags stable `vYY.MDD.N` and stages its draft. Versions come from
the tagged commit's UTC date and commit count; nothing is bumped by hand.
There is no candidate channel; stable publication stays manual after exact-asset
acceptance. Never create, move or delete `v*` tags by hand or force-push either
branch. When asked for a release, follow [the release runbook](release-runbook.md)
step by step; read it before any branch or tag work.
Publication waits for every selected job. Nightly source CI may be reused only
for the exact commit under the rules in [CI coverage](ci.md). Inspect existing runs and drafts before rerunning,
and never retag unchanged source, replace public assets or treat a draft as published.

When only publication fails, inspect the failed job and existing draft before
retrying. Reuse the verified package artifacts with a failed-job rerun when their
source identities still match; do not rebuild successful packages merely to retry
an API operation. A created draft is not a published release. Keep dependency
installer logs outside the tagged checkout so clean-source verification remains
meaningful. Do not assume an open workflow-fix PR has landed.
