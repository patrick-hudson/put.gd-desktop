# Settings idle CPU — issue #840

Code: [PR #877](https://github.com/patrick-hudson/put.gd-desktop/pull/877).
This is a boat development investigation and regression comparison, not physical
release, GPU, A/V, latency or memory acceptance. No release was published.

| Identity | Commit |
| --- | --- |
| Baseline nightly | `5e1d592eadd9898c5a20477afa94999ad1d69eae` |
| Accepted code head | `be8bc6b13f28afd03c835211f98f608909b84b6f` |
| Accepted source tree | `17352781d921c64e2ee9a28ee92ef371225430a0` |
| Generated development version | `26.1030.2615-dev+gbe8bc6b13f28` |

Both measured final packages report that exact clean source identity. The two
ABI-native builds use dynamically linked distro Qt: Ubuntu 26.04 / Qt 6.10.2
(RelWithDebInfo), and the Noble-compatible Mint package / Qt 6.4.2 (dev preset,
symbols). The official AWS C++ SDK is 1.11.907; no substitute toolchain was used.

## Result

| Desktop / platform | Before, 60 s | Final head, 60 s |
| --- | ---: | ---: |
| Ubuntu 26.04, GNOME Shell 50.1, native Wayland | 101.682% | 0 ticks; <0.017% |
| Mint 22.3, Cinnamon 6.6.4, native X11/xcb | 94.314% | 0 ticks; <0.017% |

CPU is the UI process's `/proc/PID/stat` user+system tick delta, divided by
`_SC_CLK_TCK=100` and measured monotonic elapsed time. 100% is one core. Each
sample follows three seconds of startup/transition warmup. Both native runs
register Qt accessibility, with no test AT-SPI observer. The offscreen and
Xvfb/xcb reproductions leave forced accessibility unset: Settings consumes a core
on nightly while History idles. On the final head, both views record zero ticks
in both headless platforms too (two five-second samples per view/platform).

The common [CPU driver](reproduce/profile.py) creates fresh private XDG homes and
checks resolved Unix socket byte lengths before starting the app. The
[guest driver](reproduce/measure-guest.py) verifies DEB SHA-256 before installation.
No S3 credentials, Proton session, uploads or user objects were used. Appearance
is `system`; the fresh final Cinnamon session renders dark. The measured
processes, binaries, elapsed times, host/run IDs and package hashes are in
[results.json](results.json), the individual receipts and [lab identities](lab-identities.json).

[GNOME before](gnome-before.png) · [GNOME final](gnome-after.png) ·
[Cinnamon before](cinnamon-before.png) · [Cinnamon final](cinnamon-after.png).
Final images are lossless PNG exports of saved QMP frames captured five seconds
after `ui.show`, inside each timed run. Their frame hashes, timestamps and UI
PIDs are retained; the PIDs match the CPU receipts. Both guests stayed visible on
the localhost VNC wall. Preflight proves unlocked Wayland/X11 sessions; the
GNOME lab witness was staged, rebooted and verified ACTIVE in its fresh overlay.

## Cause and fix

The preference family's zero-delay sizing callback observes `LayoutRequest`.
Its layout setters emit another layout request, continuously scheduling the next
pass. `perf` identifies that callback on the hot UI stack, independently of
AT-SPI. See the [profile](profiling/perf-report.display.txt).

The fix sizes from model, shortcut-text, font, show and resize inputs, rather
than its own layout output. Newly shown help gets a bounded inner-layout refresh.
Recording prompt changes have an explicit guarded callback, including when all
shortcut texts are empty. Live status and shortcut readiness keep their existing
paths. No periodic timer or intended visible UI change is introduced. No catalog
reference images changed.

## Verification

- All eight idle cases (four Settings pages × two sizes, including retained
  hidden widgets) fail on nightly with the new test applied. The final build
  records zero idle layout and paint events; it waits for a quiet interval and
  uses the repository steady-state helper, with no fixed-sleep assertion.
- Repository help is shown/hidden twice and checked for overlap, containment and
  enough height for its wrapped text.
- The isolated empty-shortcut recording test holds surrounding geometry steady.
  It fails on `818bfc95` and passes on the final head, including width restoration
  after cancel. This addresses the CodeRabbit finding; its thread is resolved.
- Full registered CTest: **114/114 pass, retries disabled**, 160.22 seconds.
  [Ledger](tests/snapshot-flakes.json), [source](tests/source.json),
  [focused excerpt](tests/focused-excerpt.txt). Original JUnit, full test log and
  command/build log are losslessly compressed; [index](tests/compressed-index.json)
  gives original and gzip hashes. Restore with `gzip -dc FILE.gz > FILE`.
  [Text export index](text-exports.json) distinguishes whitespace-normalized display
  copies from their byte-exact compressed originals.
- Current-head CI passes contracts, Ubuntu 24.04, Ubuntu 26.04 and Windows, plus
  required `CI`; the Ubuntu 26.04 image comparisons pass in both schemes.
  Path-unselected jobs are recorded as skipped, not counted as passes. See
  [CI receipt](ci-current-full.json). CodeRabbit completed; this is not a human
  approval or permission to merge.

## Failure context and retention

Early redundant-setter guards stopped CPU but exposed stale Qt layout-cache
geometry in three catalog states. Those approaches were rejected. The final
input-driven fix preserves the references. The first CTest setup lacked CI-user
home traversal ACLs and ran no tests; corrected commands retain the failure.
The first Cinnamon install began before its package transfer completed; SHA
verification and a fresh overlay corrected it. That failed overlay was exported,
verified and retired through the guard. A witness status query was misquoted
after a successful reboot; the corrected helper verified ACTIVE before sampling.
An RFB grab after the intermediate run's planned quit is retained as a late grab;
the valid in-run QMP frame was exported instead. No failure was relabelled a pass.

Raw rejected-candidate catalogs, perf data, source/command copies and intermediate
results remain in the private work archive, outside release source exports.
[Retention index](retention.json) records archive/member verification and restore
instructions. No disks, SSH private keys, login inputs, sessions or payloads are
in this documentation packet.

## Cleanup

Each completed overlay was stopped, exported and checksum-verified before the
guarded cleanup command retired its disk. Both original and review-run receipts,
and the failed Cinnamon receipt, are retained under [cleanup](cleanup/).
The sealed backing images and named snapshots were preserved. Three sandbox forks were created in total, exceeding the task's two-sandbox
limit. At most two were active at once; the build fork was deleted before
Cinnamon was forked. All three forks were deleted after export, and only this
task's wall entries, tunnels and SSH agent were removed.
The worktree was detached for handoff. No PR was merged and issue #840 stays open.
