# Native listener memory experiment

> **Historical record.** This page describes an app version, experiment or plan that no longer exists and is kept for context only. The current design is in [overview](../overview.md); see [memory-strategy](../memory-strategy.md). Do not use it as a work list.

Candidate `676f6e6d6e0b026dcdd1b4ef2ae7dabc8a29ded3` on
`codex/optimize-resident-native-boundary` reached the physical Cinnamon memory
target. Its version is `0.1.1-dev.49+g676f6e6d6e0b`. It remains experimental:
region selector startup regressed, so the implementation has not been integrated.
The [structured summary](../evidence/2026-10-09-native-listener.json) and
[hashed raw receipts](../evidence/2026-10-09-native-listener-raw/manifest.json) preserve
passing checks, failed checks, exact commands, parameters and source identities.
Documentation commits do not change which binary was measured.

## What changed and what was measured

The Cinnamon X11 resident constructs QCoreApplication, handles shortcuts and
clipboard ownership through XCB, and presents its tray and menu through the
StatusNotifierItem/dbusmenu protocols. Settings, History and Activity still use
the existing on-demand Qt Widgets process. Screenshots use a separate bounded
capture process which exits after completion or cancellation. Native fonts and
folder dialogs remain enabled. QtGui/Widgets are still dynamically linked; this
is deferred initialization, not removal of their shared libraries.

Two serial cold five-minute runs measured 24.676 and 24.555 MiB peak RSS, compared
with the separately identified 45.414 MiB reference. Both cold runs had exactly
one app-family process throughout. The two closed-view runs measured 25.229 MiB
median whole-family RSS, compared with 45.600 MiB: a 20.371 MiB (44.67%) reduction.
The original forecast was unknown product saving: the standalone initialization
contrast suggested a boundary worth testing, not an additive 28.7 MiB product
win. The required improvement was about 5.6 MiB; the measured combined 20.371 MiB
closed saving clears it. Independent contributions were not measured.

Closed PSS was 16.765 MiB; peak open whole-family PSS was 52.137 MiB. RSS and PSS
are different metrics. These two repeats are confirmation, not a confidence
interval or a claim that every workload fits in 40 MiB.

The local-only clipboard owner deliberately retains encoded image bytes while
an image must remain pasteable. It shares the encoded QByteArray rather than
keeping another decoded image. Replacement/ownership loss releases the current
version. At most sixteen accepted incremental transfers can retain older
versions, bounded by their size and deadlines; include them in payload tests.
Successful uploads normally replace the clipboard with a small URL. The cold
and empty-view workloads do not measure large retained clipboard content.

The same clean source passed 46 host development tests, three separate MCP
contracts, twelve visible native Cinnamon cases and real tray/view interactions.
GNOME and KDE retain their graphical resident route in this candidate. Their
current-source regression checks and post-use memory must be read separately;
a Cinnamon result does not establish a Wayland saving.

All three current-source VMs passed twelve native cases and normal view-helper
exits. Five-minute post-use whole-family observations were:

| VM desktop | RSS | PSS | Private |
| --- | ---: | ---: | ---: |
| Cinnamon X11 | 28.520 MiB | 18.383 MiB | 17.496 MiB |
| GNOME Wayland | 60.219 MiB | 34.001 MiB | 30.301 MiB |
| KDE Wayland | 138.559 MiB | 36.995 MiB | 24.059 MiB |

Each observation contains 61 samples with one resident, no GUI/capture helpers and
no escaped descendants. Independent guest observations overlapped after builds
and views drained. They are separate from serial physical measurements; no
matched old-guest baseline or per-change Wayland saving is claimed. KDE's
Mesa/EGL/LLVM category accounts for 60.80 MiB RSS but 8.13 MiB PSS. Basename mapping
groups locate library residency; they do not prove allocation call sites or a
leak. The Wayland 40 MiB RSS target remains open. GNOME development tests passed
47/47 and KDE 46/46; GNOME's extra GJS ownership test uses mocked Shell dependencies
and is separate from its real native cases.

## Repeatable source and build

The experiment branch is currently local and preserved for review. A fresh clone
can replay it only after that branch is published or its exact Git objects are
transferred. Stop if the commit is unavailable; do not recreate it from an older
checkout.

Start from the exact candidate commit, not a newer documentation commit or an
existing binary with a matching version label. Preserve the commit, tree, count,
source archive SHA256, build manifest and hashes of the resident, CLI, UI helper,
capture helper and S3 module. Record compiler, Qt, SDK, kernel and compositor.
Use the normal presets and the pinned official SDK from [build.md](../build.md).
There are no new dependencies in this experiment.

```sh
git worktree add --detach /tmp/putgd-nc49 676f6e6d6e0b026dcdd1b4ef2ae7dabc8a29ded3
cd /tmp/putgd-nc49
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-junit /tmp/putgd-nc49-dev.xml
cmake --preset release -DPUTGD_PHYSICAL_BENCHMARKS=ON -DPUTGD_LAB_FIXTURE=ON
cmake --build --preset release
```

