# Submitted editor uploads waiting in the queue (#559)

Code: [PR #562](https://github.com/patrick-hudson/put.gd-desktop/pull/562).
Source: `9f382fd0b07d7c1237d8a47a25b87ee3d6129535`, tree
`368c787617a047d52087a16aefd328503aabf147`, commit count 1254.

## Lifecycle and consumer audit

The editor handoff already set the in-memory operation to `acquiring/queued`,
but did not save that state until an upload worker became available. The journal
still said `editing`. `operation.inspect` and `operation.recent` overlaid live
phase/progress onto that journal record, so History received `editing/queued`.

The fix saves `acquiring/queued` before enqueueing. The image is still an
in-memory sanitized PNG, so claiming `stored` would be premature. History tests
queued phase before editing/acquiring state, including the stale combination.
The tray already uses phase before state and continues to say `Queued`; its
regression test verifies both `editing/queued` and `acquiring/queued`.
`Operation::activeState` already includes acquiring, so cancellation and update
admission remain active. No new state or provider contract is introduced.
Recovery treats the submitted-but-not-stored handoff as interrupted acquisition;
it cannot resume an artifact that was never committed. An open unsubmitted
editor retains its existing `edit_interrupted` recovery behavior.

## Regression coverage

- `submittedEditorWaitsOutsideEditing`: real editor helper, one occupied worker,
  journal and CLI inspect/recent assertions, then drain or queued cancellation.
- `queuedEditorStatusTakesPrecedence`: all supported active queued states,
  including the reported `editing/queued` case; a live editor keeps its guidance.
- `queuedEditorUsesPhaseRatherThanEditingState`: tray phase precedence.

## Graphical scenario

The [driver](lab/queued_burst.py) uses the production resident, editor and CLI,
the existing synthetic loopback S3 wire fixture, and the actual native desktop.
One upload is held at the fixture's PUT barrier while five more editor uploads
are submitted. It saves History while those five are queued, cancels the final
operation, releases the barrier, and checks five completions plus one cancellation.
It saves protocol/journal witnesses separately from native VNC pictures. The
fixture is not evidence of AWS availability. Memory snapshots are diagnostics,
not release memory budgets or leak proof.

Each run requires the physical desktop lock, an active visible wall tile, a
short private runtime with socket-path preflight, and the full-screen lab cover.
Pictures pass the host-side cover gate before export. The driver exits its own
resident, fixture and cover; the desktop lock's separate cleanup check governs
release. No compositor, login session or existing user profile changes are part of the physical scenario. Mint briefly installed and then purged `gir1.2-gtk-4.0`; the application ran from a private install prefix and no system put.gd package was installed. GTK 3 was sufficient for the first successful Cinnamon run.

## Results on the exact source

All builds use C++20, the dev preset and the official AWS SDK 1.11.907. Source
provenance was supplied from the clean git archive; `dirty: null` in that mode
means the extracted build directory is not a Git checkout. Binary SHA-256 values,
OS, kernel, shell process/start identity, session ID and Qt version are in each
linked result. Qt remains dynamically linked to the distro libraries.

| Environment | Build / run | Queued, cancel, drain and saved visual |
| --- | --- | --- |
| Kubuntu 26.04 Plasma / native Wayland, k72 | `~/work/i559/build/dev`, `/tmp/q559k3`, Qt 6.10.2 | [PASS](k72/result.json), [queued](k72/02-queued-history.png) |
| Fedora 44 GNOME / native Wayland, f44 | `~/work/i559/build/dev`, `/tmp/q559f4`, Qt 6.11.2 | [PASS](f44/result.json), [queued](f44/02-queued-history.png) |
| Debian 13 Plasma / native Wayland, d69 | `~/work/i559/build/dev`, `/tmp/q559d5`, Qt 6.8.2 | [PASS](d69/result.json), [queued](d69/02-queued-history.png) |
| Mint 22.3 Cinnamon / native X11, m74 | `~/work/i559/bin`, `/tmp/q559m2`, Qt 6.4.2 | [PASS](m74/result.json), [queued](m74/02-queued-history.png) |

Mint ran an Ubuntu 24.04 build made on f44 in the existing
`localhost/putgd-i410-u24-deps:latest` container, image ID
`d32d68bed7264c98aaa75fe62569e8a4213688cbdbc90e3f3b96cb588d9b91e3`.
Its private install archive SHA-256 is
`79fa3f0c84b16239cd2bc89f5d0f950478b484735f3c3a61e15d3b4d48446bc2`.
A first identical Cinnamon burst also passed; its [receipt](m74/first-pass-result.json)
is retained. No physical host was logged out or reserved during a peer's quiet slot.
Each host directory includes its acquire, cleanup-check and checked-clean release
receipts. The shared desktop wall remained active throughout.

The full k72 dev build passed (804 Ninja steps). The six focused suites passed:
`editor_job_integration`, `edit_operations_integration`, `gui_tray`,
`history_contract`, `operations_contract`, and `gui_history_view` (180.57 seconds).
The [build/test receipt](tests/build-test-receipt.json), [CTest log](tests/ctest.log)
and [JUnit](tests/results.xml) identify the exact executables and commands.
Offscreen tests establish core/widget behavior only.

### Capture admission and retained failures

All exported app frames passed the existing 50% cover / 90% on every edge gate.
The selected rectangle first passed 98% cover admission. No rejected raw frame
was opened or exported. Failing preflight attempts retain numerical gate output;
where admission permitted a covered-only picture, that picture is retained too.

- k72 attempts k1/k2: driver configuration-directory error, then an oversized
  own window failed the cover edge gate. k3 used PID-scoped KWin placement
  (1100×700 at 410,130) and crop 0,0,1920,1000. The script was loaded, run and
  unloaded through KWin's scripting D-Bus API and matched only owned helper PIDs.
- f44 attempts f1/f2/f3: full-output cover was 95.93%, a shell-identification error
  selected the wrong crop, and keyboard sizing failed the frame gate. The
  successful crop is 0,180,1920,900 with 100% pre-admission cover; an app-only
  `QT_SCALE_FACTOR=0.7` fits the native Wayland window. Temporary notification
  probing restored the previous `show-banners=true` setting. No unsafe GNOME
  Eval setting was enabled. These images are bounded-region evidence, not
  full-output/default-scale performance evidence.
- d69 d1: private runtime required an absolute inherited Wayland socket path;
  the helper could not open a window. d2/d3/d4 safely refused cover admission
  before launching the app. A persistent right-edge obstruction was located
  numerically in memory. d5 used crop 0,0,1540,1000 (100% pre-admission cover)
  and only its own KWin windows at 270,130,1000,650. A duplicate d2 launch was
  rejected by the new-directory guard before mutation. The owned d1 startup
  notification was closed during cleanup.
- Initial native build setup needed the documented SDK/dependency symlinks and
  Fedora's `lib64` runtime path. The Ubuntu container's own generated build
  directory needed its owner restored after mount creation. These were harness/
  build-environment corrections, not changes to product source or shared SDKs.

The private bulk evidence remains under `work/559/` in the issue worktree and
on each host under its named short run. The committed driver snapshots preserve
reproduction context, including the successful f44 and k72 versions.

## Outstanding gates

Ubuntu GNOME VM acceptance and native GNOME/X11 plus KDE/X11 coverage are still
pending. The four physical runs do not substitute for those rows. No full release
matrix, physical memory-budget campaign, package-format acceptance or U9 walkthrough
is claimed. Code PR CI passes on `9f382fd0b07d`; Greptile review and the remaining
applicable acceptance gates must complete before merge. This is not release
publication authorization.
