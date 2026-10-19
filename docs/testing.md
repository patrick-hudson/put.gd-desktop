# Testing

Build and run repository tests with `cmake --preset dev`, `cmake --build --preset dev`,
and `ctest --preset dev`. See [build.md](build.md) for pinned SDK provisioning and
sanitizer builds. Qt offscreen tests prove core/widget contracts; isolated Xvfb checks
prove X11 protocol behavior. Neither establishes real Cinnamon or Wayland support.

Run host lab contracts with `python3 tests/desktop/test_lab.py`. These cover private
QEMU command construction, QMP protocol/event handling through an actual UNIX socket,
native session rejection, TCG evidence classification, locked ISO identity, independent
pixel/paste comparisons, and actual/public byte hash/URL-paste mismatch rejection.
The socket peer is synthetic and is expressly not compositor evidence.

Use [vm-lab.md](vm-lab.md) to provision a golden Cinnamon guest, install the development
DEB and execute a named scenario. The single command `python3 tools/lab/lab.py run
--run RUN --scenario region` emits machine-readable results/JUnit and inspectable
synthetic evidence; use the other named capture/clipboard scenarios and configured
S3/history options as documented. Every graphical pass needs actual saved/public bytes
and independent native paste, plus external QMP witnesses. Never call a profile supported
from portal discovery, offscreen mocks or uploader-window screenshots.

Physical Cinnamon acceptance and the GNOME/Plasma native Wayland/helper matrix are still
required. Full U6 completion additionally needs two independent golden-image runs,
wrong-result and timeout cleanup witness evidence, and the required multihead layout
cases. U9 requires a fresh-agent walkthrough; U10 owns release-package lifecycle proof.
Keep credentials out of commands, evidence and golden images. Public service tests use
synthetic objects in an isolated destination and never alter bucket controls or user
objects. Record actual operation/history IDs and deletion effects before confirmation.

## Structured records and replay

Use `ctest --preset dev --output-junit "$PWD/build/dev/ctest-results.xml"` to retain
JUnit alongside CTest’s `build/dev/Testing/Temporary/LastTest.log`. GitHub Actions
uploads these reports on success or failure, with a separate artifact for each
distribution and 30-day retention. Hosted CI covers contracts, not native compositor acceptance.

Each graphical `run` appends start/finish JSON events to `RUN/test-events.jsonl`.
Each attempt receives a unique ID and a preserved `RUN/tests/ID/` bundle, containing
`parameters.json`, results JSON/JUnit, package/runner identities, native display
layout, requested/actual hotkeys, timing, pre-action selection/window geometry,
synthetic artifacts/paste witnesses (including `history-paste.json`), every focus
poll and selector screenshot, and a SHA256 manifest. Focus polls use distinct
filenames within the attempt; optional witnesses are cleared before a fresh run. Root-level files remain
the latest attempt for interactive inspection; the per-attempt bundles do not overwrite
earlier failures. An interrupted attempt retains its start event.
Image checks compare every decoded clipboard/region pixel. The 80×60 region
starts at fixture offset (280, 130), crossing all four color quadrants; window
and desktop checks compare the full 640×320 deterministic paint area using
independent pre-action geometry.

The lab maps a pixel coordinate to the first tablet tick at or above its edge:
`ceil(pixel * 32768 / viewportExtent)`, capped at 32767. Dividing by
`viewportExtent - 1` can cross Qt’s half-pixel boundary: on KDE a requested
600-to-680 drag became 600.46875-to-680.5078125 and selected 81 pixels.
The corrected ticks returned exactly 80 pixels from the actual full-screen
helper. Region gestures hold each press/move phase for 100 ms and retain a
mid-drag witness before release. Parameters record both the mapping and timing;
these functional-input delays do not modify the frozen performance observers.
The original cancelled attempt remains unexplained; gesture pacing does not
establish its cause. Complete image/paste oracles and deadlines remain unchanged.

Failures during press, move, witness capture or endpoint release request Escape
and a release at the start point. Cleanup errors remain separate from the original
error. A lost QMP reply can follow a delivered event, so the runner also records
new operation IDs/states after failure; an incomplete or empty snapshot does not
prove there were no side effects. Each failed attempt retains its witnesses.

Read the saved `replayCommand`, recreate the same profile/display layout in a new
disposable overlay, and install the package whose hash is recorded before running it.
Replace `<disposable-run>` with that overlay’s directory. Verify the current runner
hash and saved hotkeys/timing; changed inputs are a new experiment, not a replay.
Remote publication additionally requires the explicit `--settings` keychain-reference
profile and an isolated destination; the saved metadata cannot silently authorize an upload.
KDE fixture hotkeys are configured in `tools/lab/profiles/kde-wayland.yaml`; current
KWin consumes Shift on printable nonletters, so acceptance uses unambiguous function keys.

A GUI/self-hosted CI job can invoke those same lab commands and upload only the
curated `RUN/tests/` bundles and event journal. Never upload an entire run directory:
it also contains VM authentication keys and a writable disk. Never archive
`credentials.txt`, guest passwords, keyrings, configuration with inline secrets,
or real user clipboard contents. Generic keyboard/shell tracing is deliberately absent;
parameter records cover only the controlled synthetic scenario. Keep VNC active so
the user can observe native interactions on the display wall.

## Physical performance records

The release idle harness is `tests/performance/bench_idle.py`; use a short private
output path under `/tmp` because its local socket must fit the Unix path limit.
Only a full 300-second run establishes the idle budget. Results include binary hash,
hardware/session identity, warmup, time-series RSS/CPU and attributed mapped pages.

