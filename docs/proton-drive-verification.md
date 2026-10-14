# Proton Drive integration verification — 2026-10-13

This is the production storage integration from issue #10, built on
`origin/main` at `cdb4e2f0a4f38d78e3780be59242115bb87b1f15`.
There is no separate demo application. The C++/Qt adapter uses the shared
operation, settings, history and private session API contracts. The bundled
helper calls the official SDK and reuses its official account foundation.

## Built and exercised

Host: Linux Mint 22.3, GCC 13.3, dynamic distro Qt 6.4.2. The existing official
AWS 1.11.907 installation was reused. The Proton-enabled build used:

```sh
python3 tools/proton/provision_runtime.py --source /tmp/putgd-proton-sdk --offline
cmake --preset dev -DPUTGD_BUNDLE_PROTON=ON
cmake --build --preset dev
ctest --preset dev
```

The initial committed helper and an identical repeated offline build produced SHA-256
`81d8600d7af30973b9b7019c0ec823f1bc814364348e9b18679d8fdd4d913735`.
It uses official SDK `js@0.22.2`, Crypto 2.0.0 and Bun 1.4.2, pinned upstream
commit `8ffae007b0fbb8bcef53514719d6f093bb32dcf9` and protocol 1.
[Helper evidence](../tools/proton/VERIFICATION.md) records 16 tests/79 assertions,
compiled protocol checks and the complete 1,048,337-byte stdout regression.
Full TypeScript checking still has four upstream Crypto/Bun stream declaration
errors; it is not reported as passing. No helper source diagnostics remain.

The focused preset run passed all four targets (`proton_contract`,
`proton_transport`, `gui_history_view`, `gui_settings`) in 29.09 seconds.
The full `ctest --preset dev` run passed all 59 targets in 420.22 seconds,
including existing S3 and local-only regressions.
Tests use synthetic providers, a synthetic stdio peer, offscreen widgets and
isolated Xvfb/native fixtures; these do not prove Proton service access or a
real supported compositor. New private test roots were preflighted with
`tools/diagnostics/check_socket_paths.py`; the longest resolved socket shape
was 58 UTF-8 bytes, below Linux's 107-byte limit.

CMake rejected a changed `runtime.ts` while its old executable/manifest remained.
The exact source was restored before the final configure/build. Installation
to a fresh temporary prefix preserved the private runtime, manifest and notices;
SHA-256 checks and its compiled `--version` passed. The initial install attempt
wrote its stage but could not write the worktree's `install_manifest.txt` inside
the sandbox; the failed stage/log were preserved and a new stage passed with
that worktree write authorized. Qt resolves dynamically to distro libraries.

A single no-account parser rejection measured 72,124 KiB maximum helper RSS
and 0.10 seconds wall time with `/usr/bin/time -v`. This is neither an
account/upload workload nor total-process memory acceptance.

## Review and fixes

Completed Compound Engineering review receipt: `status: complete`, run
`20261013-154708-d8d0f8ac`; local artifact
`/tmp/compound-engineering-1000/ce-code-review/20261013-154708-d8d0f8ac`.
Ten local lenses and an independent Anthropic review ran. All seven validated
findings were applied; no code finding was left unresolved. The completed
review was consumed without rerunning it just to apply fixes.

| Finding | Applied behavior and verification |
| --- | --- |
| 1 (P1) | First-folder setup validates the account root independently of a selected folder. Three SDK-boundary tests failed before this fix, then passed. File operations still require both. |
| 2 (P2) | A stopped sharing job with a failed journal transition/completion exposes a terminal unknown result with no URL. Explicit inspection commits that state after storage recovery. Running/final/automatic journal failures and the GUI recovery route passed. Mutations are not replayed. |
| 3 (P2) | Inspection finding private access after an uncertain create preserves the create intent, expiration and password reference. A later explicit retry shares without another upload. Original private, revoked and previously expired shared results stay private on retry. |
| 4 (P2) | Authoritative save/reload and session validation enforce provider-specific credential modes without IO. Proton session mode is rejected; keychain mode and S3 session mode passed. |
| 5 (P2) | Proven pre-dispatch/authentication failures report failed. Timeout, malformed completion and unconfirmed exit remain unknown. The actual C++ adapter process transport, account-lock contention and missing-runtime cases passed. |
| 6 (P2) | History retains accepted sharing IDs and resumes read-only polling after reopening. Shared/failed completion and journal recovery remain visible without another mutation/upload. Legacy state remains accepted. |
| 7 (P3) | Bundling verifies the current helper sources, dependency lock, pinned versions and notices. The controlled stale-source configure test failed as required. |

