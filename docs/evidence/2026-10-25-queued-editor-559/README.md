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

Each physical run requires the desktop lock; every run requires an active visible wall tile, a
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
| Kubuntu 26.04 Plasma / native Wayland, k72 | `~/work/i559/build/dev`, `/tmp/q559k4`, Qt 6.10.2 | [PASS](k72/result.json), [queued](k72/02-queued-history.png) |
| Fedora 44 GNOME / native Wayland, f44 | `~/work/i559/build/dev`, `/tmp/q559f4`, Qt 6.11.2 | [PASS](f44/result.json), [queued](f44/02-queued-history.png) |
| Debian 13 Plasma / native Wayland, d69 | `~/work/i559/build/dev`, `/tmp/q559d5`, Qt 6.8.2 | [PASS](d69/result.json), [queued](d69/02-queued-history.png) |
| Mint 22.3 Cinnamon / native X11, m74 | `~/work/i559/bin`, `/tmp/q559m2`, Qt 6.4.2 | [PASS](m74/result.json), [queued](m74/02-queued-history.png) |
| Ubuntu 26.04 GNOME / native Wayland, VM on d69 | k72 private install, `/tmp/q559g1`, Qt 6.10.2 | [PASS](ubuntu/result.json), [queued](ubuntu/02-queued-history.png) |
| Kubuntu 26.04 Plasma / native X11, VM on d69 | k72 private install, `/tmp/q559xk1`, Qt 6.10.2 | [PASS](kde-x11/result.json), [queued](kde-x11/02-queued-history.png) |
| Ubuntu 24.04.5 GNOME 46 / native X11, VM on d69 | Ubuntu 24 private install, `/tmp/q559xg1`, Qt 6.4.2 | [PASS](gnome-x11/result.json), [queued](gnome-x11/02-queued-history.png) |

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

The Ubuntu VM used its own overlay of immutable golden
`563730cc1d929bd946d20cf30a9aecfe3c4ad40eb12ba7f9abcff1c461e342bb`,
run ID `80a2caa7-70dc-4c5f-8420-361ad1d54ca9`, KVM with 2 vCPUs and 6 GiB.
Its private installed tree came from the same k72 build; install archive SHA-256
`8cdbceb63ec467fee9f6e30a8be1a5e9bf884723a7a38b0f3c3f67c88cee3259`.
Standard guest packages `libxcb-damage0` and `liblayershellqtinterface6` supplied
missing runtime libraries. Its first boot attempt used the wrong existing
bootstrap identity and timed out; the corrected attempt used a fresh overlay.
Both overlays were retired by the guarded cleanup tool after verified evidence
export. No golden was booted or modified; VM metadata and cleanup receipts are retained.

### Clipboard scope limitation

The assertions above cover persisted queued state, editor closure, queued
cancellation and upload drain. They do **not** assert clipboard success. Fedora's
five completed uploads reported `copyStatus=failed`, `clipboard_unavailable` /
`GNOME helper cannot own CLIPBOARD`; no GNOME extension was installed or modified.
The early k72 driver reported `Native Wayland data-control result copying is
unavailable`; the corrected absolute Wayland socket in k4 produced five successful
copies and a saved `libqwayland.so` witness. Debian's completed uploads copied successfully; Cinnamon's result
copies were suppressed. These are retained in the final-operation witnesses and
are not reported as clipboard acceptance. This focused change does not modify
clipboard code or session integration.

