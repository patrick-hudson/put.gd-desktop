# Shortcut recording — issue #9

Implementation branch: `codex/shortcut-recording`, based on `origin/main`
`2bb1b124368bc4beac8fa074ee2dc54ed080fe0b` (source count 140).

## Scope

All four action rows provide Record shortcut, Cancel and Clear. Escape always
cancels, including while native suspension is pending. Releasing all held keys
accepts one PortableText chord into the Settings draft; Save settings applies it.
Physical scan codes pair key-down/up across modifier-dependent symbol changes.
Modifier-only presses wait, auto-repeat is ignored, unsupported keys and multiple
nonmodifier keys preserve the previous draft with an explanation.

The transient helper waits for acknowledged native suspension over its private
control pipe. X11 releases real grabs, GNOME binds empty shortcuts, and KDE makes
its actions inactive. Dispatch is gated during recording. Completion, Cancel,
focus loss, helper exit/crash, timeout and failed suspension restore saved
bindings. A newer settings bind owns its outcome instead of producing a false
restoration failure. Actual desktop binding status remains separate from drafts.

The port preserves main's Qt Core residents, native tray/capture helpers, KDE
keyboard-layout normalization and selection-safe shortcut dispatch. KDE layout
changes during recording cannot re-register suspended shortcuts.

## Baseline correction

The initial implementation was mistakenly developed on the stale
`codex/fedora-build-server` branch at `dcc4a89`. That branch contained memory
checkpoint documentation but lacked the merged implementation. The accepted
memory changes merged into main in PR #2 (`0ef817b`). The user corrected the
branch choice; work moved to a new managed worktree from current main, preserving
the old branch's unrelated changes. Only the task-owned duplicate edits were
removed after a SHA256 guard verified the preserved patch.

The stale-branch 40-target test results and 45.3 MiB post-use VM sample are retained
privately under `work/shortcut-recording/` and `work/lab-runs/sc9c/` in the original
checkout. They do not establish current-main acceptance or recorder memory
regression. The initial native release-order failure and its correction remain
useful development evidence, not current-build acceptance.

## Current verification

Dev configure/build passed. The full prescribed dev CTest suite passed 57/57
registered targets (367.88 s). After adding the review's service-restart coverage,
the KDE ownership integration target passed again (8.30 s). This checks pending
pause/restore interruption, acknowledged-pause service replacement, restoration of
the latest saved chord, conflict errors, and component signal reconnection. Private
JUnit receipts: `work/shortcut-recording/main-dev-tests.xml` and
`main-kde-review-tests.xml` in the original checkout. `git diff --check HEAD` passed.

Both code-review runs completed: `20261012-220317-384c6d39` (UI/IPC/backend feature)
and `20261012-222850-c63ee07f` (current-main port), retained under
`/tmp/compound-engineering-1000/ce-code-review/`. They included the configured
independent Anthropic review with served model `claude-opus-5-5` (requested effort
high; actual effort unverified). Original findings #1/#4/#3/#5 were fixed inline:
keypad/modifier classification, physical release pairing, superseded restoration
handling, and real timeout/delayed failure ordering tests. The followup review's
only finding #1 was missing KDE owner-restart coverage, now added and passing.
No justified review finding remains unresolved.

Current Cinnamon native checks passed on the main-based package: Ctrl+1 waited
for the final modifier release, Escape preserved the prior chord, modifier-first
release recorded Ctrl+Shift+!, and no operation was dispatched during recording.
After restoring/saving Ctrl+1, it produced completed region operation
`e663f1fc-18fd-4ea8-81d8-69ad4fcd8b66`. QMP PNG witnesses and operation receipts
are retained under `work/lab-runs/sc9c/`, prefixed `main-recorder-`.

The matched clean-main post-use memory run passed: 61 samples over 300 seconds,
final/peak summed RSS 28,221,440 bytes (26.914 MiB), one resident, no helper or
escaped processes. Baseline region operation:
`bbed54e0-7e26-467b-89ea-860cc8d71c7a`. The recorder passed the same 61-sample/300-second check: peak summed RSS
28,217,344 bytes (26.910 MiB), final 27,803,648 bytes (26.516 MiB). All samples
contain one resident, zero UI helpers and zero escaped descendants. Peak difference
is -4,096 bytes; this supports no observed material recorder regression, not an
optimization claim. Structured results and raw samples are in
`main-baseline-memory/` and `main-recorder-memory/` beneath `sc9c`.
GNOME/KDE checks follow this passing Cinnamon gate. These VM diagnostics do not
establish physical cold-idle budgets.
The unchanged main release and recorder release use the same GCC 13.3, distro
Qt 6.4.2, official AWS SDK and CMake Release preset. Cinnamon functional and memory
checks precede GNOME/KDE expansion. All graphical interactions remain visible
through localhost VNC, use isolated overlays and local synthetic output, and
retain failed attempts. Mock/offscreen tests do not establish desktop support.

## Wayland native validation