Simplification applied a common receipt identity predicate, removed duplicate
capability checks, indexed startup sharing recovery, and narrowed recent-item
SQL projection. A minor repeated TypeScript classification suggestion was
skipped because it did not justify another runtime change.

## Acceptance still pending

The initial automated verification did not exercise a live Proton account.
The local folder-name follow-up below records later authenticated setup evidence.
Upload/share acceptance must use the isolated destination and synthetic files,
verify downloaded bytes and recipient access, and exercise upload/share recovery,
passwords/expiry and revoked sessions.
Do not copy host authentication into VM images. Authenticated helper/total-process
memory and Cinnamon functional/memory acceptance remain pending; GNOME/KDE
follow only after Cinnamon passes. Source/protocol/Xvfb evidence is not release
acceptance. Complete redistribution review and Proton third-party/service
readiness also remain release gates.

Supported limits are documented in [Proton Drive](proton-drive.md): create-only
uploads; no revision-guarded remote deletion; normal Proton share-page URLs;
fresh identity checks around sharing, without an atomic SDK revision guard.
The default request timeout is 120 seconds (configurable up to 600); large or
slow transfers may become uncertain. Abandoned browser login can hold its account
lock for up to ten minutes; setup cancellation is not yet exposed in the GUI.

## Folder name follow-up

The local user created a folder and saved its ID, but the picker reloaded with
“Saved Proton folder” instead of the actual name. A read-only SDK account
inspection confirmed that the saved ID matched the created top-level test folder.
Authentication and the folder listing worked on the local machine; this does not
establish upload, recipient access, memory or compositor acceptance.

`createdProtonFolderNameSurvivesSaveRefreshAndReopen` failed before the fix because
Save replaced the generated name with the generic label. It now passes through
creation, Save, an explicit Refresh reporting a renamed Unicode folder, another
Save, a fresh window, retained-window restoration and configuration reload.
The optional `folderName` display hint is paired with the selected ID and
classified in retained state. Legacy settings without the hint still validate.
The helper accepts the bounded hint; folder IDs continue to govern storage.

The updated offline runtime SHA-256 is
`9b147e7849474fef4fc99a4e38bc4a6758f10187e08ac56e8fe2936a91e86d68`.
An identical repeated offline build matched that hash. Provisioning passed
17 helper tests / 88 assertions, seven compiled stdio cases,
and the complete 1,048,337-byte stdout regression. The full C++ build passed.
Nine targeted preset tests passed in 79.91 seconds: `gui_application`,
`ui_native_lifecycle_integration`, `cli_integration`, `config_reload_integration`,
`settings_contract`, `gui_settings`, `gui_history_view`, `proton_contract` and
`proton_transport`. Both configuration-schema tests passed using the existing
locked MCP Python environment. Socket preflights passed (75-byte maximum).
This follow-up did not repeat the full 59-target suite. Type checking retains
only the same four upstream Crypto/Bun stream declaration errors.

Three fix-scoped simplification reviews found no changes needed. The independent
fix review confirmed one P3 schema mismatch: an end anchor allowed a trailing
newline where the provider and helper rejected it. The schema regression failed
with that value before the correction. An explicit negative control-character
constraint now rejects it, and both schema tests pass. Completed fix-only review
receipt: `20261013-185504-8b690fba` under
`/tmp/compound-engineering-1000/ce-code-review/20261013-185504-8b690fba`.
It reviewed the earlier fix snapshot; the confirmed schema finding was then
applied and reverified. No finding remains unapplied. Native restart/name-backfill
results are retained privately under the local test run rather than committing
account folder names or IDs.


