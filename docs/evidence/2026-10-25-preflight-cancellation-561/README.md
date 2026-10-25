# Destination preflight cancellation (#561)

Code [PR #564](https://github.com/patrick-hudson/put.gd-desktop/pull/564), based on
`80250d2559606e7fa739e789dcffb03c96f9c53b`. These are targeted development-build
regressions, not release-asset acceptance. No release was tagged or published.

## Change and regression contract

[Issue #561](https://github.com/patrick-hudson/put.gd-desktop/issues/561) records
physical m74, nightly `v26.1024.1185-nightly`, source `9dfac15c`, taking
**115,836 ms** to cancel a stalled destination check with a **120,000 ms** request
timeout. That is the issue's baseline, not a new baseline run by this thread.

The operation cancellation flag now reaches `checkName` in initial preflight and
post-collision rename checking. S3 forwards it through queue admission, process
supervision, worker dispatch and the SDK HEAD continuation callback. The existing
Curl progress hook observes cancellation while HEAD waits for headers. Proton
and native B2 use their existing helper cancellation paths. The private S3 module
contract changes from 1 to 2 with the virtual interface; packaging and its audit
require version 2. AWS headers remain under `src/storage/s3`.

The regression waits for an actual fixture HEAD, with a 30-second server delay
and 120-second request deadline. Terminal cancellation **and release of the active
operation slot** must complete within five seconds, without PUT or clipboard
copy. Explicit retry must upload the same retained candidate with exactly one
PUT. Further tests cover already-cancelled S3, stalled B2 lookup/retry, Proton
helper cancellation and History's “Checking the destination…” wording. #559's
queued/editor precedence clauses remain separate.

## Tested source and integration verification

Desktop-tested source `8c4923d06b9591dde7d39049fccd4fb50a2a8725`, version
`26.1025.1260-dev+g8c4923d06b95`. The final commit changes only tests and the package
auditor; runtime source is identical to `79fd0cb5`. Evidence remains tied to its
actual build, including refreshed head builds for the desktop matrix.

Integration head `951742b967f94cb6ffcebfe24489776fac1f52d6` merges nightly
`4188212c` after #559 landed. The only conflict was adjacent History test
insertions; both the queued-status tests and the preflight wording test were
retained unchanged. The #561 runtime changes are unchanged; the merged runtime
also includes #559's independently tested queued-editor handoff. Fresh CI is
required on this integration head. The desktop results below remain evidence
for their exact `8c4923d0` binaries, not a new desktop run of the integration head.

[Head measurements](head8c/measurements.json) retain binary hashes, build
manifests, actual session/QPA witnesses, operation IDs, retry outcomes, privacy
gates and inspected visuals. The targeted workload uses a synthetic PNG through
the production editor's explicit Submit, an isolated loopback S3 destination,
30-second stalled HEAD, cancel, then retry. It is not a screen-capture test.

| Exact 8c4923d0 desktop | Cancellation | Result |
| --- | ---: | --- |
| Physical d69, Debian 13 / Plasma Wayland | 854.22 ms | Cancel, same-name upload retry and clipboard passed |
| Physical f44, Fedora 44 / GNOME Wayland | 935.97 ms | Cancel and upload retry passed; clipboard limitation below |
| Physical k72, Kubuntu 26.04 / Plasma Wayland | 643.78 ms | Cancel, upload retry and clipboard passed |
| Fresh d69 VM k561, Kubuntu 26.04.1 / Plasma X11 | 907.55 ms | Cancel, upload retry and clipboard passed |
| Physical m74, Mint 22.3 / Cinnamon X11, approved slow harness | 1,051.79 ms | Retry independently verified; fixed settle and cleanup gates passed |
| Fresh d69 VM xg561, Ubuntu 24.04.5 / GNOME 46 X11 | 624.68 ms | Default scale; cancel, upload retry and clipboard passed |
| Fresh d69 VM g561b, Ubuntu 26.04.1 / GNOME 50.1 Wayland | 202.16 ms | Cancel, upload retry and clipboard passed |

The 8c4923d0 stalled-HEAD regression measured **512 ms**, including active-slot
drain. All **108 CTest suites passed** (78 integration, 30 unit), in 939.02 seconds
on the shared d69 builder: [full log](head8c/ctest.log),
[socket preflight](head8c/test-socket-preflight.json). Final CI/review status is
reported by the linked PRs; no skipped-by-failure check counts as a pass.
Twenty-one native-package audit tests pass locally; the test-wait contract passes.

Fedora's uploaded retry reports `clipboard_unavailable` / “GNOME helper cannot
own CLIPBOARD”. The same failure occurred on all five completed uploads in
independent earlier source `9f382fd0b07d` from #559; the
[bounded comparison](final/preexisting-gnome-clipboard.json) retains those errors.
No Fedora clipboard acceptance is claimed. No GNOME extension was changed.

## Approved slow workload

Mint uses unchanged driver `00ab777f2413a1fbd7670a7259dc614eced896e6`, native witness
`72afd00bf2dbdaaa7e4bd3f847dc54f1da715f0e` (SHA256
`347934c226f99cb0e509d5466c5df8116896e6bfac07d75fdfa4f4c2d06ca053`), a 256 KiB/s
proxy, seeded 6,223,843-byte PNG, 120-second request deadline and **300-second
settle**. The final run `/tmp/p561s3` measured settled RSS 33,411,072 bytes and PSS
21,607,424 bytes, below the unchanged 40 MiB / 29 MiB gates. Cleanup passed.
The harness labels cancellation and settle-growth thresholds **unassessed**
(no approved threshold), and content-response timings **unassessed**
(content endpoint not witnessed). This is not an all-gates #379 performance pass.
[Exact slow records and visuals](head8c/mint-slow/measurements.json),
[package hashes](head8c/packages.json), [functional driver](scripts/accept.py),
[slow invocation](scripts/slow-8c.sh) and [cleanup receipts](head8c/cleanup.json)
retain reproducible parameters. Temporary installed packages were purged; m74's checked-clean release was
`2026-10-25T05:23:11Z`. No other desktop work or heavy build ran in its quiet slot.

## Earlier evidence and failures

[Intermediate evidence](intermediate/measurements.json) preserves source
`256714813840ff86d2db0a513591788208c3f1b8`: d69 cancel 716.49 ms and m74 slow
cancel 1,108.35 ms, with successful retries and clean releases. Source
`79fd0cb5ee9170580941b175b99d89c3921ed352` passed all 108 CTest suites in 485.81 s
(stalled HEAD 513 ms), native d69/f44/k72 and Ubuntu26 GNOME/Wayland checks;
[its records](final/measurements.json) and [Mint slow run](final/mint-slow/measurements.json)
retain exact identities. The latter measured 713.75 ms and passed fixed memory
and cleanup gates. These earlier results are not relabelled as head results.

Preserved unsuccessful attempts and their corrections:

- Initial configure/build attempts needed the provisioned official AWS SDK source,
  Fedora's `lib64` runtime path, and a public History API in the regression.
  An unused optional capture benchmark was disabled after missing `xcb-xtest`;
  the approved slow driver was unchanged.
- First full CTest: 107/108 passed; the new S3 fixture incorrectly added a profile
  field to the recording action. It now changes routed actions only.
- CI on `79fd0cb5`: Ubuntu24's cancellation passed in 506 ms but immediate retry
  raced the service completion callback; the test now includes active-slot drain
  in the same five-second bound. Package audits still required module contract 1;
  the final commit updates them to 2 and adds rejection of the old contract.
- d69's first app frame failed cover/edge checks; own-PID KWin placement and a
  certified crop corrected it. Fedora's full-output precheck fails at 95.93%
  because of a persistent top overlay; the certified `[0,180,1920,900]` crop passes
  98% admission, with unchanged 50% cover and 90% every-edge saved-frame gates.
- First KDE/X11 VM frame failed (46.5% cover, bottom edge 0). Its supported
  1920×1080 mode, default app scale, own-PID geometry and `[0,0,1920,1000]` crop
  passed unchanged gates. The 1280×800 failure is retained in its private archive.
- GNOME/X11's first frame failed edge checks. A second run's CLI checks passed
  but saved visuals showed incomplete paint at scale 0.7; it is not visual proof.
  Default-scale retry with own-PID geometry passed; the three saved state visuals
  were inspected and show complete painting.
- VM cleanup initially refused a copied driver's noncanonical run root and later
  a self-included changing preview checksum. The canonical repository cleanup
  command and a manifest excluding its generated receipts passed the guards;
  no guard was weakened and no overlay was removed manually.
- A redundant packaging clone lacked its Git alternate inside the container;
  corrected read-only mounts restored provenance. That redundant build was
  stopped to reduce contention; no output from it is accepted.

Private full evidence is retained under this thread's `work/i561/evidence` and
host `~/work/i561` or `~/work/i561u` run/build directories. Failed privacy frames
were deleted unopened by the existing gate; failure metadata remains. Every
isolated runtime had a short-path preflight before application startup. All
physical GUI runs used actual leases and checked-clean release receipts. VM
archives are exported and hash-verified before guarded overlay retirement;
immutable goldens remain untouched.

All four physical leases were released checked-clean. The refreshed disposable
VM overlays were retired through guarded cleanup: k561 at 05:21:59Z, xg561 at
05:25:34Z and g561b at 05:25:36Z. Their owned tunnels and wall tiles were removed;
other sessions and the immutable goldens were preserved.
