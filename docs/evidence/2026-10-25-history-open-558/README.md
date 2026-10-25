# History-open memory investigation (#558)

Status: implementation and k72 development validation complete; minimum-Qt
validation is in progress. Physical Cinnamon's
strict 60 MiB History-open gate and the parallel Cinnamon/GNOME/KDE functional
and memory acceptance remain pending. No stable asset is accepted and #558
remains open. The unchanged physical memory protocol is 300/30/300 seconds,
with every resident/helper process counted and a continuously owned History
window during the open interval.

## Change and scope

Opening History previously constructed all Settings and destination controls,
even while hidden. The candidate constructs them on first navigation or agent
command. Existing instances remain alive across navigation. Dirty, conflicted,
blocked, provisional and alternate-provider drafts, plus accepted setup jobs,
still restore immediately. Clean saved state remains recoverable; its old
widget-derived `captureRouting` projection is omitted until Settings is actually
constructed and reconciled with current resident configuration. No theme selection/tokens,
allocator policy, process exclusion or memory ceiling was changed.

Dynamic Settings popup containers receive the same shared redesign refresh as
the prior eager tree, preserving popup sizing and the theme. CLI/MCP Settings
commands initialize the same production controls on demand.

## Historical failure

Issue #558 and companion evidence #553 record the exact published nightly 1185
Ubuntu 24.04 DEB (source `9dfac15c1ced201d13a2b70a8afb0c253213768b`,
SHA-256 `2094fd4db8cd915b02f1cdd16eb5c99bb8191948931cd2f558a89c59be5b3f47`)
at 64,956,416 B family PSS on physical Mint 22.3/Cinnamon 6.6.9/X11.
The limit is 62,914,560 B. Its cold and post-close gates passed. This record
establishes a budget failure, not a leak or regression attribution.

## Controlled diagnostics

Work ran on k72 using distro Qt 6.10.2, C++20, the official pinned AWS SDK and
the development CMake presets. Private Xvfb and D-Bus sessions were isolated
from the physical Plasma desktop; these results do not certify a compositor.
Only synthetic local configuration was used: no destinations, uploads, captures,
credentials, accessibility override or external update request.

A widget-construction probe separated the resident-service fixture, History,
Settings and the combined main window. Current trunk's main window used
22,640,832 B live heap after construction; Settings alone was the dominant
component. Production process-family samples, exact executable identities and
`smaps` then separated anonymous/heap allocations from mapped libraries.
The final comparison loaded the same library sets in both processes, including
the desktop platform-theme plugin. The dominant reduction was the UI heap.

| Clean source | Peak family PSS | UI heap PSS (separate post-sample map) |
| --- | ---: | ---: |
| Trunk `80ffa2e6c0d34223d2cc0f31254f2e60ac2bd247` | 70,081,536 B | 26,804,224 B |
| Candidate `48fd6047b2a8d7830296bc3de78f370719a1574f` | 51,537,920 B | 9,121,792 B |

The family PSS reduction is **18,543,616 B (17.685 MiB)**. Both clean runs used
five samples spaced one second apart after a three-second diagnostic warmup;
both retained two family processes, no escaped descendants, normal resident exit
(code 0), and verified socket removal. This is not the scored 300/30/300 protocol.
The exact versions, binary hashes, process starts, raw metrics and raw-record
checksum are in [summary.json](summary.json). Production binaries were unchanged
by the observer. Separate `smaps` reads occurred after the diagnostic interval.

[Code PR #574](https://github.com/patrick-hudson/put.gd-desktop/pull/574) contains
only the implementation and tests. This documentation merges after that code.

## Headless validation

The clean candidate passes the four focused application lifecycle, Settings,
History and theme suites on k72. The new absence-of-hidden-Settings regressions
fail against unchanged trunk at the intended construction assertion. Coverage
includes clean restoration and live configuration changes before first Settings
activation, dirty/provisional state and accepted setup inspection, direct agent
commands, close without constructing Settings, and existing popup geometry.

- [Focused suite output](focused-tests.txt)
- [Remaining development suite output](remaining-tests.txt)
- [Expected regression failures against trunk](regression-before.txt)

The remaining 103 development tests also passed (368.72 seconds). Together the
two separate commands cover all 107 configured development tests; this was not
a single full-suite invocation. The coordinator confirmed required CI success
for the exact candidate: contracts, Linux 24/26 and portable passed; ASan/UBSan,
packages, PipeWire and prune were skipped by selection, not sanitizer passes.

The Ubuntu 24.04 Release build
uses distro Qt 6.4.2/GCC 13.3 and the verified official AWS SDK 1.11.907. It is
paused in its owned f44 container for another task's coordinated recording slot;
no package or Qt 6.4.2 runtime result is yet claimed.

## Preserved attempts

- The first k72 configure lacked the pinned SDK license source path; it was
  corrected using the existing verified SDK/source, with the failed log kept.
- An early widget probe used the existing i559 build and is diagnostic only.
- Xvfb run `i558b1` queried the wrong diagnostics endpoint for helper readiness;
  it failed and was cleaned up. The observer was corrected to `ui.snapshot`.
- `i558b2` and the first candidate comparison are not final acceptance. Untracked
  configure/build logs had marked the first baseline dirty; logs moved under
  ignored evidence storage and the baseline was rebuilt clean as `i558b3`.
- Candidate `8fdd9013` showed a useful memory delta but failed lifecycle tests:
  a non-const JSON lookup inserted an absent Settings checkpoint. `bc3a0f50`
  corrected that lookup and updated Settings-specific test setup.
- The next tests exposed a clipped destination popup after deferred creation.
  `48fd6047` reapplies the existing shared theme refresh on first construction.
- The prepared Ubuntu 24.04 image lacked xcb-xtest and Xvfb test packages. Only
  a task-owned container/image was updated after dependency simulations;
  shared images, retained containers and host desktop packages were preserved.

The raw failed and successful attempts remain private on the builders and in
this task's evidence export. No failed attempt is relabeled as a passing gate.

## Pending acceptance

The coordinator must allocate the physical slot after the final Qt 6.4.2 build
and tests are ready. Save credential-safe visuals, exact installed/live source
and package identities, normal helper exit and complete cleanup receipts.
No encoding or extra diagnostic probes belong inside scored intervals. Final
parallel Cinnamon, GNOME and KDE functional/memory checks remain separate from
these controlled diagnostics; #384 still requires eventual exact stable assets.
