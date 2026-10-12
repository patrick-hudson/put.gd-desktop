# Live-S3 residency: source106 checkpoint

Measured source: `76467a75b18edc56fd473c63cdf3bb5b17796354`, version `0.1.1-dev.106+g76467a75b18e`. Later documentation commits do not retest or relabel these binaries.

## Cumulative strategy and Cinnamon result

The source104 idle teardown improvement remains qualified and preserved. Two matched runs measured 47.879 and 46.996 MiB, versus source102 49.445 MiB. That recovered at least 1.566 MiB but did not reach 40 MiB.

Source106 retains that change and moves Linux SDK calls into a transient same-build worker. The resident passes credentials only through private pipes, reads payloads from verified immutable artifacts, acknowledges durable journal checkpoints, and accepts a terminal result after normal worker exit. It does not unload libraries underneath callbacks or automatically replay an activated request. The child runs the original official SDK driver on its owner thread. Linux resident idle mappings no longer contain the AWS/CRT transport libraries. Existing graphical process boundaries, native tray, fonts and dialogs remain intact.

Two independent Cinnamon live-S3 workloads passed all six sources, 30 operations/31 objects per run, actual public bytes, native URL paste, region pixels, full-family process accounting and exact receipt-guarded cleanup. Their five-minute post-use peaks were 28.953 and 28.852 MiB (median 28.902; spread 0.102 MiB), with no swap or remaining helpers. The conservative worst-run reduction from source102 is 20.492 MiB, 41.444%. This is a cumulative result, not an invented per-change allocation saving.

The fresh repeat overlapped independent guest compilation; that concurrency is retained in the record. An additional zero-upload, post-cleanup Cinnamon idle confirmation after all builds drained measured 30.520 MiB. It is a different workload and does not replace either matched run.

## Acceptance blocker: GNOME and KDE live requests

Current source106 GNOME and KDE each passed 12 native and eight graphical checks. Their normal guest CMake/Ninja builds and dev tests passed (GNOME 58; KDE 57). These are real Wayland desktops visible on the VNC wall, not offscreen compositor claims.

Their subsequent production SDK runs repeatedly returned HTTP -1 / Unclassified. Some safe pre-PUT HEAD requests failed; other PUT results were uncertain and required authenticated exact-object reconciliation. Explicit guarded retries recovered some operations, but neither desktop completed the required six-source live workload. KDE production deletion also failed for 12 of 28 receipts after 16 removals. Emergency exact conditional cleanup, if used, is cleanup evidence rather than production History acceptance.

Standalone GNOME official-SDK exact HEAD probes returned six authenticated 404 results; KDE ordinary curl probes reached S3 with TLS verification. Those probes show current connectivity in their own scopes. They do not explain or erase production failures. The cause remains unresolved; no GNOME/KDE post-S3 target result is accepted. Preserve source106 on its experiment branch, preserve the passing Cinnamon result, and do not globally accept the candidate until this correctness gate passes.

## Reproduction and follow-up

Use the frozen workload, observer and evaluator documented in `docs/testing-live-s3-memory.md`. Saved numeric evidence is under `work/live-s3-memory/evidence106/`; immutable failures, retries and cleanup receipts remain in the private desktop bundles. Never concatenate observers across recovery or relabel recovery as a fresh profile. Charge reserved but unexecuted slots and retries without refunds. Do not expose credentials, payloads or raw SDK request headers in diagnostic exports.

Next diagnosis should capture a bounded numeric SDK transport reason at the actual production failure, then compare current worker and retained source104 behavior on one isolated guest. A healthy standalone HEAD is insufficient. Keep original tests and source pins, and repeat the complete live workload only after the cause is addressed. No new dependency or desktop power-setting change is part of this checkpoint.

## Cleanup and stopping point

All 170 receipted objects created during the approved experiments were removed: 124 across the two candidates’ Cinnamon runs, 18 on GNOME and 28 on KDE. GNOME production deletion removed 12 and left six unknown; KDE removed 16 and left 12 unknown. Exact authenticated receipt/metadata/ETag guards permitted emergency conditional deletion of those remaining 18 objects. All fallback removals were checked with authenticated HEAD; production unknown History states were preserved. GNOME’s uncertain PUT target was also authenticated missing. Its 329-byte failed artifact remains private for diagnosis. Both Wayland test product families and observers exited normally.

The conservative budget reserved 187 objects and 39.1875 MiB against limits of 256 and 64 MiB; failed and unexecuted reservations were not refunded. No bucket policy, versioning or existing user objects changed. Testing stopped at the approved extended window; no third candidate was attempted. Source104 remains the qualified integration checkpoint; source106 and its passing Cinnamon result remain preserved for fixing the failed Wayland live gate. Ordinary model/review cost was not separately measured.

To replay the exported Cinnamon measurements without desktop or cloud access:

```sh
python3 docs/evidence/live-s3-memory/source106/replay.py --require-target
python3 docs/evidence/live-s3-memory/source106/replay-postcleanup.py
```

`failure-index.json` hashes original metadata preserved beneath `failures/`; these incomplete workloads are intentionally excluded from the successful-run manifest. Native and final guest receipts are separate JSON files. Private payloads and the original diagnostic artifacts remain local and are not exported.
