# Proton Drive integration verification

## Current disposition — 2026-10-15

Proton is a retained experimental integration intended for production shipment
and ongoing support. Upstream endorsement/general availability is not a release
gate under the user-approved decision. The [evidence and release-gate register](proton-drive-release-gates.md)
is the current reconciliation: it credits the merged local and native campaigns,
links the P1 recipient/service and native recovery receipts, and lists the concrete
authenticated active-memory, notices, shipping-pipeline and compatibility work
still open. The [service campaign](evidence/2026-10-15-proton-p1-recovery.md) and
[normal-use application follow-up](evidence/2026-10-15-proton-p1-app-recovery.md)
complete P1 within their recorded scope. The latter fixes interrupted copy status
and verifies Cinnamon crash, destination/account loss, timeout/partial-result,
explicit retry and native paste behavior. Its post-use memory does not establish
P2's active-helper budget. Final-main packaged multi-desktop acceptance and P2–P6
remain open.

The checkpoints below are historical. PR #30 supersedes their initial pending
native tray/setup statements for its tested source and scope; newer changes still
need final-candidate evidence. Preserve the original failures and limitations.

## Initial integration checkpoint — 2026-10-13

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
At the initial feature checkpoint, the default request timeout was 120 seconds
(configurable up to 600); large or
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

## Upload progress and request-deadline follow-up

A local large-file operation reached its accepted 120-second helper deadline;
inspection found an unknown upload outcome, no result link and no cancellation.
The old generic interruption text did not distinguish expiry from cancellation.
An expected-revision MCP configuration patch increased only the current Proton
profile's `requestTimeoutMs` to 600,000. Verification confirmed all other settings
and the prior operation's identity/settings were preserved. This applies to new
operations; the uncertain earlier upload was neither reconciled nor resent.

The production pipeline now exposes preparation, upload, sharing and copy phases
through Activity and operation inspection. Upload callbacks carry optional typed
byte counters. Active observations use a bounded overlay and an on-demand timer,
without per-tick journal writes; callbacks are closed at each attempt's completion.
Old/late callbacks cannot change a retry or overwrite durable terminal results.
The final observation is informational, not publication or ownership evidence.
A saved `resultReady` event drives notifications independently of progress.
The regression observed 17 duplicate toasts from a 50-event burst before this fix;
it now checks one final notification and a later explicitly requested copy result.

The Proton Advanced **Request deadline** control exposes the existing 1–600-second
range and default 120 seconds. Three decimal places preserve previously valid
millisecond values: tests retain 120,001 ms and 600,000 ms through Save/reopen.
New expiry errors identify the reason and budget while retaining unknown remote
outcome semantics. Browser authentication keeps its separate ten-minute deadline.

Offline helper provisioning passed 27 tests / 117 assertions, seven compiled
stdio cases and the full 1,048,337-byte stdout regression. Runtime SHA-256:
`0f8ef3ec922f7da98bcaae82d35a8b00e5c90775bab25a0d215ef9a8e65970fa`.
The reporter bounds retention to one in-flight stdout write and one replaceable
latest event, preserves retry rollback and drains before final completion.
Full TypeScript checking retains the same four official Crypto/Bun declaration
errors and no helper/test diagnostics. No configured lint command was found.

An authenticated production-adapter probe used one unique synthetic 16 MiB file
in the isolated Proton folder. Upload succeeded in 11.505 seconds and produced
four byte reports before completion. The final SDK count was 16,778,576 bytes for
16,777,216 payload bytes: the SDK includes encrypted transfer overhead and has no
matching total. Activity therefore shows real bytes, with percentage only when
a provider supplies a matching denominator. The probe successfully created and
revoked only its own new link; the synthetic file remains private in that folder.
This verifies the actual SDK callback/stdio boundary, not the complete graphical
clipboard workflow, recipient download or memory/compositor acceptance.

Focused candidate operation, transport, Settings, Activity and native-tray tests
passed. An initial CTest invocation combined `--preset dev` with `--test-dir` and
actually selected the existing preset build; that run was preserved and excluded
from candidate evidence. Corrected runs used plain `ctest --test-dir` with the
explicit candidate directory. A widget expectation used KiB while the existing
byte formatter correctly displayed 0.5 MiB; only the assertion was corrected.
The separate official-SDK MCP/production-CLI contract suite passed all three tests
in 1.502 seconds, with a preflighted 57-byte temporary socket path.

The full bundled `cmake --preset dev`, `cmake --build --preset dev`, and
`ctest --preset dev` run passed all 59 targets in 408.26 seconds, including S3,
local-only, journal recovery and updated progress/notification regressions.
Socket preflight covered the resolved harness shapes (82 UTF-8 bytes maximum).
The local test app was confirmed idle with Activity open and no Settings window,
then stopped gracefully before the copied runtime changed. Deployment identity
and the subsequent native restart are recorded separately.