## Upload latency and completion notification follow-up

The local test configuration had `preferences.notifications:false`; the tray was
available. A revision-checked patch enabled that global preference without changing
other preferences. No notification code change was needed. The existing native
tray fixture covers one completion notification when enabled and suppression when
disabled; it is not evidence that a host desktop visibly displayed a toast.

Two original-runtime and two optimized-runtime runs used the production C++
adapter, account lock, authenticated helper transport and an isolated connected
Proton folder. Each created a unique synthetic 65,536-byte file and a new link,
then revoked only that link. Files remain private in the isolated test folder.
Probe evidence contains numeric timings and generic statuses; private identities,
credential references and recovery journals are excluded from this document.

| Stage | Original runs (seconds) | Optimized runs (seconds) |
| --- | --- | --- |
| Name preflight | 6.004, 4.886 | 2.950, 2.959 |
| Upload | 9.884, 6.308 | 6.666, 5.972 |
| Create share link | 12.991, 15.547 | 7.107, 7.034 |
| Ready after preflight, upload and sharing | 28.879, 26.741 | 16.723, 15.965 |
| Revoke probe link (after readiness) | 11.845, 16.062 | 6.049, 4.497 |

Mean readiness fell from 27.810 to 16.344 seconds (41.23%). This is a small
local pilot with service/network variation, not a large-file throughput claim or
a capture/encoding/archive/desktop timing. The intermediate key-cache-only run
was 22.609 seconds. A first candidate configuration omitted runtime bundling and
failed before dispatch with runtime unavailable; that failure was preserved and
corrected with explicit `PUTGD_BUNDLE_PROTON=ON` before live candidate timing.

The helper now retains at most 64 opaque SDK crypto-key entries within one
request. Entity metadata remains uncached, so ownership and revision reads still
reach the service. The cache has explicit invalidation, tagged enumeration and
bounded LRU eviction and never persists or crosses helper requests. Name
preflight uses the SDK's public `getAvailableName` encrypted hash query instead
of decrypting all folder children. SDK create collision handling remains final;
reconciliation and proof of missing still use their existing scoped checks.

The cache-retention regression failed with the original NullCache. The direct
availability regressions failed with the original folder-listing preflight. All
23 helper tests / 105 assertions now pass, along with seven compiled stdio cases
and the complete 1,048,337-byte stdout drain. The optimized runtime SHA-256 is
`146f1aa4c0e474dc76361385bd2125be1f2bf4857f1441742cccbc56bcb0e783`.
The full dev-preset app build passed. Full TypeScript checking still reports the
same four upstream Crypto/Bun stream declaration errors and no helper diagnostics.

Three fix-scoped simplification reviews found no required change. One suggestion
to remove the malformed-SDK-result guard was skipped to retain defensive boundary
validation. The independent fix review covered crypto-cache lifetime, fresh
metadata/signature checks, supported name availability and benchmark privacy.
Completed receipt: `20261013-193135-10909ddb`, with six local reviewers and an
independent Anthropic pass; no code finding remained. Review did not execute the
benchmarks. A warmed real-SDK cache followed by changed metadata/signatures and a
live occupied-name preflight remain focused coverage gaps; pinned SDK source and
existing signature/changed-identity regressions support the current boundary.

Sixteen dev-preset targets passed in 172.32 seconds: `native_tray_integration`,
`gui_tray`, `collision_policy_integration`, `upload_naming_contract`,
`history_contract`, `operations_contract`, `storage_contract_integration`,
`proton_contract`, `proton_transport`, `gui_settings`, `gui_history_view`,
`settings_contract`, `config_reload_integration`, `cli_integration`,
`gui_application` and `ui_native_lifecycle_integration`. Socket preflight passed
with a 75-byte maximum. The full 59-target suite was not repeated for this fix.
Authenticated helper/total-process memory, recipient downloads, password/expiry
recovery and current Cinnamon/GNOME/KDE release acceptance remain pending.
