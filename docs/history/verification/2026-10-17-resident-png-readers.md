# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Resident PNG readers — 2026-10-17

The [resident PNG receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-17-resident-png-codecs.md) records the
kept reader change `e9a4dac` and the dropped libpng writer. The writer prototype
passed the Mint dev suite 69/69 and 18/18 scenarios each on a Mint Cinnamon VM and
the physical Debian Plasma desktop. It saved only the 220-436 KiB of plugin files
there, so it was removed. The reader-only change passes its three plugin tests and
the affected capture, clipboard, GNOME, KDE and editor suites.
