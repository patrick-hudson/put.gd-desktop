# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Recording session contract (#410) — 2026-10-24

The [companion report](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-24-recording-session-410/README.md) and
[JSON ledger](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-24-recording-session-410/summary.json) bind clean
`3440ab9c` to 108/108 Dev tests, 5/5 MCP tests, three relevant whole unsuppressed
ASan/UBSan suites and the actual product probe, plus required code CI.
Cinnamon X11 and GNOME Wayland guests and physical Debian KDE Wayland passed
six changed-behavior cases and separate 180-second resident RSS/PSS windows,
with reviewed saved visuals, graceful helper exits and verified cleanup.

Original Mint `c5bb1171` full-run/idle evidence stays on that private-mode archive;
corrected `fb19e0c7` has the identical nine binary hashes and a fresh three-case
package smoke/access check. GNOME uses corrected `73faf5f6` with the active
version-2 extension. Native physical GNOME prerequisites remain blocked; guest
proof is not physical or full-release acceptance. Early numeric-only memory
cover failures and baseline missing binary hashes remain explicit. The separately
identified #556 Btrfs read-only capacity proof establishes storage only.
Merge recording #551 and prerequisite #556 before companion #550.
