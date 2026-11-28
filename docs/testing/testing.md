# Testing

Build and run repository tests with `cmake --preset dev`, `cmake --build --preset dev`,
and `ctest --preset dev`. See [build.md](../release/build.md) for pinned SDK provisioning and
sanitizer builds. Qt offscreen tests prove core/widget contracts; isolated Xvfb checks
prove X11 protocol behavior. Neither establishes real Cinnamon or Wayland support.

Run host lab contracts with `python3 tests/desktop/test_lab.py`. These cover private
QEMU command construction, QMP protocol/event handling through an actual UNIX socket,
native session rejection, TCG evidence classification, locked ISO identity, independent
pixel/paste comparisons, and actual/public byte hash/URL-paste mismatch rejection.
The socket peer is synthetic and is expressly not compositor evidence.

Use [vm-lab.md](../lab/vm-lab.md) to provision a golden Cinnamon guest, install the development
DEB and execute a named scenario. The single command `python3 tools/lab/lab.py run
--run RUN --scenario region` emits machine-readable results/JUnit and inspectable
synthetic evidence; use the other named capture/clipboard scenarios and configured
S3/history options as documented. Every graphical pass needs actual saved/public bytes
and independent native paste, plus external QMP witnesses. Never call a profile supported
from portal discovery, offscreen mocks or uploader-window screenshots.

Physical Cinnamon acceptance and the GNOME/Plasma native Wayland/helper matrix are still
required. Lab acceptance additionally needs two independent golden-image runs,
wrong-result and timeout cleanup witness evidence, and the required multihead layout
cases. Agent acceptance requires a fresh-agent walkthrough; release acceptance
requires package lifecycle proof.
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
[the structured memory record](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-resident-memory.json), including
failed candidates, workload parameters, binary/harness hashes and measured page
attribution. Replay with a fresh output directory on the same physical session;
do not substitute a shorter run or a hosted CI measurement. Raw local logs remain
in the private per-run directory and optimization journal. These empty-history
records do not certify populated-history or upload memory usage.

Use the current [memory strategy](../design/memory-strategy.md) for new comparisons. The
[earlier observer/view/payload record](../history/testing-memory.md) explains the
historical experiments; its three-window view workload does not describe today's
main window. Each reference and tested build must use the same observer version. Preserve complete controlled JSON
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
hotkey, warmup/sample counts, timeout, capabilities, owned operation outcomes and
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

