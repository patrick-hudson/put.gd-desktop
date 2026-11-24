# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Cinnamon login startup — issue #24 (2026-10-16)

The [login-fix report](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-16-issue24-login-fix.md) and its payload-free
JSON receipt record frozen implementation `09ce25b7851624e602c96dba855b4465e688fec9`.
Two actual Cinnamon console logout/login cycles started distinct verified
residents with a registered tray, preserved configuration and no app window or
UI helper. The new production-process startup regression failed before the fix
and all nine final scenarios pass. Full Dev passed 59/59; affected ASan/UBSan
tray/startup checks passed 3/3. These private-bus/Xvfb checks are host contracts;
the separate native VNC evidence establishes the bounded desktop results.

Cinnamon's twelve native capture/clipboard/cancellation cases, repeated native
view reopen, restart and both five-minute memory observations passed before the
GNOME/KDE package build and native campaigns. Both Wayland profiles passed the
same twelve cases and view/restart checks. Memory sampling followed each
restart and native History reopen/close: two serial 300-second intervals per
desktop met the 40 MiB RSS, <0.5% CPU, zero-swap and no-helper gates. They do not
establish same-resident post-capture retention, physical performance or complete
agent and release acceptance; broader workloads and release gates remain issue #44.

All three overlays were retired through guarded cleanup after evidence export.
Exact package/binary/run identities, raw samples, failures and cleanup receipts
remain private under `work/i24-fix/`; public summaries bind them by checksum.
The historical local-only suppressed-copy observation remains unreproduced and
is not claimed fixed by the startup change.
