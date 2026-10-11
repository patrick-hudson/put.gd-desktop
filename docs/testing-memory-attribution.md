# Memory attribution before the hidden-window experiment

These are measurements of unchanged product code, not optimization results.
The current source is `dac46af06feb0e8326d472c77bdf96cff117be51`. Its product
sources and CMake files are identical to the preceding desktop wave's
`a3d70dc2a6c6866fc6e38dd5e001553fb5c157a2`; the intervening change added test
identity guards and evidence. Each guest used its own distro-native build.

| Workload | RSS MiB | Retained drawing-buffer RSS MiB |
| --- | ---: | ---: |
| Physical Cinnamon, median of two three-view runs | 73.59 | 8.02 |
| GNOME, after the native functional wave | 114.36 | 26.68 |
| KDE, after the native functional wave | 186.17 | 26.72 |

The rows use different workloads. Compare a later candidate against its own
desktop's saved workload, never subtract one row from another. Physical runs
each opened Settings, History and Activity three times, with two seconds visible
and 30 seconds hidden per cycle, and no operation records. Their final RSS values
were 77,488,128 and 76,845,056 bytes. All windows reopened; the median is
77,166,592 bytes and the spread is 643,072 bytes.

The Wayland observations follow the twelve native capture, clipboard and
cancellation cases plus repeated view opens recorded in the [current desktop
matrix](evidence/2026-10-08-current-desktop-matrix.json). GNOME also performed the
recorded live-background region check. The common quiet 120-second observation
used five-second intervals after five seconds of warmup, with 25 unchanged RSS
samples per guest. Do not compare these used sessions with the separate physical
five-minute cold result of 44.9648 MiB.

## Where the post-use memory resides

GNOME has nine `memfd:wayland-shm` mappings of 3,108,864 bytes each. KDE has ten
of 2,801,664 bytes each; the latter size equals a page-rounded 1000 × 700 × 4
buffer. All were resident after the views closed. Their sizes and Qt's resource
lifetime support investigating retained backing stores. They do not prove
which view owns each mapping or that buffers grow without bound.

KDE's attribution snapshot also contains 48.28 MiB RSS of LLVM and 10.13 MiB of
Gallium, with only 5.69 and 1.68 MiB PSS respectively. RSS counts shared resident
pages in full. PSS and private counts explain that accounting without replacing
the RSS target. KWin reports llvmpipe; this does not identify the app's own GL
renderer. The AWS module is absent. QtNetwork remains transitively mapped on KDE
despite being absent from the resident executable's direct dependencies.

Qt 6.4.2 and official Qt 6.10.2 source inspection supports a bounded candidate:
release native window resources after an eligible hide, retaining the logical
widget tree. Qt's backing-store destructor releases its drawing buffers; ordinary
hide keeps them. Source inspection is not runtime proof, and distro patches were
not audited. No product code has changed for this candidate.

Before keeping it, test unsaved settings and conflict state, history filters and
selection, preview cleanup, activity selection, dialog parenting, minimize,
rapid hide/show, destruction with a deferred callback, full repaint and reopen
latency. Keep native fonts/dialogs. Require measured results and functional
acceptance on Cinnamon, GNOME and KDE; offscreen widget tests alone are insufficient.
The [frozen proposal](evidence/2026-10-08-memory-attribution-raw/proposal/spec.yaml)
and [checkpoint](evidence/2026-10-08-memory-attribution-raw/proposal/experiment-log.json)
record the scope, baseline, gates and pending approval. The live working copy is
under `.context/compound-engineering/ce-optimize/resident-view-surfaces/`.

## Cold-start attribution

A separate, normal CMake/Qt probe constructed native Cinnamon integration,
the app's visual style, brand icon, tray icon and inert equivalent menu in seven
stages. It did not instantiate product services, read the clipboard, capture or
upload. Each stage settled for five seconds; the last performed one allocator
trim. The final RSS was 41.37 MiB, compared with 39.70 MiB at the initial empty
native QApplication stage. These sequential stage costs are locating evidence,
not paired production savings or a lower bound.

Creating the empty tray menu added 1.12 MiB before trimming; populating its
actions added 0.47 MiB. Deferring actions alone therefore has weak support as a
meaningful saving. Deferring the entire menu needs separate native first-click
and exported-menu validation. The product's remaining mapped executable and SQL
closure account for about 1.37 and 1.61 MiB respectively; their full sizes are
ceilings, not savings estimates. There is no supported cheap five-MiB forecast.
The approximately 40 MiB cold target remains open.

## Evidence and replay

The [raw manifest](evidence/2026-10-08-memory-attribution-raw/manifest.json)
maps executed scripts and receipts to their original local paths and hashes.
It excludes credentials, payloads, VM disks, binaries and the SDK source archive.
The [cold probe bundle](evidence/2026-10-08-memory-attribution-raw/cold/README.md)
has its own source/build manifest and replay instructions.

Verify every committed file against its manifest before reuse. Copy scripts into
a new output directory; several historical collectors use fixed filenames and
must not overwrite prior evidence. Record changed host/output paths and the new
script hash. Use a current overlay's pinned SSH connection from `lab.ssh_args`,
check `lab.verify_running_app`, and pass the actual PID, process start ticks and
executable hash. Never reuse historical process identities after a restart.
GNOME's `inspect-resident.py` accepts those values as arguments. Its supplemental
`inspect-shm.py` and KDE's historical collector have expected identities in their
source; each needs a separately hashed replay copy with every identity assertion
updated to the newly verified process.

For native functional tests and the common quiet sampler, follow [the existing
wave replay](testing-vm-memory-audit.md#replay). Keep graphical work visible through
the VNC display wall. Finish builds and GUI actions before sampling; physical
Cinnamon runs remain serial. Independent guest builds and functional checks may
run in parallel. Use disposable overlays, leaving golden disks and retired cache
VMs untouched.

For the physical view run, use `tests/performance/bench_views.py` with the exact
parameters and command receipt in each saved result. The archived wrapper calls
that existing harness; it is not a substitute observer. It expects an isolated
empty-history fixture, the matching candidate app/CLI and a fresh output path.
Do not reuse the historical temporary fixture without verifying its state.
`physical/required-verification.json` records the planned functional checks;
it is a checklist, not proof that a candidate passed them.

The archived scoring adapter aggregates two already-executed physical results
and the actual three-desktop wave. It verifies workload hashes, process identities
and successful cases before printing numeric metrics. It does not execute the
wave itself or independently establish visual repaint correctness. Preserve
all raw attempts alongside its result. A failed or missing desktop cannot be
reported as a successful memory experiment.
