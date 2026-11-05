# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### Latest native and physical evidence (2026-10-08)

The actual GNOME 50.1 Wayland guest passes all six local capture/clipboard workflows,
including independent GTK 4.22 native Wayland paste for text, images, and files.
Escape, right-click, and zero-area cancellation produce no artifacts and preserve
the independent clipboard source. Helper disable/re-enable during selection settles
the interrupted operation and restores all four bindings; disabled per-action
diagnostics now report unbound rather than retaining stale bound status.

The locked official MCP 1.30.0 helper was exercised over actual stdio against the
installed GNOME production CLI: discovery, content-free inspection, revision-checked
configuration updates/conflicts, clipboard invocation, operation identity, and
history parity pass. This does not complete the Cinnamon, fresh-agent, or remote
upload/deletion acceptance gates.

The physical Cinnamon release resident measured 84,783,104 bytes RSS (80.9 MiB)
and 0.009995 percent of one core over 300 seconds, with empty history and no
capture/network invocation. The original idle budgets pass for that scope. The
user requested approximately 40 MiB; optimization and populated-history/capture
performance checks remain pending. The native KDE Qt 6.10/LayerShell build and
25/25 tests pass in Kubuntu. All six named single-head local KDE workflows now
pass with independent Qt paste: exact 80×60 region, independently queried
640×448 active-window frame, full 1280×800 desktop, text, image, and two files.
Native checks also prove saved shortcuts rebind and a foreign shortcut is not
stolen. The region check caught fractional-pointer rounding; the corrected result
has identical capture/paste hashes. Each new attempt retains parameters, JUnit,
JSONL events, witnesses, and a checksum manifest. This is partial KDE evidence;
mixed-scale/multiple-display, GTK, cloud, and lifecycle gates remain.
KDE Escape cancellation also passes with no artifacts and preserved synthetic text;
right-click and zero-area checks remain pending.

The refreshed host standard Debug suite passes 25/25 in 46.79 seconds outside
the process sandbox. A sandboxed attempt could not open local test sockets/Xvfb
and is not a code regression or a passing acceptance result.

The second UI pass removes slogan headers, compacts action/folder/destination layouts,
and passes six affected host tests (16.38s) and four native KDE GUI tests (7.14s).
Installed single-scale witnesses cover all settings sections and populated local
history/activity. Standard desktop icon installation fixes the taskbar icon; the
window-decoration glyph still needs investigation. Simple setup, human-readable
size controls, scale/error/confirmation acceptance remain pending.

A fresh independent agent exercised fixed production CLI/official MCP metadata
inspection, revision patches/conflicts and deliberate configuration-error recovery
in an isolated offscreen process. Its structured record reports14CLI calls,7MCP
requests and12discovered tools, with zero operations/provider calls. This is partial
agent evidence; real Cinnamon, build and lab walkthrough gates remain pending.
