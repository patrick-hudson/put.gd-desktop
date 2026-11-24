# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### KDE resident memory baseline — 2026-10-10

KDE used the same clean resident experiment. Its standard development/release
builds, package and all 49 registered tests completed; its registry matches the
host exactly. GNOME's additional GJS ownership contract is conditional and absent
on KDE. The installed package and live resident match
`0.1.1-dev.57+g2a02118ffc7b`. Twelve native single-head Qt/GTK capture, clipboard
and cancellation cases passed, along with focused tray, native dialog/font
integration, notification, live background, Activity copy and normal view-helper
exit checks. Single-item Activity checks do not prove multi-item subset copy.

The complete native crosshair is confirmed by actual cursor alpha. The installed
QEMU's legacy VNC mask keeps only fully opaque pixels and exactly reproduces the
incomplete crosshair shown on the display wall. That infrastructure limitation
and the rejected original mask observation remain recorded; no cursor image was
substituted and no product or host QEMU change was made.

One five-minute populated post-local-native observation measured 139.42 MiB
peak/final RSS, 33.96 MiB PSS, 21.15 MiB private memory and 0.0667% CPU of one
core. All 61 samples passed coverage and exact process/family identity checks:
299.958 seconds, maximum gap 5.050 seconds, one resident, no UI/helper and a
normal generation-four helper exit. Independent recomputation verified counters,
attribution hashes and native/source joins. Measurement integrity and CPU pass;
the 40 MiB RSS target fails. PSS does not replace the requested RSS target.

The [bounded record](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-10-kde-source57-memory-baseline.json) and
[procedure](../testing-kde-memory.md) preserve deterministic parameters, commands,
hashes, failed attempts and workload qualifications. Current cold and post-S3
memory, remote History, full geometry, physical KDE, canonical clean-checkout
lab replay, MCP and release acceptance remain open. No KDE optimization candidate
has been implemented. The retained Cinnamon and GNOME implementations are unchanged.
