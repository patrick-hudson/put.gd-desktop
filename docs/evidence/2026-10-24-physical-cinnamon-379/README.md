# Physical Cinnamon performance campaign (#379)

**Status: campaign in progress; no performance gate is accepted by this record yet.**
This is the newest-published-nightly rehearsal requested in
[#379](https://github.com/patrick-hudson/put.gd-desktop/issues/379).
[#384](https://github.com/patrick-hudson/put.gd-desktop/issues/384) must repeat
the driver on the exact stable assets. Nightly results cannot certify those bytes.

## Product and driver identities

The selected package is the published `ubuntu24.04` DEB from
[`v26.1024.1185-nightly`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1024.1185-nightly):

- Package: `putgd-26.1024.1185-nightly-ubuntu24.04-x86_64.deb`, 57,534,908 bytes.
- SHA-256: `2094fd4db8cd915b02f1cdd16eb5c99bb8191948931cd2f558a89c59be5b3f47`.
- Product source: `9dfac15c1ced201d13a2b70a8afb0c253213768b`;
  tree: `4e96912274d9220ba9476bd1b50333f6cf738f4f`.
- Package version: `26.1024.1185~nightly`.
- The download matches GitHub's asset digest and release `SHA256SUMS`.
  The mirror's checksum file is byte-identical to GitHub's, and its minisign
  signature verifies with nightly public key `71440FA5E439AEBB` and trusted
  comment `put.gd v26.1024.1185-nightly nightly`.

Installation at 15:10 UTC passed `dpkg --verify`; the embedded identity matches
the clean release commit above. Per-attempt running-process identities remain
in the raw receipts. The package must
remain the published bytes; the independently committed lab drivers are recorded
separately. Driver hardening is in [#554](https://github.com/patrick-hudson/put.gd-desktop/pull/554)
(#495); additional #379 performance drivers are in
[#555](https://github.com/patrick-hudson/put.gd-desktop/pull/555).
This documentation PR contains no driver code.
The recording contract fix (#410) has its own development-build evidence and is
not represented as part of this published package.

## Required workload and assessment

Run on physical m74, Linux Mint 22.3 / Cinnamon X11, with the real login session,
Qt platform, hardware, host load and process identities recorded. Measurements
require an exclusive desktop lock and no competing heavy SSH, VM or GUI work.
Keep the wall visible; use synthetic covering content, isolated private profiles
and the loopback S3 fixture. Preflight complete UTF-8 Unix socket paths before
each isolated launch. Retain failed attempts and corrected commands.

| Workload | Required assessment | Current result |
| --- | --- | --- |
| Region overlay, empty history | 5 warmups + 30 warm samples; p95 ≤150 ms | Pending |
| Region overlay, populated history | Same protocol with 10,000 independent image/video operation records | Pending |
| Cold/closed memory and idle CPU | RSS ≤40 MiB; CPU <0.5% of one core over 300 seconds | Pending |
| Closed family / History-open family | Closed RSS ≤40 MiB; PSS ≤29 MiB / ≤60 MiB, with helpers attributed | Pending |
| 10,000-entry history | Open, whole-list scroll, search, image/video preview, Copy image, Delete from destination; response/frame timings, CPU and memory | Pending; unbudgeted response thresholds need Patrick's explicit approval |
| Upload burst | At least 50 queued captures/uploads, concurrent slots, correct tray/history/links, memory during and after settle | Pending |
| Slow/stalled S3 | 256 KiB/s proxy, a stalled connection, visible progress, responsive UI/tray, measured cancellation and successful retry | Pending |

The memory limits are the approved plan's [source53 checkpoint](../../plans/2026-10-07-1926-feat-qt-linux-share-plan.md#clean-source53-cinnamon-checkpoint-and-wayland-compatibility).
The frozen idle harness's older 100 MiB `budgetPassed` field cannot replace the
40 MiB assessment. Preserve the original capture/idle harnesses and socket
ownership checks. A CLI acknowledgement is not proof that a frame was painted;
record those timings and witness limitations separately.

The populated fixture must have independent operation/item/history identities,
consistent operation journals and item state, mixed image/video records and
reproducible media, timestamps and search parameters. It is a seeded workload,
not a claim that 10,000 real uploads ran during the campaign. Preparation and
fixture generation are excluded from quiet measurement intervals.

## Preserved attempts

The physical host is `mint-builder` (m74), Mint 22.3, kernel
`6.14.0-37-generic`, Intel i5-6500T (four cores), Cinnamon `6.6.9+zena`,
Muffin `6.6.3+zena`, distro Qt 6.4.2, native login session `c89` / X11 / `:0`.
The existing powersave governor was preserved. The desktop lock was acquired
at 15:10:13 UTC. After coordinated reversible quiescence of a pre-existing VM,
a 10-second activity check recorded zero VM CPU ticks and no build, test or
encoder process. Other low-activity desktop services and the wall stayed up.

- **c1, driver `daf7145a`: incomplete capture workload.** The first warmup
  created a distinct cancelled region operation, but the unchanged witness
  returned `Actual resident selector did not map/paint on every display`.
  There are **zero measured samples and no valid p95**; raw `-1` is a sentinel,
  not a latency measurement. [The sanitized attempt](attempt-c1.json) binds
  the exact raw hashes and normal resident exit events. Source inspection
  suggests the historical white-at-outer-edge probe conflicts with the current
  black outer / inset white outline. This attribution is provisional; the
  frozen witness and failed attempt are unchanged.
- **m1, driver `daf7145a`: bounded memory pilot, still running.** Independent
  review identified insufficient continuous History-window checks and cleanup
  proof in the draft driver. Its summary cannot establish acceptance. Preserve
  its actual source and raw samples; acceptance requires a fresh run with the
  corrected, reviewed driver. No extra UI activity is added during its scored
  intervals.
- **Populated gates: held.** The earlier #495 grow revision was staged but never
  executed. Persisted-network-state validation, DB/runtime association and full
  helper ownership guards require a corrected reviewed handoff before use.

![c1 post-cancellation recovery frame: only synthetic background, no selector-paint claim](c1-witness-failure.png)

This is an annotated recovery frame from the tested nightly after the failed
warmup and cancellation, not a picture proving that a selector was absent or
painted. The original reviewed frame contains only synthetic cover pixels;
its SHA-256 is retained in the attempt JSON.

## Evidence and cleanup

[campaign.json](campaign.json) binds the selected package, budgets and current
gate status. Private raw samples, exact commands, failures, original visuals and
identity/cleanup receipts are retained under m74's private
`~/putgd-lab/p379/` and the per-attempt `/tmp/p379-*` roots; completed run roots
will be exported before cleanup. Further reviewed sanitized results and
annotated pictures will be indexed here when collected.

Before measurements, finish the arranged #495 functional window and establish a
quiet host. A pre-existing VM requires coordinated reversible quiescence; its
original state and verified resume must be retained. Do not touch immutable
bases. Stop only campaign-owned processes, restore temporary desktop state,
remove only campaign-installed package/profile changes and obtain a verified
desktop-lock release receipt. A failed measurement remains a failure until
fixed and rerun; missing evidence remains an open gate.
