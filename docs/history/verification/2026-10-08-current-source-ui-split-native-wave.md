# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### Current-source UI split native wave — 2026-10-08

The frozen build is `0.1.1-dev.41+g97ca3a521714`, full commit
`97ca3a521714e2717cedc1139976130627bfb8a8`. Its clean host Dev suite passes
41/41 and the locked MCP suite passes 3/3. GNOME and KDE each pass 40 guest Dev
tests; the host-only GNOME ownership contract requires Node, absent in those
guests. All three executables and packages have independent checksums. Older
app residents were stopped after identity and terminal-operation checks; the
three existing test overlays were reused without changing their golden disks.

Cinnamon/X11, GNOME/Wayland and KDE/Wayland each pass the current 12-case native
capture/clipboard matrix and four actual tray actions with the UI absent. All
four views open and close normally, leaving the original resident and tray alive.
Safe drafts, external-edit conflicts, Cancel, native folder dialogs, own-window
and own-modal capture exclusion, and real clipboard paste after helper exit pass.
On each desktop, an accepted two-file local save remained active after ordinary
UI exit, then completed with exact sizes and hashes: one 512 MiB streamed fixture
and one 23-byte file. This establishes local-work continuity, not slow S3 upload
continuity. Cinnamon's additional user-confirmed manual region is excluded from
scripted acceptance; its payload was never read, hashed or exported.

Quiet post-use VM observations each contain 25 family samples over 120 seconds,
with no helper/escaped child and stable operation/UI guards. RSS/PSS/private are
57.65/37.32/35.91 MiB on Cinnamon, 65.32/38.77/35.22 MiB on GNOME and
145.50/45.17/32.76 MiB on KDE. KDE's LLVM mapping contributes 48.28 MiB RSS but
5.98 MiB PSS; retain proportional/private figures alongside RSS. These are
post-capture/local-save observations, not matched historical VM savings or the
physical cold 40 MiB target. GNOME's successful sampler was followed by a host
launcher `Popen.check_returncode` error; retain it and the corrected future replay
driver. No timed sample was repeated to hide that error.

[Current-source receipts and raw replay evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-ui-process-native-memory.json)
distinguish these demonstrated subsets from full AE17. Real S3 publication and
precise deletion, native crash/version/typed-credential cases, the historical
GNOME readiness/focus concerns, mixed displays and remaining release gates stay
open.

The matched physical Cinnamon comparison passed all four serial runs, two per
build, with three Settings/History/Activity cycles each. Median final closed
RSS/PSS/private falls from 65.09/40.40/37.59 MiB to 45.04/25.40/22.89 MiB: savings
of 20.05/15.00/14.70 MiB. Retain the split for this measured closed-state benefit.
Maximum sampled open RSS/PSS is higher: 120.84/55.01 MiB versus 73.19/44.49 MiB.
Four repeated Settings reopens per build have mapped-readiness medians of
332.68 ms versus 100.92 ms. These include observer overhead and do not certify
completed painting or p95 latency. The separate 300.605-second cold family run
measures 44.390625 MiB peak RSS and 0.00998% of one core; original idle budgets
pass and the 40 MiB target fails. Native fonts/dialogs remain enabled. Raw
commands, complete time series, grouped latency events and a measured decision
are retained in the linked current-source record.

A four-run standard CMake/Ninja Qt styling cost probe finished once, without
product changes or native-font/dialog overrides. Settled full-versus-native RSS
differs by 0.63 MiB, with 0.19 MiB immediate full-style growth. Preserve custom
styling: this five-second temporary-tray diagnostic does not establish product
savings or a route to 40 MiB. Exact source/binary hashes, stage timing, warnings
and replay instructions are retained separately under the physical evidence.
