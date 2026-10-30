# Boat candidate controller follow-ups

This packet records the follow-ups to [code PR #685](https://github.com/patrick-hudson/put.gd-desktop/pull/685)
and [docs PR #686](https://github.com/patrick-hudson/put.gd-desktop/pull/686), plus
[#715](https://github.com/patrick-hudson/put.gd-desktop/issues/715) and
[#721](https://github.com/patrick-hudson/put.gd-desktop/issues/721).
The campaign started on 2026-10-29 local time (2026-10-30 UTC).

The frozen application source was `origin/nightly` at campaign start:
`573a11c831c2649f4b81f5a8ae86a120446f2421`. All products carry that clean,
untagged development identity. Controller commits and retries are recorded
separately from the application identity. This is candidate packaging,
offscreen catalog and unbooted disk-lifecycle evidence. It establishes no new
guest boot, compositor, physical hardware or release acceptance.

## Candidate builds

All seven artifact builds passed across the retained attempts. The final accepted
set has **452 verified files**; every artifact has the requested clean
commit and matching compiled CLI identity.

| Artifact | Build/provision seconds | Collection seconds | Exported files |
| --- | ---: | ---: | ---: |
| appimage | 964.644 | 26.797 | 8 |
| debian13 | 488.946 | 124.922 | 7 |
| fedora44 | 419.586 | 26.424 | 7 |
| lab-fixture | 488.999 | 407.546 | 8 |
| ubuntu24.04 | 1434.788 | 26.59 | 9 |
| ubuntu26.04 | 552.157 | 17.698 | 7 |
| ui-catalog | 718.307 | 9.234 | 406 |

[Structured results and product hashes](candidate.json) retain image digests,
per-target identity, transport digests, CI differences and deletion receipts.
All **14 sandboxes created by this task** are [verified absent](all-workers-gone.json)
using `boat list --all`; unrelated workers and named snapshots were preserved.

The test DEB is named `putgd-lab-fixture`. Its compiled update information has
`packageTarget: null`, `supported: false`, and `reason: disabled_at_build`. Its
production `native.py` audit is deliberately not run because that policy forbids
fixture files and requires the package name `putgd`; the four production native
packages passed their normal audits.

The first seven-worker attempt retained an AppImage bootstrap failure: the
minimal Ubuntu container lacked Python before the newly added mirror helper.
The controller was corrected to provision distro Python first, and only
AppImage was retried at the same frozen application SHA. All seven first-attempt
workers were deleted; its complete failed manifest and 444 successful-file
checksums were preserved and verified. The second AppImage attempt was
deliberately cancelled mid-build after it
became a measured storage outlier: more than half an hour in base dependencies,
versus the earlier rehearsal's complete AppImage build in 899.018 seconds,
with I/O full pressure averaging 37.58% over five minutes and 13,237 MiB memory
available. Its package-install timeout/retry and mid-build cancellation remain
failed receipts. One replacement worker then built the same source with the
same pinned recipe; no worker-replacement loop was added. No failed attempt was
relabeled passing. The [interrupted manifest](interrupted-appimage.json) and
[byte-identical recipe/bootstrap hashes](replacement-recipes.json) retain that
operational replacement context.

The accepted artifact set is restored again from the original transport
archives using the final extraction implementation. Archive digests, every
restored file digest, build identity and package count are rechecked. Files keep
private permissions and owner executability where applicable. The accepted
manifest expands the CI difference metadata without changing the original
attempt receipts or artifact bytes. Runner summary/env/output files and aqt
scratch data are excluded from the exported artifact inventory.

The [same-source AppImage CI comparison](ci-appimage-comparison.json) records the
remaining audit difference precisely: Boat's
`postDeploymentAudit.rootDesktopEntry.removedLinks` is
`["putgd-record.desktop"]`, while [CI run 40978718771](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40978718771)
has `[]`. Both keep `putgd.desktop` as the single root desktop entry. This is a
recorded container/hosted-runner assembly difference, not an identical-byte CI
claim. The guide and each target's manifest metadata list the remaining cold
cache, image, Qt provisioning, installer policy and fixture audit boundaries.

## Controller, cancellation and transport

[29 controller contracts](controller-tests.txt) cover pending/stopped worker
cleanup, name recovery after a create without an ID, SIGINT/SIGTERM/SIGHUP,
child-session isolation, repeated cancellation, explicit SSH configuration,
archive hash/path/link validation, collection budgets and primary collection
after an optional benchmark failure. The closed-output test checks that the
completed manifest and checksums exist before the first final print.
[Separate real processes](closed-output.json) close stdout or stderr and both
exit 0 without a shutdown flush error.

The [deliberate cancellation](cancellation.json) sent two terminal-process-group
Ctrl-Cs while `bx_v695pu7v` was starting. Its initial pending deletion request
is retained separately from the verified final absence from `boat list --all`.
The controller exited 1 with a completed failed-run manifest, checksums and no
owned workers remaining. The later deliberate mid-build cancellation of the
I/O-outlier AppImage worker also deleted that worker and recorded final absence.
[Signal timing](cancellation-signals.json) shows that
the second Ctrl-C arrived during cleanup. The actual
[`--cleanup SHA` recovery invocation](cleanup-mode.json) also verified all seven
fixed candidate names absent without starting any new worker.

The same 173-capture catalog and metadata were copied by both transport methods:
legacy recursive SCP **206.097 s**, verified tar transfer/extraction **9.234 s**,
**22.32 times faster**, with identical per-file inventories. Its 18,821,120-byte
transport archive has SHA-256
`5be4b26d3fab2f603a155360d1f4ff5308e04c8281d307c424c0a1f16fcd3eec`.
The worker was deleted and verified absent. [#715 was closed with this evidence](https://github.com/patrick-hudson/put.gd-desktop/issues/715#issuecomment-6512459920).

## Canonical snapshot and guarded retirement

[Snapshot identities, golden hashes, retirement and deletion receipts](snapshots.json)
record separate checks of `putgd-build-base`, `putgd-lab-cinnamon`, the repaired
GNOME preparation, and a fresh fork of the new named snapshot.

`putgd-lab-ubuntu-gnome-v2` is snapshot
`5497ec24-56ff-44b9-b046-851fcaa91a48`. Its canonical disk is
`/home/user/work/put.gd/work/lab-images/gnome-wayland/image.qcow2`, a standalone,
read-only, single-link copy of inventory image `gnome-provisioned-20261016`,
SHA-256 `563730cc1d929bd946d20cf30a9aecfe3c4ad40eb12ba7f9abcff1c461e342bb`.
The fresh validation fork `bx_xaa48327` fully hydrated before the unbooted
qcow2-overlay test. Both guarded dry-run and actual `lab.py cleanup` succeeded
with `--images-root /home/user/work/put.gd/work/lab-images` and hashed explicit
completion evidence. The overlay was absent afterward; the golden hash stayed
unchanged. No cleanup guard was weakened.

Cinnamon and build-base already use canonical `work/lab-images` paths. Both
passed the same actual unbooted-overlay retirement and unchanged-golden checks,
so they need no replacement names. All four inspection/validation sandboxes
were deleted and verified absent. Their source and new named snapshots remain
ready. The old `putgd-lab-ubuntu-gnome` (`ec09fae5-901a-4b53-8ef4-8358f712e5c9`)
is **preserved and superseded**, not deleted; the original golden was copied,
not moved or removed. [#721 was closed with this evidence](https://github.com/patrick-hudson/put.gd-desktop/issues/721#issuecomment-6512621747).

## Reproduction and retained evidence

Use the [candidate guide](../../boat-candidates.md), its `--benchmark-catalog-scp`
option for a same-data comparison, and its `--cleanup SHA` mode for recovery.
The [Boat copy inventory](../../remote-golden-images.md#verified-boat-snapshot-copies)
distinguishes the provisioned GNOME bytes from the older unprovisioned image
whose historical inventory name is `gnome-wayland`.

Private complete evidence is outside Git at
`/home/patrick/work/boat-candidates/fixes-685/`: both candidate attempts and their
generated recipes/bootstrap scripts, transport archives, original and final
restored artifacts, installed package inventories, audit receipts, full hashes,
failed-attempt context, `snapshot-check.py`, `validate-v2.py`, hashed completion
manifests and retirement/deletion receipts. Goldens remain on Boat; none was
transferred to the workstation. The PRs remain unmerged for the coordinator.
