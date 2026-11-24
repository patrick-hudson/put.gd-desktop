# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### Simple setup and measured performance follow-up (2026-10-08)

The simple destination form now opens with Advanced settings collapsed. Standard
S3 defaults and an explicit local-only choice share the validated settings draft;
expansion retains values. Credential storage remains a separate explicit action.
MiB/GiB controls preserve exact byte counts, including previously accepted odd-byte
values; activity/history summaries use readable units without changing JSON receipts.
The native KDE test first failed for missing disclosure and size controls, then
passed after implementation. The host full suite passed 25/25 (40.47s); the final
compact form passed the three affected host GUI targets (6.17s) and native KDE Qt
targets (5.16s). Those widget tests run offscreen and do not certify KWin rendering.
The standard CPack DEB was installed and its binary fingerprint verified. Installed
single-scale inspection is partial: complete scale, focus, error, and remote-deletion
visual acceptance remains open. Simple setup was implemented; the complete visual review was not established by this checkpoint.

The physical capture supervisor records private isolated configuration, hardware,
binary hashes, parameters, logs, operation IDs, and result data. The final actual
Cinnamon three-head run completed five warmups and 30 measurements with 35 cancelled
operations and no saved payloads. Its p95 painted overlay latency was 235.821ms,
which fails the 150ms budget. Earlier fixture-readiness failures remain recorded.
The fixture background is labelled neutral grey. The actual KDE selector was
separately inspected over the normal desktop with a drawn rectangle. Capture
latency still requires attribution and correction; this is not a passing release gate.

The VNC viewer showed fragmented incremental updates after a KDE window drag,
while contemporaneous direct QMP framebuffer witnesses remained clean. The exact
viewer/codec cause is unconfirmed. Investigation was parked at the user's request;
no viewer source was changed and this evidence does not establish a Qt paint bug.

An unchanged 300-second physical Cinnamon idle comparison at baseline 1e3735d measured
87.0 MiB peak. Deferred AWS initialization measured79.0 MiB peak and 0.1366% of one core,
passing the original idle budgets. All 25 existing release tests passed (47.83s),
as did 12 deferred runtime generations, 3072 concurrent initialization calls, eager
and unused-deferred lifecycles, and the production safe DELETE factory. No network
requests occurred. This candidate is retained on the isolated optimization branch
at 23102be. The 40 MiB
target is unmet. The earlier lazy-view candidate remains rejected because its
five-minute CPU gate failed; short diagnostics do not replace that result.

The next candidate constructs each persistent view on first request. Independent
review identified a GNOME shutdown callback that outlived its captured window
owner; a context declared after the view owners now disconnects it before teardown.
On the retained SDK baseline, the unchanged 300-second measurement passed at
58,671,104 bytes (55.95 MiB), with 0.00333% of one core. This is 23.05 MiB below
the SDK candidate and 31.06 MiB (35.69%) below the original 87.0 MiB baseline.
All 25 release tests passed (46.75s). Actual physical Cinnamon activation mapped
all three requested views and reused the same windows on repeated requests, with
no capture, clipboard-content access or network request. The optimized snapshot
is 016edb8; canonical integration is 9c1b128 plus d6c7519. Its fingerprint predates
the latest simple form changes, whose separate GUI evidence is above.

The bounded batch ended after three experiments: two retained and one degenerate.
The remaining one-hour budget could not fit another full measurement. The 40 MiB
target remains open, and neither empty-history results nor native mapping checks
establish upload/populated-history memory or complete visual acceptance. The
[structured comparison](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-resident-memory.json) retains parameters,
hashes, time series, page attribution summaries and the failed candidate.
These savings describe a background resident before views or S3 are first used.
Views remain cached to preserve drafts and selections; AWS stays initialized after
first S3 work. These measurements do not establish a reduction after those actions.
The integrated current dev build passed all 25 targets (48.02s), including the new
unused/rejected SDK-owner and barrier-started concurrent first-use regressions.
The four affected SDK/GUI targets also passed ASan/UBSan with leak detection
(14.33s) and the documented instrumented-Qt TSan environment (42.72s). The release
AWS SDK/OpenSSL/libcurl remain uninstrumented; this does not certify their internal
race freedom. No new suppression was introduced.
The focused Compound Engineering review combined a local correctness read with
an independent Anthropic Claude Opus 5.5 pass and returned no findings for the
six-file optimization slice at 016edb8. Its
[receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-memory-review.json) records exact scope and limitations;
it does not certify whole-product readiness or the later permanent-test diff.


A fresh agent followed the normal dev build, CPack installation, native Cinnamon
lab and locked official MCP instructions on a new KVM overlay visible at VNC5935.
The host 25-target suite passed. Native region capture and independent Qt paste,
then GTK text, image and ordered-file acquisition/paste passed. The GTK fixture
initially requested an unadvertised URI format and accepted an empty stream;
filtering its preference list by actual advertised formats fixed negotiation.
The original failures remain preserved, and byte/pixel/paste comparisons were
unchanged. Eight lab contracts and Python compilation passed for the fixture fix.

The actual graphical resident exposed all 12 MCP tools. An action through the
official SDK yielded the same operation result as the production CLI. Stale
configuration revision rejection, invalid reload recovery and visible settings
refresh passed. The stdio client needed the explicit resident socket because its
minimal child environment omitted XDG_RUNTIME_DIR; the documented endpoint option
worked without launching another resident. The
[structured receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-fresh-cinnamon.json) retains run parameters,
package/binary hashes, operation IDs, failures and exact limitations. This local
single-head subset does not complete full lab and agent acceptance, remote publication/deletion, the
compositor matrix or release lifecycle acceptance.