The same Fedora failure is independently recorded on #561 source
`79fd0cb5ee9170580941b175b99d89c3921ed352`, which does not include #559:
[bounded receipt and binary identity](https://github.com/patrick-hudson/put.gd-desktop/blob/b9a25cf928df8035593717d9157418d50a470300/docs/evidence/2026-10-25-preflight-cancellation-561/final/measurements.json),
`fedora-wayland.retry`. Operation `1531d49e-99e5-4a42-8460-a30a4ee6c009`
completed/uploaded in `/tmp/i561f1` while reporting the identical
`clipboard_unavailable` error. The original retry receipt was verified against
SHA-256 `2cc9a665af504dc583c51bad9287841516bf4e45fd70e939e4bf257d8457994a`.
This establishes that the clipboard failure also occurs without the queued-editor
fix; it does not turn that clipboard result into a pass. No new run was performed.

### Capture admission and retained failures

All exported app frames passed the existing 50% cover / 90% on every edge gate.
The selected rectangle first passed 98% cover admission. No rejected raw frame
was opened or exported. Failing preflight attempts retain numerical gate output;
where admission permitted a covered-only picture, that picture is retained too.

- k72 attempts k1/k2: driver configuration-directory error, then an oversized
  own window failed the cover edge gate. k3 used PID-scoped KWin placement
  (1100×700 at 410,130) and crop 0,0,1920,1000. The script was loaded, run and
  unloaded through KWin's scripting D-Bus API and matched only owned helper PIDs.
  k3 did not pin/record Qt's platform plugin and is not native Wayland proof.
  k4 reran the same source with an absolute Wayland socket and explicit QPA,
  retaining `libqwayland.so` module witnesses and passing all scoped assertions.
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

## X11 guests and golden retention

The KDE/X11 guest used run ID `228b404f-8986-4da1-af2a-59b729e69e24`
and a fresh overlay of the new immutable golden
`5306a311c69220421d3efbbb8929300634c3812214edef3b9203b830ce300826`.
Its actual X11 session and Qt `libqxcb.so` witnesses are in the result. Its own
1100×700 window at 410,130 fit crop 0,0,1920,1000 at scale 1. Evidence was
exported and verified before guarded retirement at 05:18:07 UTC.

The GNOME/X11 guest used run ID `8ab70c4a-c666-437a-9b37-5da939109890`
and #491's separately provisioned Ubuntu 24.04.5 golden
`fff0b55cc2d09d403bea61dd17d46165994cd8a9d0c2d467c5883c7fb795e245`.
Its own window was 1100×700 at 410,280, scale 1, within crop 0,180,1920,900.
The saved login-session type is X11 and the loaded Qt module is `libqxcb.so`.
The original harness package lookup returned an empty Qt version; the same
run's production diagnostics supply [Qt 6.4.2](gnome-x11/runtime-version-witness.json).
The original result is preserved unchanged. Export archive SHA-256 is
`ad7eac864350cf58fbce40224f048c1aa750745d673176658fa52ed886a877f4`.
Guarded retirement completed at 05:25:33 UTC; its tunnel and wall entry were
removed afterward. Both X11 runs passed all scoped checks and copied all five
completed links. These guests did not replace any physical acceptance row.

The new KDE/X11 golden is retained on both d69 and f44 at
`/home/patrick/work/put.gd/work/lab-images/kde-x11-20261025-559/image.qcow2`.
Both have the SHA-256 above; the [Fedora verification](kde-x11/golden/f44-destination.sha256)
and [image check](kde-x11/golden/f44-destination-check.txt) confirm the replica.
It is standalone, with 32 GiB virtual capacity. Provisioning used an independent
overlay of the existing KDE golden, standard `plasma-session-x11`, `kwin-x11`
and `wmctrl` packages, and `plasmax11.desktop` for the disposable guest's SDDM
autologin. The approved `lab` / `putgd-lab` login was verified visibly before
sealing; the old base was unchanged. The provisioning overlay was retired at
05:18:49 UTC. The inherited old put.gd package remains in the golden, with no
resident running; acceptance used the exact private install described above.

For reuse, copy the [profile](kde-x11/golden-profile.yaml), use a fresh overlay
through `tools/lab/lab.py`, and allocate distinct SSH/VNC/wall ports. The existing
builder bootstrap identity is
`/home/patrick/work/put.gd/work/lab-bootstrap-d69/rec-ws/identity`; the golden's
`known_hosts` sidecar pins the guest key at port 22260 and must be remapped to
the new endpoint. Keep keys private. Never boot the sealed disk directly.
The S3 replica is still blocked by expired AWS authentication; both verified LAN
copies are preserved. No offsite-copy success is claimed.

## Acceptance scope and review gates

The targeted queued-editor scenario passes all seven mapped desktop/session rows
on the exact source above. No full release matrix, physical memory-budget
campaign, package-format acceptance or U9 walkthrough is claimed. Fedora's
clipboard limitation above remains outside this scenario's assertions.
Code PR CI passes on `9f382fd0b07d`; the [saved check receipt](tests/code-ci-checks.json)
records the selected jobs. Actual Greptile 5/5 review on the final heads and
final documentation CI remain merge gates. A successful check with zero comments
is not treated as a scored review. No release publication is authorized.
