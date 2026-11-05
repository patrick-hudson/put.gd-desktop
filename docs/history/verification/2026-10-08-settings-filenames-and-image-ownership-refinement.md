# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### Settings, filenames and image ownership refinement (2026-10-08)

The simple form now exposes editable S3 fields on first use, with searchable
region suggestions and standard provider defaults. Detailed adjacent info icons
are reachable by keyboard; plain labels, neutral charcoal surfaces and a blue
accent replace the green treatment. Cancel discards unsaved fields and clears
credential input. Reload from file rereads accepted external configuration;
an accepted save shows a blue confirmation banner. The installed Cinnamon
1000×700 window was inspected at scale 1 on a 1280×800 display through VNC5935.
Witnesses cover fresh setup, region selection, help, advanced filename patterns,
save feedback, unsaved Cancel/reopen and Reload. Full visual scale/state acceptance
and final user visual approval remain open.

Fresh configuration preserves imported file names and uses timestamped capture,
clipboard image and text names. Optional templates share the validated JSON
contract; existing generated-mode settings retain their choice. Names use the
accepted action's local timestamp even when queued, then persist for retries.
Local archive collisions preserve existing files. Review identified schema drift,
queue-time naming and a History reopen/cancellation defect; each has a genuine
failing regression and a tested correction.

Image payloads move through acquisition and are cleared after immutable disk
persistence, before preview/archive/network consumers. Cancelled queued images
release immediately. A configurable aggregate queued-image budget defaults to
64 MiB. The cleanup-callback test proves decoded pixels are freed while upload
is still blocked and retry reuses the same disk artifact. History decodes one
selected bounded thumbnail at a time and clears it when hidden. The 10,000-row
metadata ledger remains tested. This proves ownership and admission behavior;
it does not measure allocator page return, post-use RSS or complete the 40 MiB goal.

The final standard Debug suite passes 25/25 (49.71s); eight affected contracts pass
ASan/UBSan with leak checking (37.47s) and the documented instrumented-Qt TSan
environment (82.72s). The optional actual-resident/CLI JSON schema test passes
1/1 (0.256s). No new sanitizer suppression was added. Production continues to
use distro Qt; the release SDK/OpenSSL/libcurl are uninstrumented. The rebuilt
standard CPack DEB was installed and its binary hashes verified in the disposable
Cinnamon guest. Native region/Qt paste (80×60) and clipboard-image/GTK4.14 X11
paste (96×64) pass with matching pixel hashes and correct timestamped local names.
The lab's original configuration was preserved and its resident restored.

The [structured record](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-settings-refinement.json) retains source,
binary and package fingerprints, commands, parameters, failure/green log hashes,
JUnit hashes, controlled GUI inputs, screenshots and separate review/fix receipts.
Only synthetic local workflows were exercised here. Successful live S3, full
desktop/profile, release lifecycle, MCP and physical performance gates remain open.
