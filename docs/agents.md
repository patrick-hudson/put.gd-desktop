# Working as an agent

Read [AGENTS.md](../AGENTS.md), [build](build.md), [architecture](architecture.md),
[configuration](configuration.md), [CLI](cli.md), and [current evidence](verification.md).
Read [resident memory strategy](memory-strategy.md) before changing process boundaries, codec loading or idle retention. Use the source114 live-S3 checkpoint below for the later cross-desktop transport evidence; source63 remains the earlier local-memory checkpoint.

The implementation contract is issue #1 and the dated plan in `docs/plans/`.
Implement its dependencies in order and record blocked acceptance separately from passing tests.

## Source and verification map

| Concern | Implementation | Tests / instructions |
| --- | --- | --- |
| Immutable operations, artifacts, naming, archive routing | `src/core/`, `src/app/OperationService.*` | `tests/unit/`, collision/storage integration tests |
| Configuration and revisions | `src/core/SettingsService.*`, `schemas/config.schema.json`, `config/development.example.json` | `test_config_reload.cpp`, configuration docs |
| Persistent history and remote deletion | `src/core/HistoryStore.*`, `src/app/HistoryService.*` | history/storage/deletion integration tests |
| Provider contract | `src/core/StorageProvider.h`, `StorageReceipt.h` | `tests/fixtures/U2Fixtures.h`, storage contract tests |
| S3 / explicit credential resolution | `src/storage/s3/`, `src/secrets/` | isolated S3/keychain fixtures; [credentials](credentials.md), [S3](s3.md) |
| Settings/activity/history widgets and login startup | `src/ui/`, `src/app/LoginStartup.*` | `tests/gui/`, `test_login_startup.cpp`, [user guide](user-guide.md) |
| Disposable desktop VM lab / development package | `tools/lab/`, `packaging/linux/` | `tests/desktop/test_lab.py`, [VM lab](vm-lab.md), [preferred remote hosts](remote-builders.md) |
| UI process lifecycle and total memory | `src/app/UiProcessController.*`, `src/ui/UiApplicationController.*`, `UiSession.*`, `ViewState.*` | [protocol](ui-process-protocol.md), [state allowlists](ui-process-state.md), `test_ui_process`, `test_ui_split_services`, `test_gui_application` |
| Session JSON transport | `src/app/LocalServer.*`, `src/cli/main.cpp` | `test_cli.cpp`, [CLI](cli.md) |
| Optional stdio MCP | `tools/mcp/` | `tests/integration/test_mcp.py`, [MCP](mcp.md) |
| Edit & upload editor and agent editing | `src/editor/`, `src/ui/EditorWindow.*`, `src/ui/ImageEditor.*`, `src/core/EditorJob.*`, `src/core/SanitizedPng.*` | `editor_contract`, `gui_editor_window`, `editor_job_integration`, `edit_operations_integration`; [image editor](image-editor.md) |
| Core listener/native tray and ephemeral capture | `src/platform/NativeSession.h`, `src/ui/NativeTray.*`, `src/platform/x11/Capture*`, `src/capture/` | [boundary and reproducible gates](native-listener.md); Core clipboard, native snapshot and capture lifecycle tests |
| Resident/post-use memory and capture latency | `tests/performance/`, `src/app/OperationService.*` | [physical replay](testing-memory.md); synthetic lab results are separate from physical budgets |
| Integer widths / member layout | `tools/diagnostics/layout/` | [read-only probe and replay](../tools/diagnostics/layout/README.md); sizes alone do not prove RSS savings |
| Build, dependency pins, sanitizers | `CMakePresets.json`, `cmake/`, `tools/verify_dependencies.py` | [build](build.md), [thread checks](thread-sanitizer.md) |
| Commit/build identity and package versions | `cmake/BuildVersion.cmake`, `src/core/BuildInfo.h` | `version_contract`, `version_integration`, `gui_version`; [versioning](versioning.md) |

Use `cmake --preset dev`, `cmake --build --preset dev`, and `ctest --preset dev`.
After each commit, run the ordinary build before reporting that commit as the
running version. Retain `--version`, `build-info.json` and binary hashes with test
receipts. Archive/VM builds must carry the exact exported commit/tree/count as
documented in versioning.md; unknown provenance must remain labeled unknown.
Provision the pinned official AWS SDK following build.md; preserve dynamic distro Qt
linking and the Qt 6.4.2 API floor. Administrator package installation requires the user.
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

Prefer the [Fedora, Debian and Kubuntu physical servers](remote-builders.md) for builds,
VMs and native graphical acceptance directly on their existing GNOME/Plasma
desktops; use local VMs only when the required remote host/profile
is unavailable or unsuitable, and record why. Passwordless `sudo -n` setup is
authorized on those three servers. Preserve their existing projects and desktops;
keep the workstation SSH key local. Check KVM, capacity and ports on the actual
VM host before launch. A physical host's OS/desktop is not a substitute for a
different version in the declared acceptance matrix. Keep Cinnamon-first ordering.

Every graphical run must stay visible at the workstation's display wall and
produce saved screenshots or recordings, including failure evidence, tied to its
host/session and source/build identity. Remote guest displays need their own
loopback VNC tunnel/tile; a physical-host tile does not automatically show a VM.
Retain the scenario's independent paste/artifact and QMP witnesses as required.

