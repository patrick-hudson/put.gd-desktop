# Startup reclamation source digest

Reference is committed product 2e789b8feedb974f04984dc460a9ffb52c8a16cb. The unchanged physical Cinnamon observer measured 48,431,104 bytes (46.1875 MiB) peak/final RSS over 300 seconds, 0.006662% one core, zero operations and no activated owned windows. Native GTK fonts/dialogs remain loaded. This cold workload uses private synthetic local configuration and does not capture pixels, clipboard or cloud data.

OperationService already owns a two-second single-shot timer. It calls glibc malloc_trim(0) only when pending and no stopping flag, workers, queued/running/active operations, selector or image-byte leases remain. Each background worker stops the timer; finished removes itself and reschedules. Acquisitions and decoded copy paths request reclamation; startup currently does not. Journal initialization completes asynchronously; only successful initialization may request this cleanup. Callback and QThread::finished event order must both be safe. Live Qt data and native theme objects stay owned.

Standalone native QApplication locating probe recovered 1,855,488 and 1,859,584 bytes with one trim and unchanged allocated bytes. This is attribution only, not a product saving. Smallest decisive product test is this single minimal request plus unchanged 300-second measurement. No useful further cheaper locating probe changes whether to try it.

Only src/app/OperationService.cpp and tests/unit/test_operations.cpp may change. Tests already cover blocked upload, queued image cancellation, active selector/background work, cancelled first acquisition and idle no-repeat behavior. Add focused successful startup, failure and shutdown checks as warranted; do not weaken existing tests. Root handles tests, measurement, scoring and commits.

Kept outcome: successful initialization now requests the existing idle cleanup once under __GLIBC__. Reviewed file hashes and measured44.96484375 MiB are in the experiment log; the baseline description above remains historical.
