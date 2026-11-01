# Live-S3 memory evidence

These archives retain numeric samples, metadata-only operation/public-byte/native-paste receipts, exact deletion results and offline JUnit. They contain no credential values, payload bodies or screenshots. Their member hashes and archive hashes are in `manifest.json`; the observer and evaluator are frozen copies with recorded hashes.

Replay from any checkout with Python 3:

```sh
python3 docs/evidence/live-s3-memory/replay.py
python3 docs/evidence/live-s3-memory/replay.py --bundle idle104-repeat
```

The replay uses a new private temporary directory, verifies every regular archive member, checks size/hash bounds, and compares newly evaluated results with the archived result. It requires no Qt, AWS access, desktop, Node.js or guest. A GitHub Actions job can run the first command and retain its JSON output as an artifact. This checks saved evidence; it does not rerun capture, upload or graphical acceptance.

`--require-target` fails when any selected run exceeds 40 MiB. The three current runs intentionally fail that target check: they document the baseline and a useful partial improvement.

| Archive | Measured source | Closed-view RSS + swap |
| --- | --- | --- |
| `baseline102.tar.gz` | `c5c7dc9e3fc4` | 49.4453125 MiB |
| `idle104-first.tar.gz` | `02ad835f4a5e` | 47.87890625 MiB |
| `idle104-repeat.tar.gz` | `02ad835f4a5e` | 46.99609375 MiB |

All runs cover 30 operations and 31 objects across text, image, files, region, window and desktop acquisition, followed by a five-minute quiet observation. The candidate's two valid samples differ by 0.8828125 MiB. The unchanged optimization decision script uses their median, 47.4375 MiB; the additional conservative check uses the worse run, which saves 1.56640625 MiB against the baseline. Both exceed the required 1 MiB improvement. Both still miss 40 MiB.

Source104 also passed all 55 standard dev tests, six selected Release S3 tests, three locked MCP tests, and current Cinnamon native/GUI acceptance. The native suite covered 12 Qt/GTK source/cancellation cases, Settings Cancel, the native folder chooser, populated History/Activity, tray/menu and normal helper exits. Its receipt recorded 15 file descriptors versus 14 initially; it is functional proof, not a claim that every resource count returned to its initial value. Fresh live profiles separate that suite from memory scoring. GNOME/KDE expansion remains deferred until the cumulative Cinnamon candidate meets the target.

The repeat preserves an invalid first fixture attempt: its synthetic source marker was not ready at the fixed read, before any action invocation. The marker later arrived. Explicit visible focus and fresh-marker readiness corrected preparation; the frozen driver, observer, workload and evaluator remained unchanged. Its failed upload reservation remains counted. Archive timestamps are normalized for deterministic compression; original timestamps remain in the receipt data.

The legacy filename `baseline102.json` is retained within every bundle for the frozen evaluator's input contract. Read its compiled build manifest to identify the actual measured source; candidate archives contain source104, not source102. Later documentation commits do not relabel measured binaries.

For a new run, follow [the live-S3 protocol](../../history/testing-live-s3-memory.md), [standard build instructions](../../build.md) and [immutable VM lifecycle](../../vm-lab.md). Freeze a new clean source/package identity, use a fresh private profile and namespace, run current native parity and the recorded live workload, and preserve exact receipts before cleanup. The observer's original lab profile/stage paths must match that recorded setup. Do not reuse historical install/start commands or classify replay as new compositor proof.
