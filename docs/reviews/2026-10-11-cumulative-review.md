# Cumulative code review — 2026-10-11

The review completed with ten confirmed P2 findings. No fixes were applied. This is a draft publication checkpoint; live-S3 and full release acceptance remain pending.

Reviewed source: `050b702c6a6da5c945ea8b4e138dee50e6fdbd83` (`0.1.1-dev.63+g050b702c6a6d`), against bootstrap `cfb29542ffc28486cb876442373f1d40a20ce0c6`. Ten lenses reviewed the cumulative source packet; historical evidence logs were inspected selectively. The [machine-readable report](2026-10-11-cumulative-review.json) preserves evidence, validation and proposed tests.

## Findings

| ID | Finding | Location |
| --- | --- | --- |
| 1 | Stale Cinnamon helper passes package identity verification | [cmake/CheckBuildIdentity.cmake.in:10](../../cmake/CheckBuildIdentity.cmake.in#L10) |
| 2 | Recent operations return stale remote outcomes | [src/core/HistoryStore.cpp:138](../../src/core/HistoryStore.cpp#L138) |
| 3 | Preserve remote outcomes when clearing history | [src/core/HistoryStore.cpp:154](../../src/core/HistoryStore.cpp#L154) |
| 4 | Notify open views when history mutations commit | [src/app/HistoryService.cpp:82](../../src/app/HistoryService.cpp#L82) |
| 5 | Reserve artifacts against retries during cleanup | [src/app/OperationService.cpp:351](../../src/app/OperationService.cpp#L351) |
| 6 | CWE-214: CLI accepts raw credential setup in argv | [src/cli/main.cpp:20](../../src/cli/main.cpp#L20) |
| 7 | Recover accepted deletions after resident restart | [src/core/HistoryStore.cpp:116](../../src/core/HistoryStore.cpp#L116) |
| 8 | Per-item checkpoints rewrite every history row in the batch | [src/core/HistoryStore.cpp:58](../../src/core/HistoryStore.cpp#L58) |
| 9 | Reject corrupted images outside the sampled red pixels | [tools/lab/lab.py:707](../../tools/lab/lab.py#L707) |
| 10 | Preserve all generated witnesses in each attempt bundle | [tools/lab/lab.py:830](../../tools/lab/lab.py#L830) |

### 1. Stale Cinnamon helper passes package identity verification

A partial rebuild can produce a successfully installed package whose Cinnamon region, window and desktop actions all fail with capture_incompatible. The install guard checks the resident, CLI, UI and optional KDE selector, but never putgd-capture, even though that separately built executable is installed and must match the resident's exact build manifest. Adding it to the same optional-target check closes the green-package/red-capture gap.

Proposed resolution: Add a PUTGD_PACKAGE_CAPTURE target-file expression guarded by TARGET_EXISTS:putgd-capture in BuildVersion.cmake, append putgd-capture and resolve its binary in CheckBuildIdentity.cmake.in when present, and extend version_contract.cmake with stale-capture install/CPack rejection and absent-X11 cases, matching the existing KDE helper checks.

Validation: For a partial rebuild leaving putgd-capture stale, CheckBuildIdentity.cmake.in:10-23 checks only the resident, CLI, UI and optional KDE helper while CMakeLists.txt:229-231 installs the capture helper and CaptureProcessController.cpp:138 rejects its mismatched manifest, establishing a reachable package/capture incompatibility introduced by this cumulative diff; actual package incidence was not measured.

### 2. Recent operations return stale remote outcomes

After an uploaded item is deleted or superseded, operation.recent still reports its old remoteStatus, commonly not_requested, while operation.inspect and history.detail report removed, changed or superseded. Activity renders that stale status directly, so refreshing or reopening Activity continues to contradict History about the same publication. The single-operation reader already overlays authoritative history statuses; applying the same hydration to recent results makes the public service responses agree without changing original receipts.

Proposed resolution: Extract the history remote_status hydration currently used by HistoryStore::operation into a shared helper and apply it to every operation returned by HistoryStore::recent, preferably with a bounded bulk query for the selected operation IDs. Add a regression that completes an upload, deletes or supersedes its item, then verifies operation.recent, operation.inspect, history.detail and the refreshed Activity remote label agree without a later operation rewrite.

Validation: After deletion or supersession updates history through HistoryStore.cpp:70-71 or 161-164, recent():138 reads the untouched operation blob unlike operation():100-101, and OperationService.cpp:266 plus RecentOperations.cpp:23,57 expose that stale remoteStatus; an independent in-memory trace of the inspected queries returned removed for inspect but not_requested for recent.

### 3. Preserve remote outcomes when clearing history

After deleting an upload and clearing history, Copy result from Activity or operation.copy can write that known-deleted URL to the clipboard again. Deletion updates only history, while operation() overlays remote outcomes from that table onto an older operations document; clear() and expiry remove the only current state, restoring not_requested and making linkAvailable() true. Keep the per-item remote outcome independently of the clearable history view so local record cleanup cannot undo a known removal.

Proposed resolution: Persist last-known per-item remote outcomes in the operation journal or a separate item-state table that history.clear and expiry do not prune. Merge that state on operation reads and writes, including deletion and supersession updates; add a regression for delete -> clear/expire -> restart -> operation.copy that must not copy the removed URL.

Validation: When a completed deletion is followed by clear or eligible expiry without an intervening operation rewrite, HistoryStore.cpp:145,154 removes its only remote-state overlay and cleared_history retains only IDs, while OperationService.cpp:286,302 and Operation.cpp:3 again permit its uploaded URL; an independent in-memory trace reproduced removed becoming not_requested and linkAvailable=true after clear.

### 4. Notify open views when history mutations commit

Remote deletion, history clearing and retention expiry initiated outside the GUI do not invalidate an already-open History view. Its table, inspector and enabled link actions retain old remote state until manual refresh, reopening or an unrelated operation/settings event; GUI-initiated deletion hides the gap by polling only its own deletion ID.

Proposed resolution: Add a HistoryService change signal and connect it to the resident UI invalidation path. Emit on the session thread after successful clear/expiry and persisted deletion acceptance/progress/completion, coalescing progress. Test an open History view while a separate session client clears or deletes, without explicit refresh or reopen.

Validation: For an open History view and a separate-client clear/delete or retention expiry, HistoryService.cpp:17,21,82 publishes no change signal and main.cpp:222-225 excludes HistoryService from invalidation, while HistoryWindow.cpp:64-65,119-120 polls only GUI-initiated deletions and UiSession.cpp:10 refreshes views only from snapshots, establishing stale views until another refresh event; runtime incidence was not measured.

### 5. Reserve artifacts against retries during cleanup

An explicit retry of an aged failed upload can lose its only retained bytes if cache.clear was dispatched immediately before it. cleanup() freezes active IDs before starting an independent worker; retry() can then reserve the same operation while cleanup still considers its previously failed record eligible and removes its artifact. The existing active-retry test only covers the opposite request order, so it cannot establish the intended ownership protection.

Proposed resolution: Set a cleanup-in-progress admission guard on the session thread before starting cleanup and reject retry/retryLocalSave with a busy error until its callback clears that guard; preserve the existing exclusion of already-active IDs. Alternatively reserve cleanup candidate IDs against retry admission. Add a deterministic test that pauses cleanup after its snapshot, requests retry, and verifies the artifact cannot be removed from an admitted retry.

Validation: For an aged failed operation eligible for cleanup, OperationService.cpp:351 snapshots active IDs before launching an independent QThread through :63-66, yet retry():274-282 and retryLocalSave():285 can subsequently admit that ID without a cleanup guard while ArtifactStore.cpp:31 removes its spool, making the admitted retry lose retained bytes under a reachable request ordering; the precise concurrent schedule was not executed.

### 6. CWE-214: CLI accepts raw credential setup in argv

A caller can successfully supply accessKey, secretKey and sessionToken to setup.storeCredentials through the CLI's positional JSON argument. Those values are then exposed in the process command line and potentially shell history, even though the setup protocol explicitly promises credentials never enter arguments. The CLI forwards every method without a public-method or credential-field guard, so the GUI's new secret-bearing socket endpoint has also become an argv-based interface; restricting this method to the existing private GUI socket flow closes that unintended route. See [MITRE CWE-214](https://cwe.mitre.org/data/definitions/214.html) for the process-argument exposure mechanism.

Proposed resolution: Reject setup.storeCredentials in putgd-cli before opening the session socket, with fixed guidance to enter credentials through Settings. Keep the GUI's SessionClient endpoint intact. Add a synthetic CLI contract test proving this method cannot reach the resident; if credential setup via CLI is intentionally desired later, implement a bounded stdin/private-descriptor input path instead of positional JSON.

Validation: For a saved profile with a current expectedRevision, CLI main.cpp:16-20 accepts positional credential JSON and forwards setup.storeCredentials without restriction to LocalServer.cpp:105-109 and SetupService.cpp:55-57, exposing input in argv contrary to docs/ui-process-protocol.md:37-39, with test_ui_endpoints.cpp:161-163 evidencing endpoint acceptance; the argv route was inspected without passing any credentials or measuring user incidence.

### 7. Recover accepted deletions after resident restart

A resident crash after accepted deletion persistence but before the running checkpoint leaves that deletion marked accepted after restart, with no owning worker. History treats accepted as active and polls indefinitely, while cancellation returns delete_not_active. Explicit retry is available but the recorded state does not indicate that user action is needed; recover accepted requests as interrupted without replaying remote deletion.

Proposed resolution: Recover deletions in both accepted and running states as interrupted. Preserve pending item statuses and exact receipts/progress for accepted deletions, and convert only deleting items to unknown as currently done; add a restart regression seeded with an accepted deletion and verify inspect returns interrupted, no provider removal is replayed, and explicit retry works.

Validation: A crash between HistoryService.cpp:43-45 committing accepted deletion and :49 committing running leaves a reachable accepted document that HistoryStore.cpp:116 never recovers, while HistoryService.cpp:89 has no active owner to cancel it and HistoryWindow.cpp:120 continues polling accepted indefinitely; the crash window was established from checkpoints but not executed.

### 8. Per-item checkpoints rewrite every history row in the batch

Sharing a multi-file clipboard batch performs quadratic journal work, including on the local-only path. OperationService saves the whole operation after each archived item, name check, and upload transition, while each save executes two lookups and an upsert for every item: the default permitted 100-file local batch alone performs roughly 30,000 history statements during its archive checkpoints. This unnecessary work delays completion and occupies the bounded operation worker; the supported 10,000-file configuration magnifies it to roughly 300 million statements for that phase alone.

Proposed resolution: Add a transactional checkpoint API that accepts the changed item IDs and updates only their history rows alongside the durable operation snapshot. Populate all history rows once after acquisition, pass the affected item ID at per-item archive/name/upload checkpoints, and retain one full pass for transitions that intentionally change every item. Preserve the existing remote-state merging, clear tombstones, replacement supersession, and receipt-before-publication ordering; verify SQL work scales linearly for a 100-file local batch and crash recovery remains correct.

Validation: For an admitted multi-file local batch, OperationService.cpp:178 checkpoints after each archived item through :169 and HistoryStore.cpp:58-68 performs three history statements per item on every checkpoint, yielding quadratic ledger work under Settings.h:22 and Settings.cpp:154 limits without a changed-item guard; wall-clock incidence and speedup were not measured.

### 9. Reject corrupted images outside the sampled red pixels

The graphical harness can report successful capture and native paste when most image pixels are wrong: clipboard-image checks only dimensions and one red pixel, and window/desktop likewise inspect one red pixel. Region also selects an entirely uniform red rectangle, so an equally sized crop shifted within that quadrant is indistinguishable from the requested crop. The independent public/artifact hash comparison proves transport consistency but cannot establish that the transported image is correct; the existing negative host test changes a sampled color or dimensions and never exercises these false positives.

Proposed resolution: For clipboard-image, compare every decoded pixel against the independently generated synthetic image. Make the region selection cross the existing four-color fixture boundaries and compare the complete expected crop, using the independent pre-action geometry; verify the full deterministic paint area for window/desktop. Extend test_exact_pixels_reject_wrong_crop_and_clipboard with a correctly sized image corrupted away from the old sample points and a same-size shifted crop, asserting rejection for both saved artifacts and paste witnesses.

Validation: The current oracle in lab.py:156-167,704-711 checks dimensions and only selected red pixels, while :567,664-666 selects a uniform red fixture quadrant from desktop_fixture.cpp:85 and test_lab.py:102-112 omits unsampled corruption, and independent execution of the actual compare_ppm functions in memory accepted a correctly sized clipboard image with pixel (0,0) corrupted.

### 10. Preserve all generated witnesses in each attempt bundle

A later graphical attempt overwrites selector/focus screenshots and the independent History copy paste witness from an earlier attempt, because the per-attempt archive omits those files. This leaves failed selector setup and History copy claims with only summarized outcomes after reuse of the run directory, preventing a reviewer from independently checking the observed state. The documentation promises preserved per-attempt synthetic paste witnesses, while the host journal test checks event JSON only and does not verify bundle contents.

Proposed resolution: Track the synthetic witness paths actually produced by the current fixture_run attempt, including focus/selector QMP screenshots and history-paste.json, and copy/hash them into RUN/tests/ID alongside the existing artifact list. Reset or separately track optional witness files so a fresh attempt cannot archive stale ones. Add a host-only bundle test that creates two attempts with different witness bytes and verifies each preserved bundle and manifest retain that attempt's bytes.

Validation: When fixture_run is repeated in one run directory, lab.py:383-389 reuses screenshot filenames and :782 reuses history-paste.json, but the per-attempt copy/hash list at :830-840 excludes focus/selector screenshots and that History witness despite docs/testing.md:38-44 promising preserved attempt witnesses, so later attempts overwrite evidence absent from the prior bundle; a repeated guest run was not executed.

## Acceptance and review limits

The [memory strategy](../memory-strategy.md) retains the measured local source63 results and exact workloads. Those results do not close these defects or the current guest live-S3, remote History copy/delete, post-S3 memory, full fresh-agent walkthrough, package lifecycle and broader compositor release gates. The image-oracle finding limits the strength of existing graphical image assertions. Existing attempts and measurements are preserved.

Claude’s review adapter failed with a concurrent OAuth refresh conflict. A subsequent installed-CLI authenticated request succeeded. The adversarial review was completed locally; independent cross-model corroboration is absent. The validator confirmed all ten findings in one batch; the normal eight-item cap overage is recorded in the original report. Some findings were confirmed from reachable code paths without executing their crash or concurrency schedules.

The separately committed [allocator diagnostic annex](2026-10-11-allocator-annex-review.json) at `aea2238702a3a2b3a6c41f5b8d525c3f7c2b3a76` received a narrow static review with no actionable findings. It is not product code or new memory acceptance. Subsequent integration changes documentation and adds that diagnostic; production source, CMake/configuration and registered tests remain unchanged from source63.

Follow-up order: retain durable remote outcomes and recover accepted deletions; protect cleanup/retry admission and credential entry; then fix view invalidation, package identity, batch checkpoint work and graphical evidence.
