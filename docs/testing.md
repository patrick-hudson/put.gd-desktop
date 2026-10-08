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
distribution and 30-day retention. This workflow has been authored; a remote run
has not yet been observed. Hosted CI covers contracts, not native compositor acceptance.

Each graphical `run` appends start/finish JSON events to `RUN/test-events.jsonl`.
Each attempt receives a unique ID and a preserved `RUN/tests/ID/` bundle, containing
`parameters.json`, results JSON/JUnit, package/runner identities, native display
layout, requested/actual hotkeys, timing, pre-action selection/window geometry,
synthetic artifacts/paste witnesses, and a SHA256 manifest. Root-level files remain
the latest attempt for interactive inspection; the per-attempt bundles do not overwrite
earlier failures. An interrupted attempt retains its start event.

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
