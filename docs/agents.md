# Working as an agent

Read [AGENTS.md](../AGENTS.md), [build](build.md), [architecture](architecture.md),
[configuration](configuration.md), [CLI](cli.md), and [current evidence](verification.md).
Read [resident memory strategy](memory-strategy.md) before changing process boundaries, codec loading or idle retention. Its linked source63 checkpoint distinguishes accepted local memory from pending remote/release acceptance.

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
| Disposable desktop VM lab / development package | `tools/lab/`, `packaging/linux/` | `tests/desktop/test_lab.py`, [VM lab](vm-lab.md) |
| UI process lifecycle and total memory | `src/app/UiProcessController.*`, `src/ui/UiApplicationController.*`, `UiSession.*`, `ViewState.*` | [protocol](ui-process-protocol.md), [state allowlists](ui-process-state.md), `test_ui_process`, `test_ui_split_services`, `test_gui_application` |
| Session JSON transport | `src/app/LocalServer.*`, `src/cli/main.cpp` | `test_cli.cpp`, [CLI](cli.md) |
| Optional stdio MCP | `tools/mcp/` | `tests/integration/test_mcp.py`, [MCP](mcp.md) |
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


## Preserved native listener experiment

The isolated branch `codex/optimize-resident-native-boundary` at source49
`676f6e6` passed the physical Cinnamon 40 MiB cold/closed RSS target, with a native
Qt Core listener/tray and disposable screenshot helper. It is not integrated
because selector startup regresses; do not mistake the memory-only evaluator's
keep recommendation for a release decision. Current-source native cases and
post-use VM footprints were recorded for all three desktops. GNOME/KDE retain
the graphical fallback and exceed 40 MiB RSS. Follow the
[exact-source replay and limitations](testing-native-listener.md) and
[hashed evidence](evidence/2026-10-09-native-listener.json) before continuing.
The experiment Git objects are local until its branch is deliberately published;
documentation commits identify source49 without claiming their binaries were
measured. Never discard the measured candidate or its failure records to restart
its clock, and never lower the capture timing guard to accept the memory saving.

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
never relabel a prior binary with the current HEAD or source114 results. Public
release publication follows exact-asset acceptance, with curated logs/parameters
and no credentials or private login/payload data. Flatpak is deferred to issue #14.
