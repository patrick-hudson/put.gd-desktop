# Source53 Cinnamon numerical archive

These are transformed numerical projections of the completed source53 physical
Cinnamon checkpoint. They retain measurements and original fingerprints without
exporting configuration, databases, payloads, image pixels, argv, environment,
arbitrary logs, mapping names, source archives or binary bytes. No new runtime
test was performed while producing this archive.

The measured commit is `1ee42f32c61c3d93d88cc1d3dc624d9ad086bb70`, count 53,
version `0.1.1-dev.53+g1ee42f32c61c`. The reviewed expansion gate SHA256 is
`834071fcf867ac6be7a032a1e2a67228703046776820c854fef820387926fece`.
Its 96 references are historical fingerprints, not 96 tests. They remain in
`manifest.json`; files such as binaries and source archives are not copied.
Current build files may later change, so this archive does not attempt to
rehash unrelated current binaries and claim they are still source53.

The 22 JSON projections contain all 30 timing samples and their driver/window
witness costs; both cold primary sample series and numeric initial/final
footprint rows; all 289/288 cold-family samples; all 42 view samples in each
of four runs; detailed per-process rows for those same view samples; both
three-generation normal-exit receipts; and failed source51/source52 checkpoint
qualifiers. Detailed view rows duplicate the same observations at another
level; they are not additional independent repeats.

Every exported object has `transformed:true` and its exact original path and
SHA256. The manifest also hashes the exported bytes. `final-gate-receipt`
bindings match the original reviewed gate. `additional-numeric-detail` rows
are separately hashed sample JSONL projections whose shared fields and sums
are checked against the corresponding gate-bound result; they are not silently
represented as directly listed gate receipts. `prior-failed-checkpoint` rows
remain failed historical qualifiers.

The portable verifier requires only these projections and the three accompanying
Python scripts. It does not open any `/tmp` originals, connect to an app/VM,
query a desktop, or install dependencies. Keep the packet directory structure:
the scripts live in this evidence directory's `tools/` subdirectory. Verify with
standard Python:

```sh
python3 docs/evidence/source53-cinnamon/tools/verify.py
python3 docs/evidence/source53-cinnamon/tools/test_verify.py
```

The verifier checks exported/tool hashes, source/build scope, nearest-rank p95,
35 distinct cancelled/no-result operations, cold coverage and peaks, all three
settled view cycles, 40 MiB closed RSS / 29 MiB closed PSS / 60 MiB open PSS,
per-process sum consistency, reported CPU at least 0 and below 0.5% of one core,
and independent supplemental normal-exit status joined to the measured resident's
PID, start ticks, UID, device, inode and app SHA256. Executable paths remain
omitted; the app hash comes from the measured raw result's `binaries.app.sha256`
and its derivation is labelled.
It checks the summaries against recomputed values. The ten static contracts
also reject sparse coverage, tampering, stale builds, missing/crashed/wrong
exit generations, wrong peers, self-matching foreign resident identities, matching over-budget CPU,
nonquiet state, and promoting a failed attempt.

Recomputed source53 values are:

| Measurement | Result |
| --- | --- |
| Selection paint, 30 samples after 5 warmups | p95 95.400159 ms; gate 150 ms |
| Cold-family peak RSS, repeats 1 / 2 | 24.76953125 / 24.734375 MiB |
| Cold-family sample count, repeats 1 / 2 | 289 / 288 |
| Cold-family span, repeats 1 / 2 | 299.900948 / 299.426819 seconds |
| Cold-family maximum gap, repeats 1 / 2 | 1.100497 / 1.101007 seconds |
| Primary duration, repeats 1 / 2 | 300.127959 / 300.137221 seconds |
| Original final closed RSS, repeats 1 / 2 | 25.22265625 / 25.328125 MiB |
| Supplemental final closed RSS, repeats 1 / 2 | 25.359375 / 25.3359375 MiB |
| Supplemental final closed PSS, repeats 1 / 2 | 16.390625 / 16.38671875 MiB |
| Largest open-family PSS | 50.88671875 MiB |

The primary CPU values, approximately 0.00333% of one core, are retained and
cross-checked with the family receipt. The primary did not record `CLK_TCK`
in its result, so this archive does not invent a machine-specific clock value
or claim an independent CPU tick conversion. Footprint rows retain their source
order and numerical fields while omitting mapping names; they are not an
allocation ownership or atomic footprint proof.

Original view results preserve normal exit evidence for generations 1 and 2
only. Generation 3's status is absent. The separate unchanged-workload replays
add three authenticated read-only snapshot queries each and retain their own
memory values and measurement noise. They prove normal exit 0 for generations
1, 2 and 3 in those supplemental runs. They do not retroactively fill the
original missing status, infer normal exit from disappearance, or turn the
original runs into differently instrumented measurements.

Source51's whole-family collector failed despite its primary reporting a low
RSS and completing 300 seconds. Source52's collector was interrupted after
about 70 seconds. Their exact checkpoint/output hashes and false acceptance
flags remain here. Later process-disappearance diagnosis does not prove the
precise original failing call. Neither failed attempt establishes the target.

This archive establishes the sampled source53 physical Cinnamon cold/empty-view
checkpoint described by the original gate. It does not certify source54 or any
later binary, GNOME/KDE memory, post-upload or populated-history retention,
a permanent plateau, continuous short-lived-process absence, mixed-DPI,
whole-product feature parity, or full U9 acceptance. Native graphical evidence
remains in its separate reviewed cohort; numerical exports are not screenshots
or semantic graphical proof.

The exporter preserves originals and refuses to overwrite exports. An initial
static path concatenation failure and earlier draft projections are retained
beside the final packet, outside the proposed durable evidence directory. They
are export-development records, not failed application measurements. Promotion
of these files or script entry points requires review; no tracked repository
file was changed for this draft.
