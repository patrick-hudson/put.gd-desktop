# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### On-demand UI implementation checkpoint — 2026-10-08

Settings, History, Activity and About now run in `putgd-ui`. The resident owns
the tray, capture actions, shortcuts, clipboard, configuration and accepted work.
Closing the final view checkpoints bounded safe state and releases the helper.
Strict build agreement, bounded startup/activation, explicit shutdown and typed
error recovery preserve the resident command interface. Typed credentials and
image buffers are excluded from retained drafts.

Development tests pass 41/41; the 16 affected ASan/UBSan and instrumented-Qt TSan
contracts pass. Release builds and the separate three-case MCP integration suite
pass. A production-helper Xvfb test checks four ordinary close/reopen generations,
safe drafts, zero child exit, clipboard ownership and completion of accepted work.
The GTK debug startup failure was reproduced and corrected using private control
descriptors established before QApplication. These are headless/isolated checks,
not current Cinnamon/GNOME/KDE acceptance.

[The structured checkpoint](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-ui-process.json) retains the
completed review, four confirmed findings and caller fixes, command scripts,
JUnit results, source hashes and a 15-case total-process sampler test receipt.
GNOME ownership-clear and expired setup-result recovery remain scheduled fixes.
Graphical AE17, deterministic GNOME selector readiness, total-process memory and
reopen latency remain open. The 40 MiB target is not yet met.

The two conditional review concerns now have bounded recovery fixes. Setup
inspection retains accepted jobs across transient errors, removes expired IDs
without starving later results, and never repeats the accepted action. Its three
new regressions failed before the fix; the full Settings suite and affected
ASan/UBSan and instrumented-Qt TSan checks pass. GNOME now retries failed UI-PID
clearing through the existing probe. A native Qt Wayland/private-D-Bus regression
reproduced the failure and now passes all three recovery scenarios, including
newer PID and Shell-owner protections. See the [setup receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-ui-setup-recovery.json)
and [GNOME receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-ui-gnome-recovery.json). These checks do not
close the pending current-source graphical or total-process memory gates.
