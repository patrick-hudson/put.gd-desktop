# Shortcut row-switch recording, issue #779

Code PR: [#783](https://github.com/patrick-hudson/put.gd-desktop/pull/783).
Issue #779 stays open. Merge this evidence-only PR after the code PR.

The previous action ignores chords and Backspace while a different keycap is held
under the mouse. A completed click starts the new action; Escape, deactivation and
an abandoned drag still restore bindings. The recording keycap exposes an
action-specific accessible description during preparation and readiness, and
returns to its idle description afterward.

## Exact identities

- Code head: `3907e2ebc069ffe0acf024f6eeccd18342185b5b`, tree
  `b663fdb8638d7a6c0095eb7cbce13749042340bc`.
- Base widget: nightly `fd92f8b218f8ba5d2178bcb1de3c4f925a9e013b` (#743).
- Placement: one Boat fork, `bx_w4wz6wub`, from `putgd-build-base-v2`; bare metal,
  large, 8 vCPU / 16 GB, TTL 14,400 seconds. Reused throughout and deleted afterward.
- Build host: Ubuntu 24.04, GCC 13, dynamically linked distro Qt 6.4.2, official
  pinned AWS SDK. Version: `26.1030.2479-dev+g3907e2ebc069`, clean Git provenance.
- Recipe source: `feat/surface-recipes-settings` at
  `a4a25c645ed59bd33584fd979e753b1c103d8493`, exported outside the build checkout.
- Native guest: Mint 22.3, Cinnamon/X11 (`X-Cinnamon`, Qt platform `xcb`), kernel
  `6.14.0-37-generic`, hostname `putgd-lab`; fresh overlay `c779`, run UUID
  `cc34e70d-51ba-42b8-a00b-bf4f25350f05`.
- Immutable golden SHA-256:
  `4a21566d5b741e55f349f4f07e1c687c99d1c99b5394ff0d18ed5c9e685693da`.
- Tested DEB SHA-256:
  `91649dc57e2a8cbb4fa904ecdd828a90a42f6c328e2004658ec14026c55ff79a`.

The guest's loopback VNC `5920` was forwarded over pinned SSH to workstation port
`6279`, tile **Boat Cinnamon shortcut 779**. The wall was observed connected and
the unlocked native desktop inspected before acceptance. Builds, tests and GUI
input stayed off the workstation and Patrick's physical desktops.

## Regression and full suite

The **final test source** fails against the base widget in all four held-switch
endings: completed switch, abandoned drag, Escape and deactivation. In each case
`Ctrl+Alt+Shift+8` changes the original region action. The accessibility test also
fails because the focused button only says “Change capture region shortcut.”
See [nightly failures](regression-nightly.log).

The fix passes [all 10 targeted QtTest checks](regression-fixed.log), including
#743's preparing/ready transition assertions, cancellation/failure handling, and
QAccessible role, focus and description checks for all six actions. The new
mouse tests use QWindow input inside a scroll area with status-driven row reflow.
These are widget checks, separate from the native guest evidence below.

**Full Boat CTest: 114/114 passed, zero retries, 122.78 seconds.**
[Summary](ctest-summary.json), [JUnit](ctest.xml), [log](ctest.log),
[ledger](flake-ledger.json), [exact command](ctest-command.sh).
The ordinary dev build uses the CI timeout/test scales of 3 and the existing
resource specification with parallelism 12. The observed test UID is 1001
(`putgd-ci`), with private 0700 XDG/TMP directories and the saved
[socket preflight](socket-preflight.json).

The final run puts **test scratch only** on a bounded 4 GiB tmpfs. Source, build,
SDK, packages and the native guest remain on their ordinary filesystems. No
assertions, timeout limits, production socket checks or performance harnesses were
changed. This is functional evidence, without physical memory, latency or storage
performance acceptance claims.

Earlier disk-backed runs and interruptions remain readable in
[attempt 1](ctest-attempt1-complete.log), [attempt 2](ctest-attempt2.log),
[attempt 3](ctest-attempt3.log) and the
[dedicated-account first pass](ctest-disk-ci.log) / [JUnit](ctest-disk-ci.xml).
That complete disk-backed pass had 112/114 passing; operations and Proton reached
the existing 360/90-second limits. The retry was stopped using its exact owned
process tree before changing scratch filesystems. The same binary passes both
on tmpfs. The scratch-latency follow-up is
[#792](https://github.com/patrick-hudson/put.gd-desktop/issues/792).

The initial test's fixed-settle assertion was rejected by hosted wait lint and
replaced with the scaled focus helper. A recipe overlay briefly dirtied tracked
tooling in the build checkout; that attempt was stopped, the tools restored and
the driver source separated before the final clean build. Neither attempt is
counted as acceptance. Hosted CI subsequently passed Linux 24.04, Linux 26.04,
Windows and contracts, with the required `CI` summary green; the CodeRabbit check
also passed. See the sanitized [check receipt](code-ci.json).

## Native Cinnamon checks

The unchanged `settings.shortcut_recording` recipe passes its six-action
Escape → record → second-click sequence, with `functional: passed`,
`visual: reviewed-ok`, no review flag and successful cleanup.
See its [receipt](recipe-receipt.json); 59 recipe frames are retained privately.
This does not claim an additional independent post-second-click native-binding
sample beyond the recipe's actual assertions.

The additional [held-switch probe](native-held-switch.json) starts region
recording, holds the window keycap, and sends `Ctrl+Alt+Shift+8` and Backspace.
Both saved values stay unchanged. Releasing starts window recording; the native
AT-SPI focused button description reads:

> Recording Capture window shortcut. Press one key with optional modifiers, then release all keys. Escape cancels.

The probe then records `Ctrl+Alt+Shift+9` for window, confirms its native binding,
and preserves region's `Ctrl+Alt+Shift+7`. Both fixture and target cleanup report
no errors. The observed resident reports the exact clean code head above.

The figures add captions, boxes and arrows to native screenshots. Raw frames
remain unchanged in the verified archive; [image provenance](image-provenance.json)
binds their SHA-256 hashes to the annotated files. Asset links are pinned to the
asset commit, independent of subsequent README edits.

![Held row switch preserves both saved chords](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/75c3c062d2ea8f62ae4901bbe1e27d5261c0fb97/docs/evidence/2026-10-30-shortcut-row-switch-779/held-switch-annotated.png)

![The new action receives the chord after release](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/75c3c062d2ea8f62ae4901bbe1e27d5261c0fb97/docs/evidence/2026-10-30-shortcut-row-switch-779/saved-action-annotated.png)

This proves the tested Mint Cinnamon/X11 workflow. No native GNOME/KDE or physical
hardware acceptance is claimed.

## Retention and cleanup

All **656 exported files** verified against the completion manifest before VM
retirement. The private archive is 219,255,427 bytes, SHA-256
`18c9aad1840e41685a5c717ddf586cafae383dc0a5ed2d15fd72154b73e670f5`.
It retains full reports, source scripts, raw QMP visuals, the tested DEB, build
configuration and identities. [Retention and restore index](retention.json),
[export verification](export-verification.json), [asset checksums](asset-sha256.json).
VM disks, SSH private keys, credentials and live cloud sessions are excluded.

[Guarded overlay retirement](overlay-cleanup.json) completed against the exact
verified manifest. [Sandbox cleanup](sandbox-cleanup.json) records the delete
request and subsequent absence from the all-state sandbox list; the named
snapshot is preserved. [Wall cleanup](wall-cleanup.json) removed only the owned
tile and tunnel, and the owned SSH agent was stopped. The default home traversal
permission and temporary mount were restored before deletion. No issue was
closed, PR merged, review request posted or PR watch started.
