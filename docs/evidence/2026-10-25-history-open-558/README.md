# History-open memory investigation (#558)

Status: the local development candidate passes the unchanged physical Cinnamon
300/30/300 memory protocol and separate targeted native UI checks. Parallel
Cinnamon/GNOME/KDE functional and memory acceptance, response-time gates and
published asset acceptance remain pending. No stable asset is accepted and #558
remains open. Every resident/helper process was counted; History had a guarded,
continuously owned native window throughout its open interval.

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
- [Qt 6.4.2 Release focused suite output](qt642-focused-tests.txt)
- [Expected regression failures against trunk](regression-before.txt)

The remaining 103 development tests also passed (368.72 seconds). Together the
two separate commands cover all 107 configured development tests; this was not
a single full-suite invocation. The coordinator confirmed required CI success
for the exact candidate: contracts, Linux 24/26 and portable passed; ASan/UBSan,
packages, PipeWire and prune were skipped by selection, not sanitizer passes.

The Ubuntu 24.04 Release build uses distro Qt 6.4.2/GCC 13.3 and the verified
official AWS SDK 1.11.907. All four focused suites pass (174.33 seconds) in
the owned f44 rootless container, after the coordinator verified the other
task's clean desktop release. These are headless tests. The development DEB
was then packaged and verified: all six executables have the same clean source
identity, use distro Qt dynamically, and no Qt libraries are bundled. Its
SHA-256 is `9c8ff3711ae2b55bd75e44c92ab7496d3e88d5b23a480d7e83d071819817084d`
(size 10,993,726 B); [package and binary identities](qt642-package-identity.json)
retain exact metadata. This is a locally built development package, not an
accepted published asset. The first packaging attempt correctly refused an
unbuilt configured KDE region helper; building that target resolved the refusal.

## Physical Mint candidate — 2026-10-25

Physical m74, Mint 22.3, Cinnamon 6.6.9/Muffin 6.6.3, native X11 session `c89`,
distro Qt 6.4.2 ran the exact development DEB above. The unchanged
[#555 driver](https://github.com/patrick-hudson/put.gd-desktop/pull/555) at
`00ab777f2413a1fbd7670a7259dc614eced896e6` used a new private `/tmp/p558m1`;
the resolved session socket was 38 bytes. Admission recorded a quiet host,
inactive stress service and the preserved pre-existing VM. No added probes or
encoding ran inside scored intervals; required visuals were saved outside them.

| Gate | Measured duration | Peak family RSS | Peak family PSS | Resident one-core CPU |
| --- | ---: | ---: | ---: | ---: |
| Cold closed | 300.049 s | 30,117,888 B | 19,172,352 B | 0.006666% |
| History open | 30.070 s | not an RSS gate | **46,641,152 B** | not a CPU gate |
| Post-close | 300.137 s | 30,281,728 B | 19,314,688 B | 0.003332% |

All scored memory/CPU gates passed. History stayed below **62,914,560 B**;
closed RSS/PSS stayed below 40/29 MiB and CPU below 0.5% of one core. The
History guard performed 299 checks with exact PID/start/parent identity and
queued X11 unmap/destroy events. Its helper exited normally with code 0.
Both resident cleanup receipts show normal exit 0, no forced termination,
no remaining owned processes and removed sockets. This demonstrates the local
candidate's budget result; comparison to older nightly 1185 is not a matched
build experiment or a regression/leak attribution.

A separate unscored `/tmp/p558f1` run passed fresh History, external configuration
before first Settings activation, native Destinations navigation, live light/dark
appearance, retained navigation, clean close/reopen, omission of stale derived
state and direct agent initialization from History. The intentionally absent
profile returns the expected `profile_not_found` response. The Settings visual
shows the externally changed local-copy path. Both functional helper generations
closed normally; resident/socket/fixture cleanup passed. Dirty/provisional/setup
lifecycle coverage remains in the automated suites, not a new native claim.

- [Exact native results, gate limits, lifetime and cleanup receipts](native-summary.json)
- [Verified-clean desktop release at 17:51:30 UTC](2026-10-25T17-51-30Z-release.json)
- [Visual hashes and source identities](visual-identities.json)
- Reviewed owned-window visuals: [History](history-open.png),
  [Destinations](destinations-light.png), [Settings light](settings-light.png),
  [Settings dark](settings-dark.png).

Only the task DEB and temporary GTK introspection package were installed and
purged; no desktop packages were upgraded or unrelated automatic dependencies
removed. The pre-existing VM generation and shared SSH processes were preserved.
The raw archive SHA-256 is
`1daa3de9e1b1d2c818466fd4a3744be6d61a44d9b5196aa629e13adf909683e9`;
original raw artifacts remain private and directly usable.

## Preserved attempts

- The first k72 configure lacked the pinned SDK license source path; it was
  corrected using the existing verified SDK/source, with the failed log kept.
- An early widget probe used the existing i559 build and is diagnostic only.
- Xvfb run `i558b1` queried the wrong diagnostics endpoint for helper readiness;
  it failed and its resident was terminated. A later audit found no owned
  workload processes and a non-listening stale socket, retained with the failed
  attempt. The observer was corrected to `ui.snapshot`; successful repeats
  require normal exit and socket removal.
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

The frozen driver explicitly leaves `historyPaintResponse` **unassessed**.
Saved functional visuals are not a paint-response timing result. Final parallel
Cinnamon, GNOME and KDE functional/memory checks on the exact candidate remain
coordinator-owned gates, along with the other declared distribution/session rows
and eventual exact published assets. This targeted local development-package
pass does not close #558 or certify a stable release.
