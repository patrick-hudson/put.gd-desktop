# Replaying on-demand view startup measurements

Keep the on-demand process split and resident tray. The original source-41
comparison reduced closed whole-family RSS by about 20 MiB, but its cold
five-minute peak was 44.390625 MiB. The 40 MiB target remains open. This document
covers Settings reopen timing; it does not replace capture, populated-history,
post-upload or cold-idle acceptance.

## Frozen workload

Use `tests/performance/bench_ui_process.py` without changing its observer.
The approved reference hash is
`a28c94b075036ea65f8eca8e068d480cff46ee959d4680593f5c4d55cce3fc40`.
Each process run uses split mode, Settings/History/Activity in that order,
three cycles, a 5-second resident warmup, 2 seconds visible after readiness,
30 seconds quiet after closing, and a 5-second sampling interval. Confirm a
candidate with two independent serial runs, giving four Settings reopen samples
(cycles 2 and 3). Compare their median with the original 332.6767 ms reference.
A saving must exceed 40 ms; the target is approximately 200 ms.

The same validated private fixture must have local actions, empty history,
no shortcuts, notifications or autostart, and no credentials. The harness
copies validated configuration into its own isolated state directory. Never
point it at the production configuration, history, captures or credential file.
Do not retain or prewarm an invisible helper. Normal last-window close must
checkpoint safely and exit the helper; resident shortcuts, clipboard, tray and
operations remain alive.

Freeze the exact commit and archive before building. Record archive/source and
app/CLI/helper/module hashes, the compiled manifest, and PID/start/hash identities.
An instrumented or uncommitted overlay needs its complete patch and changed-file
hashes; its base commit label does not identify its executable bytes. Preserve
failed attempts under new output names. Never relabel an earlier binary after a
documentation commit.

## Native correctness and quiet timing

Before confirming performance, validate the candidate on Cinnamon/X11,
GNOME/Wayland and KDE/Wayland. Builds and functional scenarios can run in parallel
across those desktops. Keep guest output on each existing localhost VNC listener
and visible through the display wall. Stop verified old app residents before
starting replacements; preserve immutable VM bases and historical evidence.

Complete real region/window/desktop and Qt/GTK clipboard tests, region
cancellation, view reopen, draft/Cancel behavior, full-ID routing, independent
result-copy paste, normal helper exit and rendered tray evidence. Offscreen
widget tests establish contracts, not compositor acceptance. Report a missing
native case or uncertain compact geometry as an open gate.

Drain all compilation and active desktop testing before physical timing. Run
only one measured family at a time on the same Cinnamon session. Keep native
fonts and folder dialogs enabled. Preserve per-run command, environment summary,
fixture identity, structured events/samples, CLI acknowledgement and independently
observed mapped readiness. The observer includes process/window/diagnostic work;
neither a `show()` return nor CLI acknowledgement proves painted pixels or p95.

Reject a candidate exceeding 46.5 MiB closed RSS, 29 MiB closed PSS or 60 MiB
maximum sampled open PSS. These whole-family gates account for resident and helper;
summed RSS counts shared pages twice. The workload's success flags establish only
this empty three-view exercise. Verify native receipts separately.

## Locating measurements

The first BASE41 diagnostic overlay retains source and binary provenance and
fixed stage-only stderr markers. It exports no settings, widget text, secrets or
payloads. Native Qt initialization, control/bootstrap, snapshot validation,
Settings construction and showing are timed separately. Its observed reopened
constructor/show cost was 116–129 ms, versus under 7 ms for the handshake.
Those values guide experiments; diagnostic overhead prevents treating them as a
product saving. Hidden-page cost attribution precedes any lazy-page refactor.

The local optimization ledger is
`.context/compound-engineering/ce-optimize/view-startup-latency/`.
It retains the approved specification, opportunity estimates, source digest,
raw attempt paths and decisions. Durable evidence must include the final hashes,
commands, patches, structured results and unresolved gates before scratch cleanup.

## CI and agent use

The existing GitHub workflow preserves JUnit, CTest logs, lab-contract reports
and build identity for 30 days. Its containers run compiler/core/widget and host
protocol checks; they do not establish native desktop or physical memory budgets.
A future native performance job needs a dedicated graphical runner, the same
fixture and workload, current-source native acceptance, exclusive timing and
artifact upload even after failure. Do not substitute a hosted container's timing
for this physical comparison. Each agent should inspect the actual installed
build/capabilities before replay and record the exact effect and operation IDs.


The GNOME extension ownership fixture runs with the desktop's existing GJS
interpreter (`gjs -m tests/desktop/test_gnome_ownership.mjs
integrations/gnome/extension.js`). It uses mocked Shell dependencies and provides
no graphical acceptance. Node is not a host/guest test requirement. Where GJS is
absent, run the shared C++ contracts and obtain this extension-specific receipt
from the GNOME lab; do not install another JavaScript engine for it. File loading
uses the [GJS/GIO file API](https://gjs.guide/guides/gio/file-operations.html).
