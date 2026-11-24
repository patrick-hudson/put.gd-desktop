# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Published nightly physical native gates — 2026-10-31

Published `v26.1031.3384-nightly` / clean source `4d3840c6108c` was installed
on the real m74 Cinnamon X11, f44 GNOME Wayland and k72 Plasma Wayland desktops;
Aurora's AppImage stopped at native probe preflight before app launch. Local
outcomes passed five of six recipes on m74 and k72; guarded-browser proof,
physical GNOME window witnessing and Aurora Atomic probe admission remain
blocked (#1060–#1062). No app compilation or live destination was used.

m74's 10k fixture passed cold/closed CPU and memory, content-open and search
limits. Warm drawable paint p95 was 138.854 ms empty and 158.401 ms with 10k
(150 ms limit, failed 10k gate, #1063). The links cohort also failed the
empty-history drawable gate at 158.998 ms against 150 ms while uploading. History PageDown response upper-bound p95 was
101.779 ms (100 ms limit, #1064); observer overhead is included. Capture-to-link
p50/p95 was 814.546/947.144 ms, ungraded. Fifty actual native captures/uploads
and both slow-network cases completed; populated Copy image/video/delete
remain unassessed at the current AT-SPI boundary (#1066). Full results, samples, failed
observers, source/asset/session identities, annotated visuals and cleanup are
in [the physical native evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-31-native-pass/README.md).
#379, #1063 and #1064 are now closed; these failed observations remain the
historical results for the tested build. #44 stays open; this does not certify
stable bytes or complete agent acceptance.
