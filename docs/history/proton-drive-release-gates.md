# Proton Drive: evidence and release gates

> **Historical record.** This page describes an app version, experiment or plan that no longer exists and is kept for context only. The current design is in [overview](../overview.md); see [proton-drive](../proton-drive.md). Do not use it as a work list.

Reconciled: 2026-10-15. Initial audit source: main
`50fd37cafafcc4611925642b93dccb9d9f0949ab`. Subsequent tested candidates
are identified with their individual receipts below.
Issue of record: [#10](https://github.com/patrick-hudson/put.gd-desktop/issues/10).

## Product decision

The user approved retaining, supporting and shipping Proton Drive to production
as an **experimental integration** on 2026-10-15. put.gd owns its integration,
packaging, compatibility updates and recovery behavior. Upstream endorsement or
SDK general availability is **not a release gate**. The experimental designation
communicates compatibility maturity; production distribution remains the goal.

Proton's [current SDK guidance](https://github.com/ProtonDriveApps/sdk/blob/main/README.md#current-status),
checked on 2026-10-15, still describes third-party production use as unready and
estimates a cryptographic migration for late 2026/early 2027. The project accepts
that upstream support position and the risk of service/interface changes. The
migration date is provisional. Compatibility, artifact redistribution and actual
service behavior still require the concrete checks below.

The initial runtime scope remains Linux x86-64. Windows/macOS Proton runtimes,
sync, mounts, recursive uploads, raw-content links, overwrite and remote deletion
are outside this acceptance scope. Existing S3 and local-only behavior remains
supported. Preserve the documented third-party identity, keychain isolation,
create-only writes, fresh ownership checks and explicit recovery contracts.

## Evidence reconciliation

These rows credit only their recorded source and workload. The initial audit read
sources and existing records only. The subsequent [P1 campaign](../evidence/2026-10-15-proton-p1-recovery.md)
built and tested its fingerprinted candidate and exercised an isolated account
folder. The [normal-use application follow-up](../evidence/2026-10-15-proton-p1-app-recovery.md)
adds Cinnamon recovery and post-use idle evidence. The later
[P2 resource campaign](../evidence/2026-10-15-proton-p2-resources.md) measures active
SDK process families and rejects a memory experiment. None certifies final main.

| Scope | Evidence credited | Remaining limit |
| --- | --- | --- |
| Provider integration and shared pipeline | Merged [PR #25](https://github.com/patrick-hudson/put.gd-desktop/pull/25), head `39d8d3ec1a303123e8384f1ec1181518b83d2037`; [integration checkpoints](proton-drive-verification.md) record S3/local regressions, configuration, receipts, history and sharing recovery | PR description's draft/pending language describes an earlier checkpoint; merge is confirmed. It is not blanket service acceptance. |
| Automated checks | Integration checkpoints record full 59/59 dev-preset runs; final large-file helper follow-up records 32 tests / 149 assertions and compiled stdio checks | Full TypeScript checking still reports four upstream Crypto/Bun declaration errors. No claim of a clean type-check or a new test run on main. |
| Authenticated local uploads | Integration checkpoints record private/share probes, real SDK progress and own-link creation/revocation. Large-file recovery at `c758b4d` completed 2,851,612,672 source bytes through explicit same-operation retry | This proves the recorded local recovery, not anonymous download or all service failures. Older unknown drafts were preserved. |
| Recipient download, password and expiration | Fresh [P1 recipient receipt](../evidence/2026-10-15-proton-p1-recovery.md): unsigned-in public/protected downloads matched source hashes, password cases passed; shortened live expiry and revocation blocked access | The historical receipt was not recoverable. This is new service evidence, not final packaged/native acceptance. |
| Live native S3 and Proton tray/clipboard workflows | Merged [PR #30](https://github.com/patrick-hudson/put.gd-desktop/pull/30) records source `9f186e9dff667a9f550b3db4c89e8a042dfbc9a3`, tree `5dad7cfb2b59bb22bf22e7a97a166be60c9f62aa`, count 169: Cinnamon first, then GNOME/KDE; queue/concurrent admission, cancellation/retry, exact link paste, local recovery, restart and tray-host recovery | Credited from the PR acceptance record, not a new raw-artifact replay. Proton recipient decrypted download and active-helper memory were outside that campaign. Same-account helpers serialize. |
| Cold/resident memory | PR #30 records two five-minute physical whole-family cold peaks of 25.09/25.18 MiB and VM resident observations of 25.53 MiB Cinnamon, 27.46 MiB GNOME and 29.06 MiB KDE | VM samples exclude the compositor and do not measure authenticated active-helper memory. The cold target is not an active-upload budget. |
| Later shared tray changes | [Issue #33 native receipt](../evidence/2026-10-14-storage-tray-native.md), source `061c2d9c569dc10b5c8681a7ad7beb3093e61b80`: three-desktop rendered states and two post-use memory windows passed | Uses a synthetic offline S3 wire oracle, not a live Proton account. Do not substitute it for the earlier live Proton campaign. |
| Later settings changes | [Issue #37 record](../evidence/2026-10-14-settings-drafts.md), final tested candidate `337773f4d5cef719b3f54d59bdb336ba3121f453`: Cinnamon graphical/memory passed; KDE 59/59 and GNOME 60/60 development suites passed | Final host suite was 58/59 before a passing tmpfs retry. KDE graphical work stopped after a cancellation timeout; remaining KDE/GNOME graphical/memory work was not completed. Current-main three-desktop acceptance remains open. |
| Bundling and package integrity | Integration checkpoints record repeated offline helper builds, stale-source rejection, installed runtime/manifest/notices checks. PR #30 records an ordinary distribution DEB audit | Historical artifact evidence only. The final P4/P6 receipt below supersedes the former shipping-pipeline, notices and format gaps for its recorded candidate. |
| Authentication isolation and disposable lab setup | [Supported setup](../proton-drive.md), [native replay](../testing-tray-progress.md) and PR #30 describe the private keychain bridge, serialized refreshed-state handoff and removal of copied guest entries | This does not establish expired/revoked session acceptance. Never revoke a copied shared session to manufacture a disposable-session failure. |
| Final experimental Proton candidate | [P4/P6 receipt](../evidence/2026-10-15-proton-p4-p6-acceptance.md), clean `a5112bc`: four audited artifacts, source export/rebuild, Cinnamon-first then GNOME/KDE native checks, Fedora/AppImage smoke checks, anonymous download and secure session cleanup | Bounded VM workload; P1 controlled-only cases and four upstream type diagnostics remain disclosed. Merge local work before closing #10; wider application certification remains separate. |

The historical pending statements in the integration/helper records retain their
original meaning. PR #30 supersedes their initial pending tray/compositor/setup
claims for its tested scope only. The P1 follow-up above addresses current
recovery. The P2 campaign below adds bounded authenticated process-family
measurements. The final P4/P6 receipt below completes distribution and candidate
checks for its exact artifacts and bounded workload.

## Release gates

P1 is complete for its recorded owned-folder and bounded-response scope.
P2 is complete for its recorded Cinnamon resource workload. P3 notice/source/rebuild
work is complete. The [final shipping/package receipt](../evidence/2026-10-15-proton-p4-p6-acceptance.md) completes P4 and
P6 for clean candidate `a5112bc`; P5 credits the actual maintenance transition.
These are source-specific feature results, not current-main or whole-application
release certification.
Owners are project roles: the integration maintainer owns P1, P2 and P5; the release maintainer owns
P3, P4 and P6. The same person may perform both roles. Before the first production
shipment advertising Proton, retain a dated pass/failure/blocker and exact source,
runtime and package identity for each gate. No future upstream approval is needed.

The user selected the official JavaScript helper for the production path on
2026-10-15. The separate native trial remains unadopted; its committed branch and
all three indexed evidence archives were verified and retained in the P3 record.
No native trial code is part of this release candidate.

### P1 — Recovery and recipient evidence (complete within recorded scope)

Exercise real expiry/refresh/revocation using an independently authenticated
session, folder permissions/quota failures, and changed-revision handling at
upload/sharing boundaries on isolated synthetic items. Verify preserved operation,
artifact/publication and receipt identity, no duplicate confirmed upload, no
unconfirmed copied link, and actionable reconnect/retry behavior. Link creation,
inspection and revocation must retain their original protection settings.

Use bounded controlled service-response fixtures for rate limits and other
failures that cannot safely be induced live. Record the SDK backoff/error path
and separately identify any live case still unavailable; a fixture cannot earn
a live-service pass. Do not exhaust an account or provoke service-wide limiting.

Attach the existing recipient evidence if available: candidate/runtime identity,
clean unsigned-in browser context, source/download byte counts and hashes,
correct/wrong/missing-password results and actual expiration result. Exclude
capability URLs, passwords, tokens and account identities. If it cannot be
recovered, perform a bounded synthetic repeat during final acceptance rather
than inventing a receipt. The historical issue checkbox remains issue-reported; the new repeat is independently documented below.

P1 progress on 2026-10-15: [fresh service and recipient evidence](../evidence/2026-10-15-proton-p1-recovery.md)
now covers recipient hashes/protection/expiry/revocation, access refresh, isolated
session revocation, cancellation/reconciliation/retry, changed revisions, a real
quota rejection and an anonymous viewer write denial. Actual SDK Retry-After
timing/cancellation is tested; ten bounded wrong-password attempts produced no 429.
Fixed swallowed service guidance, revoked-refresh reconnect guidance and an
interrupted-journal copy status that stayed pending. The
[application follow-up](../evidence/2026-10-15-proton-p1-app-recovery.md) completes
normal-use folder-unavailable, crash/restart, real deadline/partial-result and
missing-account-access recovery with native clipboard checks. Retries preserved
identities and confirmed receipts; a scoped SDK listing found ten confirmed nodes
for ten receipts with no duplicate confirmed uploads. Cinnamon post-use memory
passed separately from P2's active-workload requirement.

P1 is complete with the recorded limits. Second-account ACL downgrade was not
performed: supported setup targets owned My files/direct-child folders, whose
unavailability was exercised live. The actual anonymous Viewer denial is service
evidence, not authenticated second-account acceptance. Controlled typed responses
cover permission/429 handling; actual SDK Retry-After and cancellation tests cover
rate-limit backoff. No live 429 was observed and manufacturing one through
wrong-password traffic is not a release requirement. Extended cooldown timing
remains unmeasured. Final packaged multi-desktop evidence stays in P6.

### P2 — Authenticated process-family resources (complete within recorded scope)

Measure production Release first account use, small and large upload/share,
cancellation/retry, repeated use and post-upload idle across resident, UI and all
Proton descendants. Retain sampled RSS/PSS/private memory, swap, CPU, process
identity/lifetime and payload sizes; catch short-lived helpers as well as the
resident. Establish that helpers exit and retained memory does not grow without
bound. Confirm S3/local-only usage does not start the Proton runtime.

Keep the frozen cold/post-use idle protocols and existing idle limits intact.
The user decided on 2026-10-15 to optimize measurable avoidable costs without a
hard total active-memory ceiling, given the SDK/runtime behavior. The pre-run
512 MiB provisional envelope is a diagnostic reference, not an approved release
gate. Freeze the authenticated workload, record its cost and preserve the
separate 40 MiB post-use idle, CPU, swap and helper-lifetime gates.

The [P2 receipt](../evidence/2026-10-15-proton-p2-resources.md) records clean Release
baseline `e558436e30c96d0e68fea14d9429e5ed2b811eaf` in visible Cinnamon/X11:
account inspection, small/large upload/share, cancellation after actual bytes,
original-operation retry and three repeated cycles passed with native paste.
Peak whole-family RSS was 487.25 MiB; two five-minute post-use windows peaked at
31.305 MiB, with no helpers/swap and negligible CPU. All 39 SDK children and one
UI child were observed and reaped; local-only use started zero SDK helpers.
A supported Bun smaller-heap experiment raised peak RSS to 539.96 MiB and sampled
large-stage CPU, so it was reverted; no optimization was retained. Restored
provisioning reproduced the baseline helper hash.

This closes the measured P2 workload under that resource policy, with finite-run
and instrumentation limits retained. It is not an upper bound for every file,
account or desktop, nor physical cold-memory acceptance. Live S3 testing was
explicitly excluded by the user; only local-only behavior and inspected
Proton-only deployment/source boundaries earn the negative-control credit.
Current packaged cross-desktop evidence remains P6.

### P3 — Redistribution and rebuild materials (complete)

The [initial audit](../evidence/2026-10-15-proton-p3-redistribution.md) and
[materials receipt](../evidence/2026-10-15-proton-p3-materials.md) remain historical
records. The [rebuild follow-up](../evidence/2026-10-15-proton-p3-rebuild.md) completes
the remaining native compiler/relink proof using the user-approved disposable
toolchain exception. P3 is complete for notices, source materials and executable
rebuild proof. Public delivery and actual package inclusion remain P4/P6.

Historical npm crypto 2.0.0 ISC notice provenance could not be established. The
approved alternative remains official WebPackages commit
`956a713c0823b365cc512018974eb4fcba47c9dd`, version 2.0.1, with its explicit GPL
grant and original predecessor MIT notices. No later grant is applied retroactively.
The production helper remains
`eeffe97c9c7f473895f790434dabafb13a018b8909a2f5215c3b769e7b7bb539`;
SDK 0.22.2 and official Bun 1.4.2 are unchanged. Historical P1/P2 passes remain
credited to their original helper; fresh final-candidate evidence is required.

All compiler-retained JS locations have supplied notice texts. Bun attribution
preserves the conservative source superset, now supplemented with 77 original
legal files from pinned Rust standard-library preferred source and its vendor
inputs. This is not an exact linked ELF notice inventory. Six source/build-input
assets are catalogued with exact hashes in
[tools/proton/notices/SOURCES.json](../../tools/proton/notices/SOURCES.json).
The new supplement corrects the original native preflight and Argon replay route;
the original sealed source packets and receipts remain unchanged.

Modified Bun Rust, JavaScriptCore C++ and TinyCC C compile and link under LLVM
21.1.8 and Rust nightly 2026-07-20, using local JSC/system ICU in release-local.
The rebuilt helper passes 41 tests, eleven no-account stdio checks and the complete
stdout regression. It calls all three native source markers and compiles/executes
TinyCC code from its embedded runtime. Argon2 scalar/SIMD C builds pass the vector
checks in pinned Emscripten 3.1.45. Preferred OpenPGP source rebuilt with the
modified WASM passes real Argon2 crypto and bidirectional interoperability in a
fixture compiled with the rebuilt native runtime. No native/WASM byte identity is
claimed. These marked proof executables are not production replacements.

The official helper was reprovisioned offline and retains its exact fingerprint.
Actual Proton install-rule fixtures verify delivery and tamper checks for all
twelve redistribution files; they do not establish final package acceptance.
The subsequent [P4/P6 receipt](../evidence/2026-10-15-proton-p4-p6-acceptance.md) records public delivery of all six sealed
packets through two hashed transport parts, the complete application source
export/rebuild, and actual package/live/resource/graphical checks. The original
P3 install-rule fixtures alone did not prove those later results. No S3 operation
was used in this work.

### P4 — Shipping pipeline and supported assets

Linux x86-64 Build and release jobs now explicitly provision the pinned helper,
run its helper/compiled-protocol checks and enable `PUTGD_BUNDLE_PROTON`. The
CMake default remains OFF for unsupported platforms and deliberate builds without
Proton. Windows/macOS do not provision this Linux-only artifact. Packaged users
need no separate Bun, Node or Proton CLI.

Native and AppImage audits require the exact provisioned runtime manifest,
helper hash, mirrored notices, all twelve redistribution files and the source
catalog. AppImage repeats the audit after deployment. Native packages declare
browser-launcher/Secret Service library dependencies; a default browser and
unlocked Secret Service-compatible keychain remain desktop prerequisites.

The curated application source export includes the complete bounded
`tools/proton/` build inputs, pins, tests required by provisioning, notices and
setup instructions. Its production rebuild can provision from those extracted
inputs, enable Proton and verify that the rebuilt helper is the bundled helper.
Credentials, build output and lab evidence remain excluded.

Corresponding-source transport preserves the six sealed P3 packets inside a
hashed gzip tar archive, delivered in two separately hashed parts under GitHub's
per-file size limit. Release collection verifies both parts, every inner packet,
the current catalog and each binary package's helper/catalog hashes before
staging. Both parts and the catalog accompany every installer and outer checksum
list; the combined application ZIP includes the catalog and reconstruction
instructions, with the large source parts available separately. Source-only
publication and final package results are recorded in the final acceptance
receipt; wiring alone does not establish hosted or desktop acceptance.

### P5 — Ongoing SDK and service compatibility

The user approved crediting the maintenance already performed rather than
requiring an artificial additional SDK-version change. P5 is satisfied by the
maintenance procedure below and the actual P3 transition from the unresolved
Crypto 2.0.0 bootstrap to the explicit GPL Crypto 2.0.1 source snapshot. That
transition exercised source/lock selection, guarded source patches, notices,
corresponding sources, exact helper rebuild, modified-source relinking,
protocol/smoke tests and install integrity. P1 supplies the normal-use recovery
contract; P6 checks the resulting packaged helper and preserved operation state.
No second SDK upgrade or server migration was fabricated as acceptance evidence.

Compatibility remains an ongoing release responsibility: required SDK/API/crypto
updates must produce a tested bundled runtime through put.gd releases. An observed
live incompatibility blocks affected publication. Upstream readiness labels do
not add an endorsement gate, and rollback cannot promise compatibility after an
upstream crypto migration.

### P6 — Final candidate and package acceptance

Freeze the final source and runtime after any fixes, notice completion or
packaging changes. Run the documented dev presets, helper tests and compiled
protocol checks and applicable local regressions. The user restricted this
campaign to Proton: no S3 tests, credentials, configuration or service operations
are part of its acceptance evidence. Record the four known
upstream type-check diagnostics explicitly; new diagnostics require resolution
or a bounded documented disposition. Do not call the full type-check passing.

Build and install actual shipping packages. Preserve source/package/binary
hashes, supported-version evidence, and visible native functional/memory results
on Cinnamon/X11 first, followed by GNOME/Wayland and KDE/Wayland. Observe the
remaining general release requirements in [release.md](../release.md) and
[verification.md](../verification.md); this feature audit does not close them.

## Maintenance procedure

1. **Ownership and cadence.** The integration maintainer reviews upstream SDK,
   account foundation, Crypto and Bun releases/security and migration notes
   before each candidate/stable release, when a nightly changes Proton runtime
   inputs, and whenever an announced required update or observed incompatibility
   occurs. Record the check date,
   references, pinned versions, compatibility result and next action in the
   release receipt. This is a manual responsibility; no monitor was created.
2. **Pinned updates.** Change the upstream commit, runtime/dependency locks and
   any guarded source patches together. Reprovision explicitly, review notices
   and source fingerprints, and verify the helper protocol, version/hash mismatch
   failures, clean install and extracted-source rebuild. Do not float dependencies
   or replace a public release artifact in place.
3. **Compatibility checks.** Test browser authentication and refresh, private
   upload, anonymous protected/unprotected download equality, sharing history,
   interruption/reconciliation and old journal/config/receipt compatibility.
   Confirm fresh account/folder/node/revision proof with the upgraded SDK.
   Preserve credential references and accepted operation identities; record any
   explicit reauthentication needed. Apply P2/P6 to the changed candidate.
4. **Migration or incident.** A required-update response or newly incompatible
   service stops affected publication with a useful update/reconnect result.
   Preserve local archives, journals, unknown outcomes and offline history for
   explicit inspection/retry after the compatible release. Verify this behavior
   rather than assuming the current generic error proves it. Do not automatically
   replay mutations or redirect work to S3. Publish a new tested put.gd version.
5. **Rollback.** Retain the prior verified package/runtime and test compatibility
   of its journals/configuration before offering downgrade. After a server crypto
   migration an old SDK may be unusable; do not promise rollback restores service.
   Keep user data and recovery identities intact while preparing a forward fix.

## Issue closure and next work

The [final receipt](../evidence/2026-10-15-proton-p4-p6-acceptance.md) completes the feature-specific P4/P6 checks at
`a5112bc` and credits P5 using the completed Crypto source transition. It retains
failed attempts, the Fedora dependency and GNOME MIME fixes, actual package
hashes, source delivery/rebuild, native functional/resource evidence and secure
session cleanup. P1/P2/P3 remain credited at their recorded scopes and sources.

The wrap-up implementation and acceptance documentation remain local until
merged. Close #10 with that merge and the final receipt. The source-only release
is published; a tagged application release/full hosted CI, physical performance,
and wider U9/U10/desktop certification remain separate project release work.
No current-main certification or S3 regression credit is implied.
