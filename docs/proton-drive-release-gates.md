# Proton Drive: evidence and release gates

Reconciled: 2026-10-15. Source inspected: main
`50fd37cafafcc4611925642b93dccb9d9f0949ab`.
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
sources and existing records only. The subsequent [P1 campaign](evidence/2026-10-15-proton-p1-recovery.md)
built and tested its fingerprinted candidate and exercised an isolated account
folder. Neither campaign measures authenticated memory or certifies final main.

| Scope | Evidence credited | Remaining limit |
| --- | --- | --- |
| Provider integration and shared pipeline | Merged [PR #25](https://github.com/patrick-hudson/put.gd-desktop/pull/25), head `39d8d3ec1a303123e8384f1ec1181518b83d2037`; [integration checkpoints](proton-drive-verification.md) record S3/local regressions, configuration, receipts, history and sharing recovery | PR description's draft/pending language describes an earlier checkpoint; merge is confirmed. It is not blanket service acceptance. |
| Automated checks | Integration checkpoints record full 59/59 dev-preset runs; final large-file helper follow-up records 32 tests / 149 assertions and compiled stdio checks | Full TypeScript checking still reports four upstream Crypto/Bun declaration errors. No claim of a clean type-check or a new test run on main. |
| Authenticated local uploads | Integration checkpoints record private/share probes, real SDK progress and own-link creation/revocation. Large-file recovery at `c758b4d` completed 2,851,612,672 source bytes through explicit same-operation retry | This proves the recorded local recovery, not anonymous download or all service failures. Older unknown drafts were preserved. |
| Recipient download, password and expiration | Fresh [P1 recipient receipt](evidence/2026-10-15-proton-p1-recovery.md): unsigned-in public/protected downloads matched source hashes, password cases passed; shortened live expiry and revocation blocked access | The historical receipt was not recoverable. This is new service evidence, not final packaged/native acceptance. |
| Live native S3 and Proton tray/clipboard workflows | Merged [PR #30](https://github.com/patrick-hudson/put.gd-desktop/pull/30) records source `9f186e9dff667a9f550b3db4c89e8a042dfbc9a3`, tree `5dad7cfb2b59bb22bf22e7a97a166be60c9f62aa`, count 169: Cinnamon first, then GNOME/KDE; queue/concurrent admission, cancellation/retry, exact link paste, local recovery, restart and tray-host recovery | Credited from the PR acceptance record, not a new raw-artifact replay. Proton recipient decrypted download and active-helper memory were outside that campaign. Same-account helpers serialize. |
| Cold/resident memory | PR #30 records two five-minute physical whole-family cold peaks of 25.09/25.18 MiB and VM resident observations of 25.53 MiB Cinnamon, 27.46 MiB GNOME and 29.06 MiB KDE | VM samples exclude the compositor and do not measure authenticated active-helper memory. The cold target is not an active-upload budget. |
| Later shared tray changes | [Issue #33 native receipt](evidence/2026-10-14-storage-tray-native.md), source `061c2d9c569dc10b5c8681a7ad7beb3093e61b80`: three-desktop rendered states and two post-use memory windows passed | Uses a synthetic offline S3 wire oracle, not a live Proton account. Do not substitute it for the earlier live Proton campaign. |
| Later settings changes | [Issue #37 record](evidence/2026-10-14-settings-drafts.md), final tested candidate `337773f4d5cef719b3f54d59bdb336ba3121f453`: Cinnamon graphical/memory passed; KDE 59/59 and GNOME 60/60 development suites passed | Final host suite was 58/59 before a passing tmpfs retry. KDE graphical work stopped after a cancellation timeout; remaining KDE/GNOME graphical/memory work was not completed. Current-main three-desktop acceptance remains open. |
| Bundling and package integrity | Integration checkpoints record repeated offline helper builds, stale-source rejection, installed runtime/manifest/notices checks. PR #30 records an ordinary distribution DEB audit | Demonstrates those artifacts. Standard release workflows do not yet provision/enable Proton; existing package audits do not establish complete Proton notices or all shipping formats. |
| Authentication isolation and disposable lab setup | [Supported setup](proton-drive.md), [native replay](testing-tray-progress.md) and PR #30 describe the private keychain bridge, serialized refreshed-state handoff and removal of copied guest entries | This does not establish expired/revoked session acceptance. Never revoke a copied shared session to manufacture a disposable-session failure. |

The historical pending statements in the integration/helper records retain their
original meaning. PR #30 supersedes their initial pending tray/compositor/setup
claims for its tested scope only. It does not close today's recovery, helper
memory, distribution or final-candidate gates.

## Open release gates

All gates below remain open after this documentation audit. Owners are project
roles: the integration maintainer owns P1, P2 and P5; the release maintainer owns
P3, P4 and P6. The same person may perform both roles. Before the first production
shipment advertising Proton, retain a dated pass/failure/blocker and exact source,
runtime and package identity for each gate. No future upstream approval is needed.

### P1 — Recovery and recipient evidence

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

P1 progress on 2026-10-15: [fresh service and recipient evidence](evidence/2026-10-15-proton-p1-recovery.md)
now covers recipient hashes/protection/expiry/revocation, access refresh, isolated
session revocation, cancellation/reconciliation/retry, changed revisions, a real
quota rejection and an anonymous viewer write denial. Actual SDK Retry-After
timing/cancellation is tested; ten bounded wrong-password attempts produced no 429.
Fixed swallowed service guidance and revoked-refresh reconnect guidance. P1 remains
open for authenticated application folder-access loss and final application
recovery/clipboard/restart acceptance; controlled cases are
credited separately. Actual SDK Retry-After and cancellation tests cover the rate-limit
case; a live 429 was not observed and manufacturing one through wrong-password
traffic is not a release requirement. Extended cooldown timing remains a stated
coverage limit. This is not native or resource acceptance.

### P2 — Authenticated process-family resources

Measure production Release first account use, small and large upload/share,
cancellation/retry, repeated use and post-upload idle across resident, UI and all
Proton descendants. Retain sampled RSS/PSS/private memory, swap, CPU, process
identity/lifetime and payload sizes; catch short-lived helpers as well as the
resident. Establish that helpers exit and retained memory does not grow without
bound. Confirm S3/local-only usage does not start the Proton runtime.

Keep the frozen cold/post-use idle protocols and existing idle limits intact.
Define the authenticated workload and its active-transfer budget before running
acceptance; no separate authenticated active budget was found in the inspected
records. Report it separately from the approximately 40 MiB idle target. Start
with Cinnamon functional and memory checks before sibling expansion.

### P3 — Redistribution and rebuild materials

Finish the exact embedded dependency inventory, distinguishing bundled code from
build-only/optional installed packages. Resolve the missing supplied license
texts identified in [the notice inventory](../tools/proton/notices/README.md),
include Bun's embedded component notices for the pinned executable, and record
the applicable Proton OpenPGP redistribution/rebuild materials. The current
inventory is an input to this review, not its completion receipt.

Verify the final package contains the reviewed notices, exact runtime/manifest,
locked recipe and required source/rebuild materials. Retain hashes and review
outcome. This audit makes no new license-compliance claim and changes no license
inventory or helper fingerprint.

### P4 — Shipping pipeline and supported assets

[cmake/Proton.cmake](../cmake/Proton.cmake) defaults `PUTGD_BUNDLE_PROTON` to OFF.
The inspected [.github/workflows/build.yml](../.github/workflows/build.yml) and
[release.yml](../.github/workflows/release.yml) neither provision the runtime nor
enable that option. The native package auditor has no Proton-specific inventory
gate. Standard build/package success therefore cannot establish Proton delivery.

Add pinned provisioning and helper test/smoke coverage to the intended Linux
release path, explicitly enable the bundle, and require runtime/manifest/notices
integrity in the package audits and release receipts. Cover each advertised
DEB/RPM/AppImage asset with its actual ABI and keychain/browser runtime needs.
Record unsupported assets explicitly; a Linux x86-64 lock is not Fedora desktop
or cross-format acceptance. Users must not need a separate Bun, Node or CLI.

[tools/releases/source_archive.py](../tools/releases/source_archive.py) currently
excludes `tools/proton/` through its positive allowlist. Include the Proton
production helper sources, provisioning/locks/notices and setup instructions in
the curated source/rebuild handoff; verify an extracted Proton-enabled build.
Keep credentials, build output and lab evidence excluded. Preserve the existing
stable/candidate exact-package and nightly automated-coverage distinctions.

### P5 — Ongoing SDK and service compatibility

Adopt the maintenance procedure below and exercise its upgrade/recovery path
before first shipment. Required SDK/API/crypto updates must produce a tested
bundled runtime through put.gd releases. A live compatibility failure remains a
release blocker even though upstream's readiness label does not.

### P6 — Final candidate and package acceptance

Freeze the final source and runtime after any fixes, notice completion or
packaging changes. Run the documented dev presets, helper tests and compiled
protocol checks, plus applicable S3/local regressions. Record the four known
upstream type-check diagnostics explicitly; new diagnostics require resolution
or a bounded documented disposition. Do not call the full type-check passing.

Build and install actual shipping packages. Preserve source/package/binary
hashes, supported-version evidence, and visible native functional/memory results
on Cinnamon/X11 first, followed by GNOME/Wayland and KDE/Wayland. Observe the
remaining general release requirements in [release.md](release.md) and
[verification.md](verification.md); this feature audit does not close them.

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

Issue #10 remains open for P1/P2 and the concrete distribution/maintenance gates
P3–P6. Its body was reconciled on 2026-10-15 with the accepted product decision,
these gate IDs and the source-specific evidence limits. It remains open. The
existing recipient checkbox stays checked; its historical provenance remains issue-reported and the fresh P1 repeat is linked above; the
stale “four groups” instruction was replaced. The historical requirements below
the issue’s disclosure section were preserved verbatim.

Next work is the remaining P1 acceptance and P2 resource characterization, followed
by P3/P4 distribution work and the P5 update rehearsal, then P6 acceptance on the
resulting candidate. This completes the evidence reconciliation and release-scope
record for wrap-up steps 1–2 and the subsequent bounded P1 campaign. It does not
mark P1–P6 passed. Local documentation links and cited Git objects/ancestry were
checked; P1 build/test scopes and outstanding gates are recorded in its receipt.