The standard CTest suite includes old configuration compatibility, filename
templates, Unicode byte bounds, local collision preservation and actual signed S3
collision fixtures. The Settings form's help, autosave and reload feedback, capture
naming in the operation queue, image byte budgets and History thumbnail release
and paging are no longer tested; they went in the second round of
[removed tests](removed-tests.md#second-round).

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
defect and crashed the Debian 13 resident on `settings.save`. GCC and clang both
poison the dead temporary, but only exported functions in the uninstrumented distro
libQt6Core read through the reference, so the asan preset passed with the defect in
place. The CI `sanitizers` job loads an ASan-instrumented QtCore for that reason
([ci.md](../release/ci.md#jobs)). Read through `.value(key)` or `.at(index)`, or convert in the
same expression.

Repeat the affected sanitizer contracts with
`payload_contract|archive_contract|upload_naming_contract|settings_contract|operations_contract|gui_settings_[a-z]+|gui_history_view|collision_policy_integration`
as the CTest `-R` expression, using the ASan/UBSan settings in [build.md](../release/build.md)
and the instrumented Qt environment in [thread-sanitizer.md](thread-sanitizer.md).
To run every test under ASan/UBSan, as the nightly workflow does for each new nightly
source, build
the `clang-asan` preset and run `ctest --preset clang-asan-all`
([ci.md](../release/ci.md#every-test-under-the-sanitizers-nightly)). The `clang-asan` test
preset alone runs only the core contracts the per-pull-request job runs. Both presets read
`tests/sanitizer/lsan.supp`, which may hold only leaks inside a third-party library, each
with its reason.
Keep separate JUnit/logs per attempt; a prior failure remains evidence even when
the final source passes. Native UI witnesses and the exact controlled inputs are
recorded in [the settings-refinement evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-settings-refinement.json); offscreen results establish no
compositor certification.

### Settings GUI area suites

The Settings GUI tests run as five CTest entries: `gui_settings_general`,
`gui_settings_capture`, `gui_settings_destinations`, `gui_settings_credentials`
and `gui_settings_advanced`. They keep only the guards against silent, costly
faults: secrets kept out of the config, saved view state and credential hints;
destination edits and routes kept apart when destinations are reordered or
removed; opening Settings never rewriting the config; and a diagnostics package
uploading only when asked. Shared fixtures live in `tests/gui/SettingsGuiFixture.h`.
Run them with:

```sh
ctest --preset dev -R '^gui_settings_'
```

Historical evidence retains the original `gui_settings` name and runtime.

### Custom uploader suites

The custom HTTP uploader (#1361) is tested in four layers:

- `http_uploader_rules` (unit, no network or files): response values, `.sxcu` import
  including legacy fields and text bodies, the settings rules, which hosts and
  transports are allowed, and that stored values stay names until they are needed and
  out of what is shown or recorded.
- `http_uploader_integration`: the production provider and `putgd-http-worker` against
  the scripted loopback receiver `tests/fixtures/http_uploader_server.py`. It covers the
  multipart, JSON and header reply styles, where the token travels, refusals, lost and
  late replies, redirects that keep credentials on their host, deletion that the server
  did not do or that belongs to another upload, and History with an explicit retry that
  may leave two copies. The receiver also plays a careless or hostile server: it repeats
  the credential it was sent in the link, the thumbnail or the deletion URL, echoes a
  key out of a stored JSON body or around a control character, names a stored value in
  a deletion that is redirected to another host, and cuts its reply short. Imports run
  one at a time and never take another destination's keychain entry.
- `customUploaderImportKeepsItsTokenOutOfSettingsAndCheckpoints` in
  `gui_settings_destinations` drives the form: import, masked rows, store, and the
  token's absence from `config.json` and every checkpoint.
  `CustomUploaderResidentTests` in `tests/integration/test_mcp.py`
  does the same through the MCP helper against a real private resident
  ([mcp.md](../automation/mcp.md#headless-contract-checks-and-remaining-acceptance)).
- Real receivers: [HTTP receivers](../lab/http-receivers.md) runs Zipline, Chibisafe and
  XBackBone in containers on a builder and drives a resident against the `.sxcu` file
  each of them generates. This layer is a lab run, not a CTest entry.


The SFTP destination (#1871) is tested in three layers:

- `sftp_rules` (unit, no network or files): the Server field's paste forms and refusals
  (a password in the address, a leading hyphen, a bad port), the saved settings' rules,
  folders, links and `host:/path`, passwords left out of addresses and user parts out of
  links, the optional password and passphrase, askpass answering only ssh's exact local
  password and passphrase prompts as whole strings, literal host tokens, host-key fingerprints as
  `ssh-keygen -l` prints them, and the bounds of the SFTP reply parser.
- `fuzz_sftpwire` ([fuzzing](fuzzing.md)): the same parser on arbitrary bytes.
- `test_sftp_live`, opt-in and not a CTest entry: the production provider, `putgd-sftp-worker`,
  `putgd-sftp-askpass` and the machine's own `ssh` against the storage lab's real OpenSSH
  server (`tools/lab/storage_servers/servers.py`, #1877). Build it with
  `cmake --build build/dev --target test_sftp_live`, source the lab's
  `storage-servers.env` on the machine that runs it (with its client key, password and
  passphrase files and the lab CA readable there), and run it; it skips without that
  file's variables. Two `~/.ssh/config` Host aliases for the key user, one with the plain key
  and one with the passphrase key (`IdentityFile`, `IdentitiesOnly yes`), named in
  `PUTGD_SFTP_LAB_ALIAS` and `PUTGD_SFTP_LAB_PP_ALIAS`, cover key files and `ssh -G`. It
  covers an unknown host key shown and trusted only by its exact fingerprint (put.gd's
  `ssh/known_hosts` private, its folder closed to others), then a full
  Test with the link fetched over the lab's HTTPS location and compared; the password user
  with no, a wrong and a misplaced secret; an agent of its own; a cancelled 256 MB upload
  whose partial file is removed (and says so); upload, collision, the recorded `known_hosts`
  line in the receipt, an unknown upload not reconciled without its own evidence, a deletion
  from a new session proved against the recorded line alone, a record whose recorded key isn't
  the server's refused as `sftp_server_changed` with the file left in place, deletion and a
  changed file's refusal;
  the passphrase key with and without its passphrase (and a password refused at that
  prompt); the read-only and full folders, a missing folder and Create folder, and the server
  without the SFTP subsystem. The host-key rotation (`servers.py rotate-hostkey`, then
  `known-hosts` to pin the new key again) is a manual step of the same run: the changed key
  in put.gd's file and in the user's own `known_hosts`.
Whether a reply is whole is tested without relying on timing (#1393). The rows of
`aReplyCutOffAfterItsStatusLineIsUnknown` cut a reply by an unfulfilled
`Content-Length`, a reset in the body, a chunked reply without its last chunk and a
reset inside a chunk, and expect one outcome and one code for all of them.
`aShortReplyThatWaitedWhileTheFileWasSentIsUnknown` makes the receiver send a short
reply while the file is still arriving and close afterwards. That is the order of
events in which Qt reports a request it could not send again (error 205) where it
normally reports a closed connection, and in which a short reply holding a whole link
was once recorded as an upload; the rule produces it on every run, where load produced
it in about one run in forty. `lostAndLateRepliesAreUnknown` checks that an upload
with no body is sent once when its reply is lost, is not failed by the connect timeout
while its reply is slow, is unknown when no reply comes, and is a plain failure when
the server is absent. Run the suite under contention with the builder stress lane,
`tools/ci/stress_suite.py`, after a change to the worker's reply handling.

The suites cannot show on one machine the refusal of a returned address that is, or
resolves to, this computer, because the scripted receiver is on loopback and a loopback
destination may be answered with loopback addresses. The tables that covered it went
in the second round of [removed tests](removed-tests.md#second-round).

### Nextcloud and WebDAV suites

- `dav_rules`: Nextcloud and WebDAV address and settings rules (#1872): pasted Files and share
  links reduce to the server (route suffixes only), plain HTTP only where allowed, per-segment
  URL encoding, share token redaction, sign-in pages kept on the server and strong ETags only.
- `dav_integration`: the production provider and `putgd-webdav-worker` against real Nextcloud and
  WebDAV servers named by `PUTGD_TEST_NEXTCLOUD_*` and `PUTGD_TEST_WEBDAV_*` (the lab servers in
  `tools/lab/storage_servers/`); each case skips when its server isn't named. It covers Test,
  upload, collision, share link fetched as a visitor, a changed file refused on delete, 60 MiB
  chunked upload and cancel, an untrusted certificate, Login Flow v2 sign-in and sign-out
  (`PUTGD_TEST_NEXTCLOUD_SIGNIN_URL_FILE`, completed by a browser), a missing WebDAV folder and
  Create folder, a wrong password, and with `PUTGD_TEST_NEXTCLOUD_EXPECT` a full quota, public
  links off, an enforced link password and an enforced expiry.

## Scrolling capture suites

A wrong stitch looks like a valid image, and the feature sends input to a live desktop, so
these suites compare pixels and check what is injected. None of them is desktop proof. The
design is in [scrolling capture](../design/scroll-capture.md).

- `scroll_tracker_contract` (`tests/unit/test_scroll_tracker.cpp`, on scenes of
  `tests/fixtures/ScrollSynthetic.*`; no display): the matching engine. A frame placed at another
  offset than the page's own, a pinned part in the image more than once, a row lost or doubled at a
  seam ([#1527](https://github.com/patrick-hudson/put.gd-desktop/pull/1527)); a page's last lines above
  a blank tail under a header and a footer placed where the page is, never unmatched.
- `scroll_session_contract` (`tests/unit/test_scroll_session.cpp`, on the synthetic application in
  `tests/fixtures/ScrollSynthetic.*`, with the helper's spool file; no display): the session loop. A
  person's scrolling as a script (slow, fast, still, back up, to the end) gives the exact page and not
  one injected event; auto-scroll from the top and from the middle with the way to the top gives the
  whole page and ends by itself with no frame skipped, each wheel step a short visit of under half
  the frame; a switch from the user to auto-scroll, a pause by the mouse and auto-scroll again give
  one exact image with nothing injected while the user drives or has the mouse; a jump past the screen
  is lost and found again on the way back, and Done while lost keeps the image and says it ended lost;
  under auto-scroll, a jump into the page's end is found by the steps back with the image exact, and
  a jump far past the image (on text, and on plain stretches) is handed back lost, found again when
  the user scrolls back, and finished by Auto-scroll as one exact image; Done after the stream went
  stale (the same picture again and again while the page moves) keeps the rows the page shows; a
  blank stretch taller than the region stops auto-scroll without calling it the end of the page;
  one frame shown far up under auto-scroll is not taken for the user scrolling (a person's scrolling
  up still is); the tracker's expected offset stays within what a step owes when its first frames
  show the page still; a source that stalls in the first step for 150, 1,700 or 3,500 ms ends `PageEnd`
  with `reachedEnd`, every row of the image byte for byte the page's and the distance per notch
  within 10% of the fixture's; a first step still moving at its time limit ends `PageEnd` with
  every row byte for byte, and with no distance per notch or one within 10% (#1535); a blank
  stretch under a pinned header and footer is not called the end; the
  end of a page whose last lines sit above a blank tail under a header and a footer is reached, not
  lost; the way to the top over a strip that takes no wheel under the first wheel point still starts the
  image at the page's top; the cap and the idle bound end it with the image kept, and the idle bound
  in a capture where nothing ever moved ends it like Cancel; Done keeps an exact image and Cancel discards
  it in every state. It is in the contract lists of `cmake/TestPolicy.cmake`.
- `scroll_service_integration` (`tests/integration/test_scroll_service.cpp`): `ScrollCaptureService`,
  `LocalServer` and the production `putgd-cli` against a fake backend. An image finished beside a
  Cancel is never admitted (`cancelInFlightDecides`); the driver, `activity`, `paused`, `placed` and
  `skipped` reach an agent, `scroll.autoScroll` and its old names work, Done keeps an image that is not
  truncated and Cancel discards (`drivenCaptureEnds`); a capture forgotten until the idle bound ends
  cancelled with its note, nothing admitted, the Shell's ring and pointer watch gone
  (`forgottenCaptureEndsLikeCancel`); a PNG that does not match its descriptor is never
  admitted (`corruptOutputFails`); every end reason's promise is checked and a helper that breaks one is
  refused (`endReasons`), including `foundAgain` counts and recovery notes appended to existing notes. Recovery preserves `endReason` and `truncated`; the notice is `scroll_found_again` unless another warning already applies.
- `scroll_capture_x11` (`tests/integration/test_scroll_capture_x11.cpp`): the real `putgd-record` on a
  private Xvfb display against `tests/fixtures/scroll_page_fixture.cpp`. An XInput 2 listener shows that
  while the page scrolls by itself, as under a person's wheel, not one event is injected and Done keeps
  the exact page; that auto-scroll left alone runs to the end with every row exact and finishes
  `end_of_content`, or `done` with `framesSkipped` above 0, accepted whatever the reason for
  the skips (on a starved runner the tracker was seen to skip a frame it could not place among the
  ruler's look-alike rows, #1535); either way it requires `reachedEnd` and `scrolledBy` `putgd`,
  and it checks the image's height and samples the ruler's pattern at the cell centres of every line; on failure it prints the result (for a
  capture that never ended, its last progress), the helper's `scroll: ended` line and every time
  auto-scroll let go of the wheel, from a debug log it turns on; that under auto-scroll only wheel steps and pointer motion are injected, nothing after the
  Done or Cancel reply, and, with the test's pointer warp repeated until the capture reports paused, nothing injected from then until auto-scroll is on again,
  and the pointer is put back; the end of the control pipe stops injection at once. The user-scroll
  capture also checks its own source rate: asked for 60, and still 60 with 48 to 62 delivered when it
  kept up. On every build, a fallback must end at 30 frames a second, give a reason past its
  threshold (the source sent under 48 frames a second, or the loop missed 25 to 100% of the frames),
  and report more than 0 and at most 62 delivered. That is enough on a sanitizer build. On any other
  build the helper's thread times must also show a busy host (#2099):
  - the thread that fell behind used less CPU a frame than a 60 fps interval (twice that for
    unoptimized code, the dev preset);
  - when the loop caused the fallback, its CPU time plus its wait for a CPU, per frame it took, is at
    least 60% of a 60 fps interval;
  - the loop's or the grab's thread waited for a CPU for at least a tenth of the time.

  On a non-sanitizer build, a fallback on an idle host fails. There is no
  compositor or window manager, so it does not establish Cinnamon behaviour.
- `tests/releases/test_appimage_notices.py`: the native LGPL 2.1 copy is owned by the real AppImage
  component spec.
- `gnome_ui_ownership_contract`: a frame shown with `beat: true` keeps the extension's 40 ms timer
  until the resident hides it.
- Desktop entries and packages: `record_identity_contract` pins
  `X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,org_kde_kwin_fake_input` and the first `Exec` token of
  `putgd-record.desktop`; `tests/unit/test_portable_appimage.py` pins the same list in AppRun's per-mount entry and
  that the AppImage bundles `libxcb-xtest.so.0`.

Whether captures come out right on real pages, the notices held before a capture, the Settings
controls and the MCP tools are left to desktop acceptance and the acceptance drivers since the second
round of [removed tests](removed-tests.md#second-round).

`test_scroll_capture_driver` (`tests/fixtures/scroll_capture_driver.cpp`) is a lab tool, not a test:
it runs one capture through the production backend and helper without a resident (`--driver
user|putgd`, `--auto-at-ms`, `--stop-after-ms`, `--stop-file`), and `PUTGD_RECORD_DEBUG_LOG` gives
the helper's timeline (every frame's result and hint, every wheel step, the source's frame rate;
never pixels). `test_scroll_tracker_replay` and `tools/lab/scroll_tracker_score.py` replay recorded
frame sequences through the tracker and score them against the browsers' own pictures.
