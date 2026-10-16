# Lift and duotone implementation validation — 2026-10-16

The implementation is on `codex/lift-duotone-icons`, cut from main
`0ff7639d654cac6f55f519848eef937dbaeee4b6`. The clean native candidate is
`75c56ca783367681b8af561e435006bc26fd07de`, tree
`a4ade5a12897cb161351ccc7fa263cd688dc7a83`.

## Implemented behavior

Lift identifies the tray, taskbar/window and packaged Linux launcher. Existing
in-app symbols use the duotone family. Upload fills use observed transfer bytes
and a provider total when available; otherwise the acquired source size supplies
an explicitly labelled estimate capped at 99% until finalization/receipt.
Preflight or storage work without a usable denominator stays dim green. A
confirmed successful upload briefly shows full green before idle blue. Error
pulses retain their red/blue timing and priority over progress. Concurrent active
transfers are weighted by source size; retries can reset their fill.

## Development verification

`cmake --preset dev` and `cmake --build --preset dev` succeeded with distro Qt
6.4.2 and the pinned official AWS C++ SDK 1.11.907. The Release preset also built
successfully and produced the native lab DEB with `PUTGD_LAB_FIXTURE=ON`.

- Full CTest run `/tmp/li47t3`: 58/59 groups passed. The new unequal-size
  concurrency fixture used a shared target name, serialized its second request
  and stalled during failure cleanup. Its journal and termination context remain
  preserved. Generated names and a draining completion guard corrected the fixture.
- Focused `ctest --preset dev -R '^gui_tray$'` rerun `/tmp/li47t4`: passed,
  including all 25 Qt cases, in 48.83 seconds. Product code was unchanged by
  this test-only correction. The combined verification covers all 59 groups;
  there was no additional complete rerun after correcting the fixture.
- `python3 -B tests/desktop/test_tray_progress_lab.py`: 25/25 passed.
- Native StatusNotifier wire contracts passed within the full run, covering
  actual ARGB bytes, percentage changes, duplicate suppression, host re-registration
  and error recovery without a QGuiApplication. Its watcher is synthetic.
- Synthetic production-renderer previews at scale 1 and 2 were inspected,
  including 16/22/24/32-pixel tray sizes on dark/light backgrounds. These are
  offscreen evidence; native high-DPI/panel acceptance remains pending.

The earlier `/tmp/li47t1` red/green tests, `/tmp/li47t2` full-suite tray failures
and replay-validator red tests remain preserved. Initial 100% clipping and an old
strict-blue completion expectation were corrected before the final-product-code run.

## Native Cinnamon/X11

The final candidate was installed with the guarded lab installer into disposable
run `i47c1`, UUID `df0bfedf-ebcf-4672-b2e1-35102ce951a8`, with KVM, two vCPUs,
6 GiB guest memory and visible VNC listener `127.0.0.1:5963`. Its immutable base
was `p2c-provisioned-a1.qcow2`. Cinnamon was 6.6.4 and Muffin 6.6.1,
with distro Qt 6.4.2 and kernel 6.14.0-37-generic.

All 12 local-only cases passed: region/window/desktop capture, Qt clipboard
text/image/files, three region-cancellation routes and GTK clipboard
text/image/files. Independent native paste and saved-byte checks passed. Two
visible cycles reopened Settings and the shared History/Activity view through
six production view-route calls with the same resident identity. Codex inspected
the native screenshots: Lift appears in the taskbar and tray; duotone controls
and fallback file glyphs are readable. The genuine Cinnamon StatusNotifier host
also returned the expected idle icon images.

The 120-second post-use process-family observation completed with 25 samples,
one resident and no UI/escaped descendants. Peak/final RSS was 31.26 MiB, PSS
21.07 MiB and private memory 20.14 MiB. The process identity remained unchanged.
This is a VM diagnostic, separate from physical cold-idle acceptance.

The separate physical Cinnamon/X11 cold-idle observer passed the unchanged
300-second legacy budget: peak sampled RSS 25.39 MiB, final RSS 25.37 MiB
and CPU 0.003332% of one core over 300.16 seconds. It recorded 293 owner-window
polls, zero operations and no owned activation observed; the observer marked
the workload uninterrupted. No builds or graphical test actions overlapped it.
The legacy budget is 100 MiB RSS and 0.5% CPU. Remaining desktop acceptance
gates are listed below.

The final source archive SHA256 is
`23256d41c9046a4df842732ff92d162691e42c3259fd962649262e0acc90abc1`.
Native DEB SHA256: `a7e66be90d847218e6a198407bc9d19b251ea1f9a7cfaaf7f3b0e64016e4877f`.
Installed/running app SHA256: `31311f95130bc0dac55e05b0a864320ae8263bf112df6bba6f5123936aa58b2a`.
The same application bytes were used for the physical observer. CLI and remaining
binary hashes are retained in the private candidate receipt. The background resident PID was 9260, start ticks 170098.

All isolated app sockets were checked before launch: native session 33 bytes,
physical idle 38 bytes and the tested temporary-path upper bounds at most 70
UTF-8 bytes, below Linux's 107-byte limit. The QMP path was 54 bytes.

The disposable overlay was stopped and retired only after evidence export and
hash verification. The first cleanup call from the attached worktree refused
the canonical checkout's run path; the identical tool was then invoked from the
owning canonical checkout. Both attempts and the successful guarded retirement
receipt remain preserved. No golden disk was changed.

## Review and remaining acceptance

`ce-code-review` run `lift-review-20261016` completed with six local reviewers and
an independent Anthropic peer pass. Its validated preflight full-green finding
was applied by the caller and verified by the corrected tray tests. The caller
also guarded asynchronous completion feedback during an active error pulse.
There are no justified unresolved code findings from that review.

Current-source GNOME/KDE graphical and memory checks, live S3/Proton tray-progress
replays, native host-restart recovery and native high-DPI/mixed-display checks
remain pending. This record does not establish full desktop/provider acceptance;
the pull request remains a draft. No cloud transfers or credential provisioning
were performed by this local-only campaign.

The private evidence root is `work/i47/` in the canonical checkout. It retains
source/archive identities, review and resolution records, all failed test logs,
package receipts, native scenario identities, rendered PNGs, memory samples and
cleanup receipts. All 24 scenario evidence directories from the initial and final
Cinnamon waves were hash-verified; their host-side files remain under
`work/lab-runs/i47c1/tests/`. These private archives are outside release exports.
