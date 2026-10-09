# BASE41 hidden-page attribution probe

This locating measurement is the verified original source41 archive plus a declared
diagnostic overlay. It is not a product optimization or counted candidate. The
compiled base version label does not claim original unmodified binary bytes.
Product sources/index, native Qt fonts/dialogs/tray behavior, widget construction,
private protocol, helper exit, fixture and frozen performance harness are unchanged.

`prepare.py` independently extracts the original archive, verifies its SHA-256 and
all 1,348 source hashes, then applies the original startup-stage overlay and
`extra_overlay.py`. `instrumentation.patch` records the complete patch against BASE41.
`source-provenance.json` records all changed hashes, unchanged-source verification,
base identity and runtime binary hashes. `build.py` uses the recorded original
release CMake/Ninja configuration with the existing official SDK and jobs=2.
No alternate dependency/toolchain is introduced. The new binary identities must
always be accompanied by overlay provenance.

Additional fixed timestamp pairs cover Capture, Destinations, Shortcuts,
Preferences, wiring and initial refresh. Wiring includes advanced visibility and
footer setup. Constructor-body wall time outside these pairs is shell setup and trace
bookkeeping; inherited RetainedWindow construction precedes its entry marker. Per-page construction includes synchronous work irrespective of page
visibility; this does not estimate the net benefit or correctness of lazy pages.

A diagnostic QApplication subclass delegates native dispatch exactly once.
Only QEvent::Polish for Settings-descended widgets is timed. Fixed private integer
properties mark each page body and scroll wrapper; other Settings widgets are
`shell`. Each nested timed Polish duration is subtracted from its parent so totals
are exclusive and non-overlapping. No widget names, labels, text, values, settings,
paths, payloads or class names are exported. The resident validates bounded fixed
phase/page tokens and numeric fields before relaying; all other helper stderr
remains discarded. Cumulative snapshots are taken at ctor entry/exit, show pre/post
and shown send. Collection stops after shown send.

Page ancestry is observed at dispatch time. A widget polished before reparenting
may be attributed to shell or remain outside Settings tracking; this is a known
lower-bound limitation. Notify timing does not encompass all Qt font/style/layout
work. Per-page Polish totals overlap constructor/show wall intervals and must not
be added to wall time. Property lookup, counters and trace writes add overhead.
Mapped observer latency remains independent and includes observer overhead.

Parent alone owns replay after all build/test/native work quiesces. The physical
Cinnamon session must remain visible. This preparation agent performs no runtime,
display, SSH or graphical measurement. Print the plan without display interaction:

```sh
python3 work/view-startup-latency/hidden-page-probe/replay.py --output work/view-startup-latency/hidden-page-probe/run-1
```

Append `--execute` only from the parent after quiescence to run the frozen
`tests/performance/bench_ui_process.py`: settings/history/activity, cycles3,
warmup5/visible2/quiet30/interval5. It reuses the validated original private empty
fixture `/tmp/putgd-startup-reclaim-exp001` and checks binary/harness hashes before
invocation. Every output directory must be new. Runtime products will be marked
diagnostic via `--allow-dirty` and attached overlay provenance.

```sh
python3 work/view-startup-latency/hidden-page-probe/parse_trace.py --log work/view-startup-latency/hidden-page-probe/run-1/resident.log --results work/view-startup-latency/hidden-page-probe/run-1/results.json --output work/view-startup-latency/hidden-page-probe/run-1/stage-attribution.json
```

Parser output correlates fixed safe stage and cumulative Polish records to helper
PID/workload cycles, retains CLI shown acknowledgement and independent mapped time,
and reports missing/non-monotonic counters. It does not export rejected content.
No native desktop acceptance, p95, populated history or post-upload memory claim
follows from this preparation or three-cycle locating replay.
