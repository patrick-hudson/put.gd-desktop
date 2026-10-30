# Boat candidate follow-ups (#752)

Code: [#753](https://github.com/patrick-hudson/put.gd-desktop/pull/753). Companion docs: [#754](https://github.com/patrick-hudson/put.gd-desktop/pull/754).
Application source is nightly `49c217bd4662f62b8482154c18070d4e2076d2ea`, frozen at campaign start. The live controller uses `f2426781007c012b0e142609e7c6e0514c09a0b7`; later controller commits add fake-CLI tests, treat a zero monotonic start explicitly, and make a held local lease override TTL expiry (review ca13f378). No recipe changed. The live run does not test an expired-TTL peer; the regression test covers that case. No desktop or release acceptance is claimed.

## Scope and quota correction

The coordinator limited live acceptance to the existing seven-worker nonce `6f16cffa07164fcd`, deferring live same-SHA concurrency and interruption until the shared daily quota resets. No new sandbox was requested after that instruction. The two scenarios are covered by a network-free fake Boat CLI whose service records every create/delete and delays a worker’s visibility until after the CLI is terminated before printing an ID.

An extra seven-worker controller had already started before the budget alert. A branch switch while its launch was delayed made it load the old fixed-name controller; it was stopped immediately after the alert and all seven workers were deleted. [Its failed manifest](cancelled-old-controller.json) is preserved as operational failure context, not as validation of the new controller. This task consumed fourteen starts, not the originally planned twenty-one. There was no third run.

The full-run controller was temporarily SIGSTOP’d after all seven SSH builds started, retaining those same workers for the requested fixture package test. Remote build children continued. It was resumed after the conflict proof and collected/deleted every worker. This hold is part of wall time, not a build performance comparison.

## Controller validation

[37 tests](controller-tests.txt) cover nonce leases, dead/unknown/expired run cleanup, the full creation recovery window, fake-CLI events, package recipe metadata, PKG_CONFIG_PATH preservation, transport validation, phase recording and size-scaled collection budget. [The focused subprocess fake run](fake-cli-tests.txt) repeats the concurrency/interruption contracts and retains its [service event logs](fake-cli-events.json), with every created ID deleted and the service empty afterward.

Live concurrency is **deferred**, not passing. The issue remains open for the coordinator. No issue is closed or PR merged by this task.

## Collection investigation

[Controlled before/after comparison](collection-comparison.json) runs the old collector from nightly and the new collector on the same completed output in the same existing workers, each with an exhausted build budget (`timeout=1`). Both use the same uncompressed `tar -cf` transport; every file inventory and archive SHA-256 matches between collectors.

| Target | Archive bytes / files | Old seconds / reserve | New seconds / reserve | New probe / pack / transfer / restore seconds |
| --- | ---: | ---: | ---: | --- |
| Debian 13 | 59,975,680 / 7 | 15.422 / 150 | 22.017 / 609 | 1.732 / 2.030 / 18.030 / 0.225 |
| Ubuntu 24.04 | 64,942,080 / 9 | 18.140 / 150 | 22.014 / 647 | 1.829 / 2.011 / 17.794 / 0.380 |

This is a budget fix, not a speedup claim. The ~385-second validator outliers in #752 did not reproduce, and the old aggregate-only receipts cannot identify their slow phase. The earlier merged packet also shows variability: Debian 124.922 s, Ubuntu 26.590 s and fixture 407.546 s, rather than a persistent distro-specific penalty. Both implementations already avoid compression and recursive-file transfer. The new probe recorded ext4, 33–34% filesystem occupancy and contemporaneous I/O pressure. With only 7–9 files, packing around two seconds and restoration below half a second, these measured copies spent most of their time transferring. These observations cannot distinguish historical remote storage and network stalls. The reserve now scales with size/file count and future receipts preserve phase and disk context for attribution.

## Real fixture conflict proof

The current production Ubuntu 24.04 DEB and fixture DEB were installed in one disposable Ubuntu container inside existing fixture worker `bx_jrjetfwk`. [Identity/command/hashes](fixture-proof.json), [the exact script](fixture-container.sh), and [full saved output](fixture-conflict.log) bind the proof to this run. Runtime dependencies were installed inside that container. No workstation or physical host package was installed.

`dpkg -i production.deb` succeeded. `dpkg -i fixture.deb` succeeded by explicitly removing production in favour of the fixture (the intended Conflicts/Replaces transition). Installing production again returned 1 with `putgd-lab-fixture conflicts with putgd` and `conflicting packages - not installing putgd`. No `trying to overwrite` error occurred. The final state was fixture installed, production not installed. Control metadata records `Conflicts: putgd`, `Replaces: putgd` and the distinct disposable-test description.

The [first proof invocation](fixture-first-attempt.json) omitted Docker’s `--interactive`, so it received no stdin script and produced an empty log despite exiting 0. It is not proof. The corrected invocation retained stdin and required the saved `FIXTURE_CONFLICT_PROOF_PASSED` sentinel; the container result above is that verified correction, using the same existing sandbox and same package bytes.

## Full current-nightly run and cleanup

[The complete candidate manifest](candidate.json) has all seven targets passing at source `49c217bd4662f62b8482154c18070d4e2076d2ea`, with clean source/binary identities and per-file hashes. All **490 files** in [SHA256SUMS](SHA256SUMS) passed an independent `sha256sum -c SHA256SUMS` replay ([saved output](sha256-verify.txt)). The fixture proof’s two DEB hashes match this manifest.

| Target | Verified files | Main-run collection seconds |
| --- | ---: | ---: |
| appimage | 8 | 41.603 |
| debian13 | 7 | 21.509 |
| fedora44 | 7 | 21.987 |
| lab-fixture | 8 | 28.693 |
| ubuntu24.04 | 9 | 19.917 |
| ubuntu26.04 | 7 | 18.978 |
| ui-catalog | 444 | 11.756 |

The controller’s final cleanup receipts verify all seven nonce workers absent. The already-cancelled baseline’s seven fixed-name workers are absent too: [all fourteen owned IDs](all-owned-workers-gone.json) were checked against `boat list --all`, preserving unrelated workers. [Explicit final `--cleanup SHA`](cleanup-mode.json) succeeds without any new worker. Named snapshots were never deleted or modified. Private raw logs, bootstrap/recipe scripts, pinned keys and artifact bytes remain under `/home/patrick/work/boat-candidates/issue752-20261030/`; none is a release publication.

Live same-SHA concurrency remains deferred by the coordinator’s quota instruction. The fake-CLI concurrency/interruption gates pass, the single requested full run passes, and the extra cancelled old-controller baseline is retained separately. Issue #752 remains open. Required CI and review state are reported in the linked PRs.
