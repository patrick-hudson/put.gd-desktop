# Boat candidate #752: F1 and live concurrency

Date: 2026-10-30 UTC. Code [#766](https://github.com/patrick-hudson/put.gd-desktop/pull/766),
issue [#752](https://github.com/patrick-hudson/put.gd-desktop/issues/752).
This packet supplements the earlier [#753/#754 packet](../2026-10-30-boat-candidate-752/README.md).
The coordinator owns validation and issue closure. No merge or issue closure was performed.

## Exact identities and scope

Both complete runs and the interrupted run requested nightly commit
`0c329b0692cadc52f03f91e59c06c5eda760bf04`, fetched before the campaign.
The original controllers were `a8dab553490975ad369bae5bf51879b4912682fb` (SHA-256 in
[concurrent.json](concurrent.json)); code PR head `1b9e5a5d9d8d6b211a001887874398291df0d6c8`
adds only stricter assertions to existing no-ID tests. It does not change the
controller or recipes used by the live runs. The manifests and target receipts
retain the candidate commit, named-snapshot identity, pinned container image,
placement, TTL and deletion operations. Successfully built targets also bind
build/binary identities and per-file inventories. Generated recipes and raw
artifacts remain private outside Git at
`/home/patrick/work/boat-candidates/issue752-f1-live-20261030/`.

Controllers, Git, metadata inspection and lightweight Python tests ran on the
workstation. All dependency installation, Docker builds and catalog rendering
ran on Boat `large` sandboxes cloned from `putgd-build-base`, with `--no-env` and
10,800-second TTLs. No S3 credentials were sent to Boat. No package installation,
build, container or VM ran on the workstation. No desktop or release acceptance is claimed.
Named snapshots and unrelated workers were preserved.

## F1 regression

Both [new regressions fail on nightly](f1-before.txt): a create becoming visible
only after the bounded observation window previously left
`cleanupVerified: true`; a truthy `"qualified"` value previously allowed a passing
manifest. [All 48 controller tests pass](controller-tests.txt) with F1.
The fix records `cleanupVerified: "qualified"`, `lateCreateRuledOut: false`, and a
`cleanupQualification` reason whenever an attempted create never printed an ID,
even if name-based recovery found a worker. `cleanup.verified` describes observed
absence in the bounded window. Manifest success requires literal
`cleanupVerified: true`, and unresolved names remain in
`ownedSandboxesRemaining`. TTL bounds compute; a later sweep or `--cleanup`
reconciles a worker appearing beyond the observation window.

## Two concurrent full controllers

[Controller launch identities and argv](concurrent.json),
[run A manifest](run-a/manifest.json), [run B manifest](run-b/manifest.json),
[controller A stdout](run-a-controller.log),
[controller B stdout](run-b-controller.log), and the
[per-controller real CLI invocation audit](commands.jsonl) bind the ownership
proof. The audit wrapper logs argv then `exec`s the installed real Boat CLI;
it does not fake responses or insert delays. The
[supervisor](campaign-supervisor.py) records periodic real listing observations in
[observations.jsonl](observations.jsonl). The snapshot was only read/forked;
there is no snapshot-delete command.

| Run | Nonce | Original controller | Verified files |
| --- | --- | --- | ---: |
| run-a | `02818a97bfa24edb` | failed: 6/7 passed; Ubuntu 26.04 npm ECONNRESET | 483 |
| run-b | `67399a311ac84036` | passed 7/7 as-is | 490 |
| Ubuntu 26.04 recovery | run A catalog worker | separate recovery passed | 7 |

Run B passed all seven targets as-is. Run A passed six; Ubuntu 26.04 failed on
external npm `ECONNRESET`. Its original manifest remains failed and unchanged.
The independently collected recovery below supplies that target's package/identity
proof without relabeling the failed controller. Every original full-run target
has literal `cleanupVerified: true`. The seven-ID owned sets are disjoint. Each controller's audited deletion set
exactly equals its own owned set and excludes the other's. Run A's Ubuntu 26.04
startup sweep explicitly preserved run B's live `bx_yxz2d7mn` worker. The interrupted
run's startup sweeps also preserved the full runs' live peers.

[Independent verifier](verify.py) replays `sha256sum -c SHA256SUMS` against the
retained raw artifacts, checks the disjoint sets and audited delete ownership,
checks fixture metadata, and queries the real API for final absence.
[Acceptance results](acceptance.json), [A checksum replay](run-a-sha256-verify.txt),
[B checksum replay](run-b-sha256-verify.txt), [A sums](run-a/SHA256SUMS) and
[B sums](run-b/SHA256SUMS) retain the results. Build logs and package bytes are
retained in the private campaign directory, rather than copied into Git. To replay
artifact verification, run `python3 /home/patrick/work/boat-candidates/issue752-f1-live-20261030/verify.py`;
the checked-in script is the same verifier but expects those private sibling artifacts.

## Interrupted real create and deletion timing

The [interruption driver](interrupt.py) launched the third seven-target run and
sent SIGINT 0.010 seconds after the earliest audited `boat new` invocation.
At that instant all seven `creation.jsonl` files were empty, and five new-command
invocations had been audited. The other two were issued by the already-running
threads before they observed cancellation. [Signal-time evidence](interruption.json),
[final interrupted manifest](interrupted/manifest.json),
[controller output](interrupted-controller.log), and each target's retained
creation stream and receipt document the result. Exactly 21 `new` invocations
were used across the campaign; none were retried.

Five real `new` processes printed `created` IDs before cancellation took effect;
two (Debian 13 and Ubuntu 26.04) never printed an ID. The controller found all
seven accepted creates by its nonce names and requested their deletion. The
no-ID workers were first submitted for deletion 1.286 and 3.536 seconds after
SIGINT. This demonstrates accepted-but-unreported creates against the real API,
not a guarantee that every create emits its ID before a stall. The command was
interrupted while waiting for readiness; no artificial network delay or fake API
was used. A create delayed beyond the recovery window, or an unbounded hang, was
not observed and is not claimed as reproduced.

All seven real deletion replies were initially `status: pending`, `stage: removing`.
For the five ID-bearing workers, absence was verified 2.552–2.591 seconds after
the delete command was invoked. The two no-ID workers' final verification took
116.695 and 118.953 seconds after their delete invocations because the controller
continued observing the original 120-second create window. Those are recovery
observation durations, not measured disk-erasure latency. See
[all deletion timings](deletion-timings.json) and the supplemental
[interruption listing observations](interrupted-observations.jsonl).
A longer lingering-delete delay was not observed.

The interrupted manifest correctly fails: five cleanup fields are `true`, two
are `"qualified"` with `lateCreateRuledOut: false`. All seven have observed-absence
receipts. The two unresolved names deliberately remain in the manifest even
though their recovered IDs were deleted. The independent final listing confirms
all 21 owned IDs and all three nonce-name sets absent. This does not claim to
exclude a future create beyond the bounded observation interval.

## External failure and authorized recovery

Run A's dedicated Ubuntu 26.04 worker `bx_2mxrfaus` hit npm `ECONNRESET` while
provisioning the locked Proton runtime. Run B built the identical target/SHA
successfully. The [original failure excerpt](npm-econnreset.txt) and failed
controller/target receipts remain intact. Bounded transient transport retries
are tracked in [#774](https://github.com/patrick-hudson/put.gd-desktop/issues/774).

The failed worker had already been deleted and verified absent before my SIGSTOP
reached run A. With Patrick's explicit approval, the single-target recovery used
run A's retained catalog worker `bx_jw76a9k5`, a separate clean source checkout at
the same SHA, its own build directory, and the exact original Ubuntu 26.04 CI-derived
recipe and container digest. It did not reuse catalog build outputs. The checkout and build directory were new.
The initial local clone borrowed Git objects through an absolute host path that
was invisible inside Docker; its CMake identity check failed. That attempt is
preserved in [its receipt](ubuntu26-recovery-git-alternates-failed/recovery.json)
and [error excerpt](git-alternates-failure.txt). The correction repacked the
objects into the recovery checkout, removed its alternates reference, and ran
`git fsck --full` before rerunning the same recipe. That next attempt then
stopped because the SDK provisioner unconditionally clones into a dependency
directory left by the first attempt. Its [receipt](ubuntu26-recovery-existing-deps-failed/recovery.json)
and [error excerpt](existing-deps-failure.txt) are retained. The complete failed
recovery build directory was moved outside the checkout with the worker's
authorized passwordless sudo (Docker had created that directory as root), and the unchanged recipe
was retried with a fresh build directory. The successful checkout
therefore had a self-contained object database at the identical commit. The original `/output` was untouched;
recovery wrote `/recovery-output` and was collected independently.
[Recovery receipt](ubuntu26-recovery/recovery.json), [driver](recover.py),
[exact remote script](ubuntu26-recovery/recovery.sh), and the per-file hashes
bind that result. The full recovery build log remains private alongside the raw
artifacts.

The catalog worker was retained because run A **had not ended**: its controller
was deliberately paused during the recovery while already-launched remote builds
continued. This is recorded in [hold](run-a-hold.json) and [resume metadata](run-a-resume.json). The
controller was resumed only after recovery collection; it then completed its
original failed manifest and deleted its remaining workers, including the catalog
worker. Final independent listing verification found none of our 21 IDs or three
nonce-name sets. The hold affects elapsed times; this campaign is not a build-speed
comparison.

An initial recovery helper import accidentally resolved the campaign's
`concurrent.py` instead of Python's standard library. Two controller attempts
rejected their already-existing output directories before any create/delete;
there were **zero** additional sandbox starts. The supervisor was renamed to
`campaign-supervisor.py`. [Incident metadata](recovery-incident.json) and retained
[attempt A output](accidental-import-run-a-controller.log),
[attempt B output](accidental-import-run-b-controller.log) preserve the mistake.
Their temporary metadata overwrite was restored by the still-running original
supervisor's in-memory launch record at completion. A subsequent recovery attempt
used the old worker's SSH pin and failed host verification before executing remote
commands; [its receipt](ubuntu26-recovery-ssh-failed/recovery.json) is preserved.
The successful retry used the catalog worker's original authenticated pin.
An unprivileged attempt to move the root-owned recovery build directory failed
before starting Docker; [that receipt](ubuntu26-recovery-move-permission-failed/recovery.json)
is retained too. The subsequent move used `sudo -n` on that exact owned path.
No sandbox ownership checks or host-key checks were weakened.

## Real API fields and fixture summary

The [real `boat list --all` field projection](api-fields.json) retains `createdAt`
and `ttlSeconds: 10800` for all 14 concurrent workers, with nontruncated pagination.
Thus both fields were actually available for this account/API/CLI at this time.
If these fields are missing/invalid, legacy fixed-name and unknown-lease workers
are kept: missing metadata never authorizes deletion. A known dead local nonce
lease still permits cleanup without remote time fields; a held lease protects a
live controller even after its TTL.

Both full runs built their fixture DEB from the pinned current-nightly source.
[`dpkg -I` output A](run-a-fixture-dpkg-I.txt) and
[`dpkg -I` output B](run-b-fixture-dpkg-I.txt) show:

```text
Package: putgd-lab-fixture
Conflicts: putgd
Replaces: putgd
Description: Disposable lab fixture build of put.gd; test use only
```

The package hashes are in acceptance.json and the manifests. `dpkg -I` reads
metadata only; neither fixture was installed on the workstation.

## Retention and validation

The manifests, target receipts and original creation streams, checksums,
controller logs, CLI audit and ownership verification remain readable here.
All large artifacts and full build logs remain in the private campaign directory.
No raw credential input or protected login data is included. The bounded
observation qualification is retained rather than relabeled as certainty.
CI and review state are linked from the two PRs at their exact heads.

Run B's seven targets passed as-is; run A's six successful targets and the
separately verified Ubuntu 26.04 recovery must be read together. The recovery
does not convert run A's failed controller manifest into a pass. Patrick explicitly
authorized this interpretation after the transient external failure: ownership
and complete cleanup remain the concurrency gates.