The Cinnamon package could not install on Ubuntu/Kubuntu 26.04 because several
Qt distro package names differ. Failed attempts remain under `sc9g` and `sc9k`
as `initial-package-install-failure.json`. No dependency check was weakened.
A fresh `sc9kb` overlay of the provisioned KDE golden built the identical Git
base plus recorder patch with native distro Qt 6.10.2 and the existing verified
official AWS SDK. Its backing chain was inspected and left unchanged. Storage
preflight passed with a build-growth reserve. Native patch SHA256:
`47deebb9f8464fa74772b459fc96b37ce2b5967a54a37d7b78b5cbfe61cd8729`.

Both native release builds report `0.1.1-dev.140+g2bb1b124368b.dirty`; they are
explicitly uncommitted candidate builds. The recorded patch/file manifests
identify the tested source. Package SHA256 values:

- Cinnamon candidate: `b0470e340318b31f12adc0e5f077e100475208bd31e441a1f52c4e2797427747`
- Ubuntu/Kubuntu candidate: `17d9dc494db20f2d99e4a4da72a58b3103df8d6137315fc8b808775ef9a784b1`
- Clean-main Cinnamon baseline: `e6b83e8b47610cc3a4c1c1a3c10e5141eb7475aa31a647791bfc2b486de11528`

GNOME Shell 50.1 and Plasma 6.6.6, both using distro Qt 6.10.2, passed visible
Ctrl+1 recording, last-modifier release, Escape cancellation, no accidental
operation during recording, and successful native region capture after Save.
GNOME required a fresh visible login to discover the installed system extension;
its active path was verified as `/usr/share/gnome-shell/extensions/putgd@put.gd`.
KDE's transient pending-rebind status settled to the confirmed active binding.

Completed operation IDs:

- GNOME: `4c1ecd7f-39ec-4ece-9e6e-e01b34d97153`
- KDE: `d034ae04-30af-42ca-b75f-f18bed18290f`

Per-desktop PNG witnesses, interaction logs, package/runtime identities,
capabilities and operation receipts remain privately under `work/lab-runs/sc9g/`
and `sc9k/`. KDE passed 61 samples over 300 seconds: peak RSS 32,030,720 bytes
(30.547 MiB), final 31,891,456 bytes (30.414 MiB), one resident throughout,
zero UI helpers or escaped descendants. GNOME's corrected measurement also passed, as detailed below.

### GNOME launch correction

The first GNOME memory run used QApplication fallback: the SSH lab helper copied
GNOME Shell's initial environment, which lacked `WAYLAND_DISPLAY` after the fresh
login. The resident's allowlisted environment confirmed the missing value;
`systemctl --user show-environment` and the owned socket both identified
`wayland-0`. The fallback mapped GTK/QPA libraries and exceeded the 40 MiB target.
This failed attempt is retained, not treated as the optimized native route.
The corrected launch explicitly imports the observed `WAYLAND_DISPLAY=wayland-0`
without changing product code or the frozen memory sampler. GNOME functional and
memory checks must be repeated for that process before accepting the route.

The fallback measurement was stopped after diagnosis. Its raw samples/progress
and interruption record remain under `sc9g/fallback-memory/`; no complete harness
receipt is claimed for that failed attempt. SIGINT inherited from the detached
launcher was ignored; the verified sampler was then stopped with SIGTERM. The
resident exited normally through `ui.quit` before the corrected launch.

After visibly unlocking, the corrected Core resident repeated all recorder
checks and completed region operation `c0ef08d3-9c8e-4da3-bbde-4897cc220d5e`.
Its process mappings confirmed no GTK/QPA platform plugins. Corrected GNOME
witnesses and commands are prefixed `core-`; its new sampler output is
`/tmp/sc9core`, with the same 300-second / 5-second sampling protocol.

The corrected GNOME run passed 61 samples over 300 seconds: peak RSS 34,615,296
bytes (33.012 MiB), final 31,817,728 bytes (30.344 MiB), one resident throughout,
zero UI helpers or escaped descendants. Results and raw samples are preserved in
`sc9g/core-memory/`. All three native-route post-use windows remained below 40 MiB.

| Measured route | Five-minute peak RSS | Final RSS |
| --- | ---: | ---: |
| Cinnamon clean main | 26.914 MiB | 26.914 MiB |
| Cinnamon recorder | 26.910 MiB | 26.516 MiB |
| GNOME recorder, corrected native launch | 33.012 MiB | 30.344 MiB |
| KDE recorder | 30.547 MiB | 30.414 MiB |

This is shortcut-recorder acceptance, not full release certification: physical
cold-idle, live-S3, multi-monitor, all capture actions and the broader U9 walkthrough
were not repeated. All four recorder rows, clear/disable, modifier/repeat cases,
dirty Settings drafts, focus/closure/failure cleanup and unsupported combinations
have GUI/integration coverage; native checks exercised the region chord on each
compositor. Existing accepted memory and capture routes remain in the main-based
implementation. Post-campaign host and all guest storage reserves passed.