Pass the documented SDK prefix/source/runtime flags when using an existing
provisioned official SDK. Do not substitute a toolchain or statically link Qt.
The source archive used for measurements must come from this clean commit;
archive builds require the exact exported provenance described in
[versioning.md](../versioning.md). Build and test logs are artifacts, not success
claims until their exit status and assertions are checked.

## Native gates and quiet physical replay

1. Build and test one Cinnamon candidate first. Stop its old resident by verified
   PID/start/executable identity and confirm exit before installing the new one.
   Preserve the prior synthetic profile and receipts. Never boot a golden disk.
2. Use the existing lab runner and native fixture described in [vm-lab.md](../vm-lab.md).
   Keep graphical output visible on the VNC wall. Restore locked sessions with
   protected bootstrap input through a confirmed masked console field. No guest
   Node.js fixture is needed. Preserve twelve independent Qt/GTK capture,
   clipboard and cancellation receipts; review actual tray and all three views.
3. Preflight complete UTF-8 Unix socket paths, including harness-added suffixes.
   Use fresh short private `/tmp` roots. Run two serial cold/view pairs on the
   physical Cinnamon X11 session, after builds and other GUI tests drain.
4. Cold parameters are 5 seconds warmup, 300 seconds observation, RSS every
   30 seconds and owned-window checks every second. No actions, views or uploads.
   The supplemental observer samples the complete process family every second.
5. Each closed-view run opens Settings, History and Activity for three cycles:
   5 seconds startup warmup, 2 seconds visible, 30 seconds quiet after close,
   samples every 5 seconds. Require normal helper exit, no remaining children,
   no active operations and exact binary identity. Target both cold peak and
   closed family RSS at most 41,943,040 bytes (40 MiB), idle CPU below 0.5% of one core.
6. Only after Cinnamon functionality and memory pass, run independent GNOME/KDE
   checks. Preserve their real native cases, current package/running identities,
   views and five-minute whole-family post-use samples. VM post-use data is not
   the physical cold workload. A failed capture latency gate still blocks keeping
   or releasing the candidate.

The exact physical command arrays are saved under
`evidence/2026-10-09-native-listener-raw/commands/`. The frozen supplemental
`cold_family.py`, `physical_sequence.py` and command recorder are exported under
`.../supplemental/`. They contain this machine's paths and cohort identifiers.
For replay elsewhere, copy them into a private scratch directory and change only
those paths to the fresh checkout, archive, native receipts and unused outputs;
keep sampling, assertions, order and accepted source identity unchanged. Record
both original and adapted script hashes. Their ProcessFamily implementation
comes from the frozen `tests/performance/bench_ui_process.py`, including escaped
children and PID/start/inode checks. Do not exclude a child merely because its
parent exited. Preserve failed attempts before any deliberate retry.

## Selector latency is still a blocker

The clean candidate's thirty warm live-outline observations measured 951.70 ms
p95 hotkey-to-paint and 243.51 ms median, versus 596.01 ms p95 and 142.51 ms median
for clean source45 using the same candidate driver, geometry and witness mode.
Both miss the existing 150 ms gate. Median time to mapping rose from 29.61 to
140.18 ms. The roughly 100 ms median regression is real in these observations;
it has not been fixed or attributed to one component. Pixel observation includes
X11 round trips, driver polling and input injection, so it is not compositor
presentation timing.

A syscall locating replay traced process/flush/poll calls only, excluding payload
and environment reads. It stopped after five samples when a selector did not
map/paint. The largest of fifty recorded flushes was 1.757 ms. Concurrent guest
builds and tracing overhead make that replay ineligible for timing scoring.
Fast flushes alone do not exclude journal queue/handoff/locking costs. No SQLite,
prepared-statement or durability change was justified by this result.

The next locating measurement should timestamp hotkey delivery, source request,
invocation freeze, child start, helper bootstrap, target resolution and first
selector map/paint on one monotonic clock. Use bounded phase identifiers without
paths, pixels or configuration contents. Keep the original benchmark and matched
control, and record instrumentation separately from scored release runs. Changes
to OperationService/history need a newly scoped experiment; do not skip durable
journal checkpoints or keep a hidden capture helper to make a timing result pass.

## CI and evidence retention

Ordinary hosted CI can run the CMake/CTest contracts and separate MCP suite and
retain JUnit, build manifests and structured logs with an explicit upload allowlist.
Native desktop and physical memory/latency acceptance need the corresponding real
session on a controlled runner. Xvfb/offscreen tests cannot establish those gates.
A future scheduled lab job should first freeze one source archive, run Cinnamon
functionality and memory, then expand to sibling guests; always compare installed
and running executable hashes before accepting results.

Do not export config files, databases, clipboard bytes, resident logs, credentials
or protected VM login inputs. This evidence export contains explicit synthetic
structured receipts and reviewed synthetic VM screenshots only. The local
`.context` ledger remains resumable; the tracked summary retains its measured
state even when scratch directories are unavailable.
