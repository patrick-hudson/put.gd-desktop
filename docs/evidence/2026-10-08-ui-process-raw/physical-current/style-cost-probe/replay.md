# Temporary style allocation cost probe

This is a cost-locating diagnostic, not a product variant or a savings claim.
The caller must explicitly release the physical measurement gate before any
configure, build or native execution. `run.py` additionally requires the root's
completed four-run matched-execution receipt. Preparation does not configure,
build or run anything.

After caller authorization, from this directory:

```sh
python3 run.py --gate ../matched-execution.json
```

The driver uses ordinary `cmake --preset dev` and `cmake --build --preset dev`
with Ninja, C++20 and dynamically linked system Qt6 Widgets (minimum 6.4.2).
It compiles the exact frozen production VisualStyle.cpp and header in place,
verified against pinned SHA-256 hashes. It changes no product source or packages.
The executable has two modes and runs full/native/full/native serially, five
seconds per mode, with a fifteen-second process timeout. One results directory
is permitted; failures are retained and stop the sequence without retry.

Each JSONL stage records /proc/self/smaps_rollup RSS, PSS and private clean/dirty
bytes plus elapsed milliseconds: QApplication constructed; full production
VisualStyle applied or native styling retained; production-equivalent brand
icon, tray and nine-action menu (plus two separators) created; malloc_trim(0) at two seconds; quiet
settled sample at five seconds. QApplication font, actual platform, Qt version,
style class, stylesheet character count, trim return and tray availability are
recorded. The receipt pins inputs, executable and loaded shared-object hashes.
Stdout carries stage JSON only; Qt/build diagnostics stay in separate stderr.

Actual Cinnamon/X11 is required. Native platform integration/fonts remain
enabled; no font, dialog, theme or platform overrides are added. A temporary
diagnostic tray icon is visible, labelled in its tooltip. It never opens a menu
or window and no menu action is connected. It exits normally and hides only its
own tray. It does not read application configs, clipboard, payloads or secrets,
invoke storage/network, inspect/stop app or VM processes, or change desktop
settings. The native variant retains the same icons/action labels but uses
native menu styling. Adoption would require tray/menu visual and accessibility
parity, capture/native font/dialog parity and full product measurements.

Stage deltas include instrumentation and Qt lazy/event-loop work. The first
memory reader and JSON writer also allocate; there is no pre-QApplication
sample. Five seconds is a short settling observation, not a long product idle
budget. RSS may share pages with other processes; PSS depends on sharing at the
sample instant. Do not subtract these probe totals from the product or claim
the full/native difference as achieved product savings.

Export allowlist: these four inputs plus replay.md; results/receipt.json,
results/configure.stdout/.stderr, results/build.stdout/.stderr and exactly the
four numbered JSONL/stderr pairs when generated. Do not export the build tree,
environment, private files or directories wholesale.

Observed run: all four modes completed once. Median settled full/native totals
were 41.672/41.043 MiB RSS and 23.045/22.375 MiB PSS, a 0.629/0.670 MiB
difference. Full styling immediately added 0.1875 MiB in each run. Both modes
loaded qgtk3, GTK3 and XApp and retained Ubuntu 10. These are probe totals and
are not achieved product savings. The default coarse timers sometimes fired
slightly before the nominal delay; exact elapsed milliseconds remain in each
JSONL record. Final dependency-path enumeration occurs before the final sample
and contributes instrumentation allocations. No reruns or failed attempts.
