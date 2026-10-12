# Live-S3 memory acceptance

The [source114 transport regressions](testing-live-s3-transport.md) pass on Cinnamon, GNOME and KDE, including live upload, production History deletion and five-minute post-use memory. Historical failures remain recorded. [Replayable source114 evidence](evidence/live-s3-memory/source114/README.md) preserves exact versions and qualifications; [lab storage safeguards](lab-storage.md) gate capacity and retention.

This protocol tests closed-view residency after real uploads. It does not promise a fixed ceiling during active capture/upload or while Settings/History is open. Use synthetic inputs, a unique test namespace, and exact receipt-guarded cleanup. No bucket policy, versioning or existing objects may change.

Freeze the candidate first: save the clean source commit/tree, generated version, full build manifest, installed executable/module/SDK hashes, package hash and runtime PID/start ticks. Build with the standard CMake presets/Ninja and official pinned SDK. A partial target rebuild can mix identities; build all targets consistently before acceptance.

Use one Cinnamon build first. Keep its desktop visible through VNC, verify the unlocked session, and run the unchanged functional suite. Memory acceptance requires both a complete live workload and a fresh-profile repeat. Only expand to current-version GNOME/KDE after Cinnamon passes the required memory and functional gates.

## Fixed workload

Use a fresh private configuration with the existing credential-reference schema and credentials entered in memory through the authenticated setup endpoint. Verify the input is an owned private regular file; never serialize its contents into evidence, command arguments, logs, configuration or review material. Preflight every resolved Unix socket path before startup.

On the fixed 1280 by 800 Cinnamon display, use the native synthetic paint/clipboard fixture. Verify its visible geometry and focus before its source shortcuts. Upload text in batches 1, 5, 5; regions in batches 5, 5, 5; one image; one two-file clipboard list; one active window; one full desktop. That is 30 operations and 31 objects. Regions are 80 by 60 pixels at fixture paint offset 40,40, with every pixel RGB 220,30,40. Keep the recorded selection timing and 20-second between-batch idle intervals.

Save operation IDs immediately. For every uploaded item, verify the actual public response length and SHA-256 against its production receipt. Independently paste the copied direct URL into the fixture through native Ctrl+V. Verify public region pixels. Final operation snapshots must show completion and copied URLs. Do not substitute a mock S3 endpoint for this acceptance.

Before quiet measurement, prepare and inspect deletion for only the exact confirmed History IDs. Validate receipt identity, publication ID, concurrency token, namespace and displayed effect, then save the unchanged acknowledgement. Finish all History/configuration/GUI work before setting the final quiet stage.

## Observation and decision

Sample the complete product process family once per second. Charge transient helpers until their actual exit, including reparented children. The independent paint/paste fixture is test infrastructure and is excluded. Save RSS, PSS, private/anonymous memory, swap, thread counts, CPU ticks, PID/start ticks and executable identity; collect bounded diagnostics and mapped-library names every ten seconds. Never collect heap/core dumps with live secrets.

Allow five seconds of warmup, then score the first complete 300-second quiet window. Require stable identity, no sample gaps above the frozen limit, one remaining resident, no UI helper or active operation, zero swap, and under 0.5 percent of one CPU core. No views, actions, History/configuration queries or test-fixture input during that interval. Observer diagnostics remain part of the fixed workload. Stop the observer, await its completion marker, and only then copy finalized JSONL.

The target is at most 40 MiB RSS plus swap. A measured change must beat the previous matched reference by more than 1 MiB and pass every correctness gate. Keep useful changes cumulatively; standalone measurements do not establish their isolated contribution to a later combination. Preserve failed attempts and corrected commands rather than truncating evidence or replaying an ambiguous upload.

## Cleanup and reproducibility

Confirm only the reviewed exact deletion acknowledgement. Retain the delete operation ID and require every item to reach removed/done. Verify History disables its link. Remove each synthetic local archive only after checking its private owner, namespace, resolved root, length and digest. Clear completed private spools through the production cache command; preserve numeric evidence and History metadata. Never broad-list/delete the bucket or automatically replay an uncertain destructive request.

Reserve the conservative object/byte budget before every upload attempt; retain reservations for failures or uncertainty. Current approved campaign cap: 256 new objects and 64 MiB synthetic input. Record actual created and removed counts separately from reservations.

Saved-evidence evaluation is suitable for CI and uses Python. It independently checks finalized samples and receipts; it does not establish fresh desktop acceptance. Record observer/driver/evaluator hashes, exact source identity, failure receipts, corrected commands, native acceptance receipt and external-review disposition. Never upload protected keys or raw payloads to CI/review.

A fixture input failure before action invocation is an invalid test attempt. Prove zero accepted operations before retrying preparation. A mismatch between source/build/package identities is also invalid evidence. Neither is a reason to weaken the workload or product safety checks.