```sh
python3 tests/performance/bench_idle.py \
  --app build/release/putgd --cli build/release/putgd-cli \
  --output /tmp/putgd-idle-unique --seconds 300
```

Retained comparisons are exported in
[the structured memory record](evidence/2026-10-08-resident-memory.json), including
failed candidates, workload parameters, binary/harness hashes and measured page
attribution. Replay with a fresh output directory on the same physical session;
do not substitute a shorter run or a hosted CI measurement. Raw local logs remain
in the private per-run directory and optimization journal. These empty-history
records do not certify populated-history or upload memory usage.

Use the owned-window observer and repeatable view/payload protocols in
[testing-memory.md](testing-memory.md) for new comparisons. Each reference and
candidate must use the same observer version. Preserve complete controlled JSON
and JUnit reports with their source/binary hashes; keep private configuration,
resident logs and payload directories out of CI artifact uploads.

Enable the non-installed capture driver with `cmake --preset release
-DPUTGD_PHYSICAL_BENCHMARKS=ON`, then build normally. On the physical Cinnamon X11
session, run:

```sh
python3 tests/performance/run_capture.py \
  --app build/release/putgd --cli build/release/putgd-cli \
  --driver build/release/putgd-bench-capture --output work/capture-run-unique --live-outline
```

Use a new output directory per attempt. The supervisor records exact binary hashes,
shortcut, warmup/sample counts, timeout, capabilities, owned operation outcomes and
logs; driver results add display geometry/scale and all 30 timing samples. Every
screen is covered by a labelled grey synthetic canvas, all selections are cancelled,
and the isolated profile is local-only. The driver waits for the fixture to be visible
on every head before each invocation, then independently checks owned selector
mapping and exact painted pixels. This avoids confusing compositor fade with a
failed capture. A completed run can still fail its 150 ms budget: retain that result.
For the current transparent selector, `--live-outline` draws a synthetic rectangle
and witnesses its first white edge on every head before cancelling with Escape.
This workload includes external drag injection and supports only scale-one heads
with a shared vertical strip. It differs from the historical shaded-first-frame
workload (retained as the default for replay) and proves neither final saved pixels
nor mixed-scale support. Record its witness mode with every comparison.
Run physical capture and memory measurements serially. Hosted virtual CI cannot
establish the physical budget; a suitable self-hosted desktop can run these commands
and retain only their controlled report directory. Never include credentials or VM keys.

## Settings, naming and payload ownership regressions

The standard CTest suite includes settings info-help coverage, fresh S3 drafts,
quiet autosave and explicit reload/retry feedback, in-flight reversion and Quit
flushing, old configuration compatibility, filename templates,
Unicode byte bounds, local collision preservation and actual signed S3 collision
fixtures. `queuedCaptureNameUsesOriginalActionTime` holds an earlier upload for
1,100 ms, then verifies the queued capture uses its original action timestamp.
Names and object candidates remain unchanged across explicit retry.

`decodedImagesReleaseBeforeBlockedUploadAndQueuedCancel` gives a synthetic 8×8
ARGB image an external-buffer cleanup callback. The callback must run before a
deferred provider completes, and queued cancellation must release the second
image while that first upload is blocked. The retry uses the same disk path and
SHA256 without reading the source again. `queuedImagesRespectByteBudgetAndCancellationReleasesCapacity`
sets a 256-byte limit and verifies admission, rejection without publication, and
capacity returned by cancellation. These are ownership/queue checks, not RSS
benchmarks. History checks release the displayed thumbnail on hide and restore
it lazily on show; metadata paging retains the 10,000-entry dataset.
`restoringHistoryKeepsActiveDeletionCancellation` holds the provider at a deletion
checkpoint, closes and reopens History, then confirms Cancel still reaches the
accepted deletion operation. The barrier is released and workers drained before
the temporary fixture is destroyed; no real remote object is involved.

The optional schema integration uses the existing locked MCP environment. It
starts a private offscreen production resident with an unsupported desktop
identity, reads actual defaults through the production CLI, and checks the
published JSON schema, custom patterns, old documents without optional fields,
and invalid tokens/budgets. It reads no desktop or cloud content:

```sh
PUTGD_APP="$PWD/build/dev/putgd" PUTGD_CLI="$PWD/build/dev/putgd-cli" \
  uv run --project tools/mcp --locked python -m unittest discover \
  -s tests/integration -p test_config_schema.py -v
```

`json_ref_lifetime_contract` scans `src/` and `tests/` for an `auto` variable that
keeps the `QJsonValueRef` returned by `operator[]` on a temporary from
`toObject()`, `toArray()`, `object()` or `array()`. `Settings::validate` had this
defect and crashed the Debian 13 resident on `settings.save`. GCC's ASan does not
poison full-expression temporaries, so the asan preset passed with the defect in
place. Read through `.value(key)` or `.at(index)`, or convert in the same expression.

Repeat the affected sanitizer contracts with
`payload_contract|archive_contract|upload_naming_contract|settings_contract|operations_contract|gui_settings|gui_history_view|collision_policy_integration`
as the CTest `-R` expression, using the ASan/UBSan settings in [build.md](build.md)
and the instrumented Qt environment in [thread-sanitizer.md](thread-sanitizer.md).
Keep separate JUnit/logs per attempt; a prior failure remains evidence even when
the final source passes. Native UI witnesses and the exact controlled inputs are
recorded in [the settings-refinement evidence](evidence/2026-10-08-settings-refinement.json); offscreen results establish no
compositor certification.
