# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Stalled package installs (#467) — 2026-10-23

Code: [#498](https://github.com/patrick-hudson/put.gd-desktop/pull/498), head
`1926941a`, all CI jobs green
([run 40008537011](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40008537011)).
Policy: [CI coverage, stalled installs](../../release/ci.md#stalled-installs). Evidence:
[`records/2026-10-23-package-install-stalls-467/`](https://github.com/patrick-hudson/put.gd-evidence/tree/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-23-package-install-stalls-467).

- **Before.** Nightly run 39851369595 (`eab13a18`): `packages / appimage`
  hung for 93 minutes after `Get:5 https://archive.ubuntu.com/ubuntu
  noble-security InRelease` until it was cancelled.
- **Demo.** Run 40008409878 on throwaway `04aac39e` (#498's install script plus
  a stalling apt proxy, not merged). A stall was stopped after 60 s and the
  retry passed. Two stopped attempts fell back to the snapshot, which
  installed. With the default limits and no snapshot, four 10-minute stalls
  failed the job red after 43.5 minutes, inside its 90-minute timeout.

Not covered: retries for the `linux` and `sanitizers` installs (#389), the
AppImage install's lack of a snapshot (#500), and the Qt ASan download (#502).
The `portable` job cited by that audit was removed with the retired platform.