Use the shared C++/Qt contracts without introducing Node.js into guest test
requirements. The GNOME extension ownership fixture uses the GNOME desktop's
existing GJS runtime; run it there and retain its separate receipt when other
hosts lack GJS. Its Shell dependencies are mocked, so native graphical cases
remain required. See [startup replay](testing-view-startup.md) for the fixed
on-demand workload, exact identities and quiet measurement policy.

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

The lab interface is `python3 tools/lab/lab.py`; use [testing](testing.md) and [VM lab](vm-lab.md) for exact package/provisioning/snapshot commands. A real desktop scenario remains unverified until its saved artifact, independent paste and QMP witness pass. U10's full release lifecycle is still pending.
For memory work, start with one current-version Cinnamon build, including its
functional and memory checks. Fix or reject a failed gate before building/testing
its GNOME or KDE counterparts. Acceptance still requires current-source native
functional evidence on Cinnamon/X11, GNOME/Wayland and KDE/Wayland. After Cinnamon passes, those independent profiles
may run in parallel. Wait for build/GUI activity to finish before
quiet memory samples. Use one frozen source revision/archive across distro-native
builds, and retain package, app, CLI, S3 module and helper hashes. The lab rejects
missing install receipts and running app bytes that differ from the installed
package, including an old process surviving an upgrade. Record every blocked or
failing profile; a physical Cinnamon memory win does not close its sibling gates.
Never present an older VM resident as evidence for the current candidate. Stop
superseded VMs once their packages/evidence are copied; keep needed build caches
powered off and protect immutable bases. See [VM memory and build identity](testing-vm-memory-audit.md).

Full U9 completion requires a working Cinnamon scenario, U6 lab documentation, and a
fresh agent successfully following committed instructions to build, run, collect redacted
diagnostics, and identify a simulated failure. Headless MCP parity alone cannot close it.

For each declared compositor collect actual capture/output bytes and paste into an
independent application, with app/CLI/MCP operation IDs, desktop/backend/Qt versions,
display layout/scales and expected/actual dimensions/hashes. VM manifests also record
ISO hash, acceleration, resources, portal/helper versions and build revision. Use isolated
synthetic artifacts, QMP witnesses for every display head, and disable host–guest clipboard
sharing. The plan requires single and mixed-display coverage, cancellations, focus changes,
clipboard image/text/files, settings edits, history after restart, guarded deletion, and
installed-package startup. Physical Cinnamon measurements supplement VM evidence.

No credentials, real clipboard/source content, authorization headers or source filenames
belong in default diagnostics or review packets. Keep intentional synthetic captures in
explicit acceptance artifacts. Read verification.md for what actually passed and gates
still pending; package/release readiness waits for those gates, notices and checksums.


## Memory checkpoints and historical experiments

The native listener, disposable UI/capture helpers and transient S3 worker are
integrated. Start with [memory strategy](memory-strategy.md) and the
[source114 live-S3 checkpoint](evidence/live-s3-memory/source114/README.md),
not the rejected source49 selector experiment. Source114 measured five-minute
RSS + swap peaks of 29.059 MiB on Cinnamon, 36.078 MiB on GNOME and 31.285 MiB
on KDE, with all 105 synthetic objects removed through production.
These are exact-source, bounded workloads; they do not certify a newer binary,
unlimited upload history, mixed displays or all release acceptance gates.

Earlier source49, source57 and source63 records remain historical evidence.
Their statements about unmerged code and open desktop targets describe those
checkpoints, not the current implementation. Retain failed attempts and timing
limits when comparing results. Do not restart an experiment clock or weaken a
guard to make a memory change pass.

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

Packaging artifacts and source114 replay are not current desktop proof. Keep
Cinnamon-first gates, immutable VM bases, visible consoles, bounded socket paths
and exact source/package identities. A release tag must match a clean checkout;
never relabel a prior binary with the current HEAD or source114 results. Stable
and candidate publication follows exact-asset acceptance. Nightlies use a separate
automated build/package class and do not establish current desktop certification.
Retain curated logs/parameters without credentials or private login/payload data.
Flatpak is deferred to issue #14.

`main` is stable source and `nightly` the development integration branch. Read the
release runbook before branch/tag operations. Main pushes automatically fast-forward
nightly; schedule/manual coordination also catches it up. Never force divergent
history into alignment: reconcile it through a reviewed merge. Inspect existing runs and immutable
source identities before dispatching; a scheduled controller can have a main head
different from its pinned nightly source. Build and package producer evidence must
match the actual source. Preserve the first preparation and original package
handoffs on retries. Missing evidence or byte conflicts remain failures; never
retag unchanged source, replace public assets or treat a draft as successful.
Use the shared CI implementation for both branches, preserving all valuable
checks when removing duplicate compilation. Future update-channel UI is separate work.

When only publication fails, inspect the failed job and existing draft before
retrying. Reuse the verified package artifacts with a failed-job rerun when their
source identities still match; do not rebuild successful packages merely to retry
an API operation. A created draft is not a published release. Keep dependency
installer logs outside the tagged checkout so clean-source verification remains
meaningful. Do not assume an open workflow-fix PR has landed.