Authenticated total-process memory, independent recipient/download verification,
password/expiry/revoked-session recovery and current Cinnamon/GNOME/KDE graphical
release acceptance remain pending. The local SDK progress result and offscreen/
Xvfb fixtures do not close those gates. Redistribution/service readiness remains
outside this fix's source verification.

Three fix-scoped simplification reviews completed. Two efficiency findings were
applied: identical observations no longer schedule UI/history refreshes, and
artifact matching uses a first-match ID map outside the observation mutex.
A no-op progress regression passed. A low-value private call-options reshaping
suggestion was skipped; no safety checks were removed. The eight affected
operation/storage/CLI/history/Settings/Activity/tray targets then passed in
109.14 seconds, with a 75-byte maximum preflighted socket shape. The helper was
unchanged, so its verified source fingerprint remains valid.

Completed current review receipt: `status: complete`, run
`20261013-204613-b88d0d60`, artifact
`/tmp/compound-engineering-1000/ce-code-review/20261013-204613-b88d0d60`.
Nine local lenses and an independent Anthropic Opus 5.5 pass completed; the
peer model/independence was receipt-verified, while serving effort was unverified.
All three validated findings were applied, without rerunning completed review:

| Finding | Applied behavior and regression evidence |
| --- | --- |
| #1 (P2) | An admitted retry is checkpointed as stored/queued before enqueue and acknowledgement. The occupied-worker GUI regression failed with the old partial state; it checks durable/inspection/Activity queued state and preserved publication/destination identity. |
| #2 (P2) | Controlled-clock tests prove numeric delivery while the reporter remains open and rescheduling after a blocked write, before finish. Disabling scheduling in an isolated source copy leaves the original three tests passing and makes both new cases fail; disabling rescheduling makes the new slow-write case fail. Restored helper tests pass 29 cases / 130 assertions. Production helper source/hash is unchanged. |
| #3 (P3) | Recovery and inactive cancellation normalize stopped operation/item phases while preserving unknown upload outcomes, counters and saved identity. The new recovery/cancellation assertions failed with uploading phases before the fix. |

Review's fractional-value test gap is closed by explicitly saving/reopening the
unchanged 120,001 ms value before testing 600,000 ms. A separate genuine-resident
reply-delay fixture established Activity starvation during sustained progress:
the 600 ms delayed response test failed while 50 ms provider reports continued.
Activity now renders completed snapshots before scheduling a pending newer read;
one-in-flight/one-pending bounds, lifetime guards and selection/expansion remain.
The isolated new case plus three related GUI cases passed in 2.472 seconds.
Fixture latency defaults to zero. An initial retry-fix build omitted an outer
lambda's journal-path capture; its failed diagnostics were retained, the capture
was corrected, and the build passed before verification continued.

Remaining focused coverage: direct no-account helper-timer tests for pre/post-
dispatch deadline branches and one final drained frame. C++ watchdog regressions
cover reason/budget/uncertainty, but do not prove those helper branches. This is
an unexecuted test path, not a source-established remaining defect. Downstream
History SQL/IPC work during progress has not been measured; notifications are
coalesced and unchanged observations are suppressed. Measure before choosing
separate invalidation channels or incremental tree updates. These limitations
were raised by the current testing/performance/reliability review. No validated
code finding remains deferred. Current memory/compositor/release gates above
remain open.

Post-review bundled build passed. The 14-target preset run passed 13 targets in
180.35 seconds; the new unchanged-timeout Settings assertion inspected retained
state before Save acknowledgement. That failed run was preserved. Its wait was
corrected without changing production code, then the full `gui_settings` target
passed in 12.07 seconds. The other 13 unchanged targets remain valid: native tray,
GUI application/lifecycle, S3 module, history, operations, storage, CLI, config
reload, Activity, history UI, Proton contract and Proton transport. The occupied-
worker queued retry and stopped-phase cases now pass. Socket preflight passed at
75 bytes for the broad run and 68 bytes for the Settings follow-up. Combined
with the earlier full 59-target run and unchanged authenticated SDK transport
proof, this closes the source/contract scope of the progress/deadline fix; it does
not close the pending native memory/compositor and provider recovery gates.


## Large-file deadline and retry recovery follow-up

The request deadline now accepts 1–86,400 seconds (24 hours), with the original
120-second default. Browser sign-in remains capped at ten minutes. Explicit
Retry may adopt a longer current matching-provider/profile/account/root/folder
budget for upload and reconciliation, saved as `operation.requestTimeoutMs`.
The accepted destination/settings/receipt remain unchanged. Lowering the profile
or reloading configuration does not shorten that persisted retry budget.
Name checks and sharing retain their accepted destination budgets.

