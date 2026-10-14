# Issue #32: queued upload clipboard investigation

Issue of record: https://github.com/patrick-hudson/put.gd-desktop/issues/32

This report records the diagnosis before the user approved implementation. Its source references and results describe that baseline, not the subsequent fix.

Baseline: PR #35 before merge, `8d03c4ebc91efc6daf5270ff0f6a2a8b82db3ea2` (product change `061c2d9c569d`; the PR's later commit contains evidence). Investigation branch: `codex/issue-32-clipboard-investigation`. Production source is unchanged. New tests characterize existing behavior; they do not implement or verify a fix.

## Reproduction and scope

The production `OperationService` was exercised with the existing synthetic `Source` and `OpaqueProvider` fixtures, a private temporary journal/destination, automatic completion copies, and a single upload slot. Provider callbacks were held until all separate operations had been accepted, then released in invocation order. The source revision was asserted to remain zero throughout acceptance. Clipboard payload snapshots and copy call counts were checked at each completion.

| Scenario | Observed automatic clipboard outcome |
| --- | --- |
| One operation | Its confirmed link copies once. |
| Two separate queued operations | First is `suppressed`, with zero copy calls; second copies once. |
| Three separate queued operations | First and second are `suppressed`, with zero copy calls; third copies once. |
| One operation, user edits clipboard before completion | Copy is suppressed; synthetic user text remains. |
| Three queued operations, user edits clipboard before completion | All copies are suppressed; synthetic user text remains. |
| One two-file batch | No intermediate copy; both confirmed links copy together, in source order. |
| One three-file batch | No intermediate copy; all three confirmed links copy together, in source order. |

Every upload in these probes completed successfully and retained a confirmed link. Earlier suppressed completions made no clipboard write and preserved the initial synthetic clipboard text. Explicit `copyResult` recovered the first operation's link in the suppressed scenarios without increasing the provider upload count.

The initial probe expected a copy at each normal queued completion. It failed specifically for the two- and three-operation rows (`suppressed` versus `copied`), establishing the reported symptom. The final probe instead asserts the observed existing policy and the recovery path. Passing characterization tests must not be described as a fix.

## Causal chain

1. `src/app/OperationService.cpp:126` assigns every accepted operation a new invocation ticket by calling `invalidateCopyEligibility()`, and snapshots the clipboard revision.
2. `src/app/OperationService.h:87` invalidates adapter-level deferred copies and increments the single service-wide `invocation_` counter. Acceptance of a later queued operation therefore supersedes an earlier operation immediately, before the later upload completes.
3. Queueing and completion do not restore that earlier ticket. `src/app/OperationService.cpp:356` sends a successful completion through `performCopy(op, false, ...)`.
4. At `src/app/OperationService.cpp:422`, automatic copy requires both equality with the current invocation ticket and equality with the current clipboard revision. In the two-operation reproduction, ticket 1 loses to 2 while both clipboard revisions are still 0. In the three-operation reproduction, tickets 1 and 2 lose to 3 while revisions remain 0.
5. `src/app/OperationService.cpp:426` persists `copyStatus="suppressed"` and returns before calling the source's copy method. The last operation alone remains eligible, unless a user clipboard change blocks it too.
6. A multi-file batch shares one ticket. Its confirmed links are gathered in item order and joined with newlines at `src/app/OperationService.cpp:427–431`; all links remain in that one automatic result.
7. Explicit `copyResult` at `src/app/OperationService.cpp:412` obtains a fresh ticket and copies the retained receipt, without reuploading.

This is suppression at the shared service boundary, not evidence of a compositor clearing the clipboard or an upload losing its receipt. It explains why more than one result can be skipped. Native compositor clipboard behavior has not been tested in this investigation.

## Existing contract and prior work

Issue #1 and the local implementation plan agree: R8 protects newer clipboard changes and sharing invocations; KTD12 explicitly says only the newest accepted share with an unchanged revision auto-copies. The current behavior follows that approved contract, although it conflicts with the expectation that every queued completion automatically provides its result.

The eligibility comparison dates to `199cced` (`feat(core): add recoverable sharing services and local CLI`), before PR #35. PR #35 did not introduce it. The existing `reverseCompletionAndOwnershipSuppressOldCopy` test deliberately checks older completion suppression when the newer upload finishes first, plus user clipboard changes. It does not distinguish FIFO completion before the newer operation produces a result. The added probes cover that gap without changing the contract.

Related issue #28 requests concurrent uploads, ordered batch links and protection against an older result overwriting a newer result. Clipboard policy changes must account for out-of-order completion and future concurrency, not assume FIFO forever.

Activity already shows `copyStatus` and offers Copy result (`src/ui/RecentOperations.cpp:72,76`), but the suppression reason is not separately recorded here.

## Recommended next work

First choose the queued-result policy. Removing the invocation comparison would reverse KTD12 and expose late older results overwriting newer sharing results. Removing the clipboard revision check would also overwrite user changes. Neither is an appropriate isolated fix.

A possible revised policy is to treat overlapping accepted operations as a clipboard result group: retain confirmed links in invocation/item order and auto-copy the complete ordered list once that group settles, provided no user clipboard change or explicit copy superseded it. This would need an explicit update to KTD12, group state in `OperationService`, and review of adapter deferred-copy invalidation. It also needs bounded group lifetime, clear handling of failed/cancelled operations and private/local-only results, and deterministic behavior as new actions arrive during completion. This is a proposal, not an approved implementation.

An alternative that preserves KTD12 is clearer suppression feedback and an explicit ordered Copy completed results action. That would retain the present automatic policy while making earlier results easier to retrieve.

Use `tests/unit/test_operations.cpp` as the core test home. Extend the observed FIFO two/three-operation cases into regression assertions for the chosen policy; preserve the reverse-completion, user-change and deferred-native-commit protections. Include user changes between completions, explicit history/result copies while work is pending, later cancellation/failure, mixed batch outcomes, and invocation/item ordering. Grouped incremental copying would also need to distinguish app writes from user changes; copying once at group settlement avoids intermediate app writes invalidating subsequent members.

## Validation and limits

`cmake --preset dev` and `cmake --build --preset dev --target test_operations` passed. The targeted characterization and existing protection checks passed (11 QtTest results, including setup/cleanup). `ctest --preset dev -R '^operations_contract$'` passed in 45.63 seconds; this runs the complete operation test executable. `git diff --check` passed. The full multi-target development suite was not built or run for this diagnosis-only change. Local raw receipts, binary/source SHA256 identities and the failing probe patch are retained under `/home/patrick/work/put.gd/work/i32/`.

Confidence is high for the shared-service cause and the tested FIFO/batch scope. Actual desktop clipboard behavior remains unverified. At the diagnosis handoff, tests and this report were local, uncommitted investigation changes on the PR-head-based branch; no fix had been applied or published.

The development configure reuses the existing pinned AWS SDK 1.11.907 installation and exact source via build-directory symlinks. Initial configuration lacked the source LICENSE path; linking the existing provisioned SDK source corrected it. A probe initially referenced a nonexistent artifact field; the corrected batch probe validates artifact SHA256 and original-file identity. Failed setup attempts and corrected commands are retained in `setup-attempts.json`.

Socket preflight passed for the short private runtime path (44 UTF-8 bytes including `putgd/session.sock`). These GUI-less operation probes open no session socket and use synthetic text/files, not capture helpers or real clipboard contents.

No product candidate, package, live cloud upload, memory measurement or native graphical acceptance was performed. Cinnamon-first functional/memory gates and subsequent GNOME/KDE gates remain required for any eventual accepted product change. The original checkout's unrelated modifications remain untouched.
