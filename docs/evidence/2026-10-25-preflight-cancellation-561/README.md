# Destination preflight cancellation (#561)

Code: [PR #564](https://github.com/patrick-hudson/put.gd-desktop/pull/564), based on
`80250d25`. This is targeted regression work on development artifacts, not release
acceptance. No release tag, promotion or publication is part of this work.

## Reported baseline

[Issue #561](https://github.com/patrick-hudson/put.gd-desktop/issues/561) records
physical m74, published `v26.1024.1185-nightly`, source `9dfac15c`, and the #379
`slow2` scenario driven by `00ab777f`: terminal cancellation took **115,836 ms**
against a **120,000 ms** request timeout. Retry succeeded after restoring the proxy.
History/activity misleadingly said “Preparing capture…”. This is the issue's
baseline evidence, not a new baseline measurement by this fix thread.

## Change and regression contract

The operation's cancellation flag reaches `checkName` in both initial preflight
and post-collision rename checking. S3 forwards it through queue admission,
process supervision, worker dispatch and the SDK HEAD continuation callback.
The existing SDK runtime's Curl progress hook observes cancellation even while
HEAD waits for headers. Proton and native B2 forward it to their existing helper
cancellation paths. The private S3 module ABI version changes with the virtual
interface; no AWS headers enter provider-neutral code.

The stalled-HEAD regression waits for a real fixture HEAD to arrive, using a
30-second fixture delay and 120-second request deadline. It requires terminal
operation cancellation in under 5 seconds, no PUT, no clipboard copy, and a
successful explicit retry of the same retained name with exactly one PUT.
Additional regressions cover an already-cancelled S3 call, Proton helper
cancellation, stalled B2 lookup/retry and the History preflight label. #559 owns
queued-editor semantics; this change adds only “Checking the destination…”.

## Execution status

Remote validation and graphical acceptance are in progress. The code head is
`b46eba70315aec15a03331b3fdf811999eb7f141`. The main thread owns no desktop lock
while preparing builds. d69 and f44 were handed off after #491's checked-clean
release; m74 remains with the #379 performance campaign until its clean release.

- d69: native Debug build with GCC/distro Qt and pinned AWS SDK 1.11.907,
  using `cmake --preset dev` and `cmake --build --preset dev`, `nice -n 10`, `-j2`.
- f44: separate Ubuntu 24.04 container, Release development DEB build, distro
  Qt 6.4.2, pinned SDK, `nice -n 10`, `-j1`. No host desktop packages changed.
- First d69 configure failed because the SDK source/license path was missing;
  the retry points at the already-provisioned official SDK source.
- First Ubuntu configure requested the optional capture benchmark and found
  missing `xcb-xtest`. That unused option was disabled for the slow-network
  workload; no benchmark source, thresholds or performance harness were changed.

Final test receipts, exact build identities, visuals, cleanup and remaining
acceptance gates will replace this in-progress section before handoff. No pending
or historical desktop result is counted as a pass for this source.