A read-only check of the user-reported large-file candidate found no completed
matching file but an unavailable name. The pinned SDK hides unfinished drafts
from completed-file lists and recognizes its own drafts by client UID. The old
helper generated a different UID for each temporary request directory. Upload
and reconciliation now use an account-reference/publication-specific UID. An
explicit SDK unfinished-upload collision is preserved as `upload_draft_conflict`;
only completed-list `missing` proof permits the configured rename/skip handling.
Generic collisions, failed reads and unproven ownership remain unknown. Older
unknown drafts are not removed. Retry uses the retained staged artifact.

The helper's `status()` factory also overwrote supplied error details with an
empty object. It now preserves deadline/draft failure codes. Collision
reconciliation preserves its underlying error and reports terminal `unknown`
phase rather than leaving `reconciling` after work has stopped.

Validation:

- Full dev configure/build passed. All 59 CTest targets passed in 438.02 seconds,
  with private socket preflight maximum 82 bytes. After terminal phase/error
  fixes, nine related targets passed in 106.38 seconds.
- Review `20261013-220545-6abdd42d` completed with seven local lenses and an
  independent Anthropic `claude-opus-5-5` pass (actual model/independence verified;
  requested high effort, actual effort unverified). All three retained findings
  were applied: cancellation forwarding for both reconciliation paths, sustained
  output accounting, and actual service-level retry-budget assertions.
- Red regressions observed missing cancellation flags in both service paths and
  rejection of valid progress after the old cumulative quota. The final nine
  related targets passed in 108.16 seconds, socket maximum 75 bytes. They capture
  both upload/reconcile budgets at one hour, lower the profile and reload, then
  prove the dispatched and durable budget remains one hour. A real synthetic
  helper reconciliation with a 30-second override stops promptly on cancellation.
- Stdout uses a two-MiB burst token bucket with one-KiB/second refill and bounded
  frame/pending reads. A controlled 32,768-frame, 500-ms-cadence check exceeds two
  MiB lifetime output and succeeds; oversized frames and existing flood cases
  still fail. This is controlled clock evidence, not a multi-hour live upload.
- All 32 helper tests / 149 assertions passed. Eleven compiled no-account stdio
  cases accept one-hour/24-hour overrides and reject out-of-range values. The
  full 1,048,337-byte compiled stdout check passed. Helper SHA-256:
  `68228b887f68b06d4aca742b36ba127863b30bd87bd6e9761bcb841df637f4b3`.
- Three separate official-MCP/production-CLI integration tests passed. Full
  TypeScript checking still has four upstream Crypto/Bun declaration errors,
  with no helper/test diagnostics. No helper lint is configured.

The follow-up local review found no concrete issue in the applied fixes.
Remaining focused gaps are an explicit draft/skip fixture and a composed helper
plus watchdog reconciliation deadline check. Same-account serialization still
makes other requests report busy while a long request holds its lock.
Current compositor/total-process memory, recipient access, session recovery and
redistribution/service readiness acceptance remain pending. No offscreen, SDK
stub or controlled-clock result establishes those release gates.


### User-requested large-file recovery on the local machine

The running native candidate is code commit `c758b4d`, version
`0.1.1-dev.152+gc758b4d84a59`, dirty false. All five post-commit identity/runtime/
tray checks passed in 22.55 seconds, socket maximum 67 bytes. Restart reused the
saved configuration, journal, staged artifacts and local keychain session; the
new runtime socket was 37 bytes. Only the Proton profile deadline changed from
600,000 to 3,600,000 ms through the official MCP helper and production CLI;
every other configuration field and the accepted operation were checked unchanged.

The user requested recovery of the failed 2,851,612,672-byte file. MCP Retry
accepted a separate one-hour budget while preserving its original 600,000-ms
destination snapshot and publication ID. The typed unfinished-draft collision
was observed in the actual operation, followed by successful renamed transfer.
MCP observed real counters at 4,194,644, 583,055,516, 1,564,602,212 and
2,349,000,640 bytes, then durable `completed` / `uploaded` / `shared` with a
confirmed receipt, available link and empty error. The final SDK traffic counter
was 2,851,843,843 bytes; it includes encryption overhead and has no matching total.
No file contents, names, IDs, credentials or capability URL were included in
committed documentation or external review material.
The older failed operation and unknown drafts were preserved.

This establishes authenticated local production-adapter recovery, large-file
completion and share creation. It does not establish anonymous recipient download,
actual host toast rendering, total-process memory or current compositor acceptance.
Those release gates remain pending; the feature PR remains a draft.
