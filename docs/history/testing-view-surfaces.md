# Replaying retained-window memory checks

> **Historical record.** This page describes an app version, experiment or plan that no longer exists and is kept for context only. The current design is in [overview](../overview.md); see [memory-strategy](../memory-strategy.md). Do not use it as a work list.

The 2026-10-08 experiment releases native backing stores after an explicit close
of Settings, History or Activity. It retains widget state, protects visible owned
dialogs, ignores spontaneous minimize hides, and reapplies native window state
when recreating a surface. The source change is ten files, committed as
`427662c9bf9d4a425d672e27b085c16609b7d4f8` on the optimization branch and integrated
as `0a2b104595e06dbdc27ae64b8677c5336d819419` on the feature branch.

The [structured summary](../evidence/2026-10-08-view-surfaces.json) links the exact
source archive, binaries, comparisons, commands, parameters and correctness
receipts. The [raw manifest](../evidence/2026-10-08-view-surfaces-raw/manifest.json)
contains checksums. Historical absolute work paths inside receipts describe the
original run; the exported files under that manifest are the portable record.

## Build and correctness

Use [the ordinary system toolchain](../build.md). Compare clean source snapshots of
baseline `dac46af06feb0e8326d472c77bdf96cff117be51` and the experiment commit.
Do not silently substitute the latest branch: it also contains versioning and
the separately measured X11 shortcut correction.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-junit "$PWD/build/dev/view-surfaces.xml"
cmake --preset release
cmake --build --preset release
```

The affected GUI tests are `gui_settings`, `gui_history_view` and
`gui_operations_view`. Their added checks exercise backing-store disposal,
draft/conflict retention, repeated repaint, queued release, minimize, modal child
ownership and teardown. The corrected candidate passed those checks in Dev,
ASan/UBSan and TSan builds. See `host-checks-001b.json` in the raw evidence for
exact incremental scope; do not call three affected checks a complete sanitizer
suite. The canonical integration also passes the modeless About GUI contract.

## Physical Cinnamon comparison

Follow [testing-memory.md](testing-memory.md) to create the isolated local-only
fixture and restore the physical session. Use the committed `bench_views.py`
with three cycles, five-second startup warmup, two-second visible settling and
30-second final hidden idle. Run twice, serially, and compare median final RSS.
Record the native desktop and dynamic Qt versions and retain each result.

```sh
python3 tests/performance/bench_views.py \
  --app /absolute/release/putgd --cli /absolute/release/putgd-cli \
  --fixture /absolute/isolated-fixture --output /absolute/new-run \
  --cycles 3 --idle-seconds 30
```

The frozen wrapper `measure-views.py` and scorer `score-surfaces.py` are exported
with the raw results. Their original paths/identities intentionally identify that
experiment; adapt paths in a new copy and record its hash for a new run. Do not
overwrite a prior result or edit a comparison's frozen workload after measuring.

Baseline median was 73.59 MiB; candidate median was 65.38 MiB, an 8.21 MiB saving.
The two candidate samples differed by 32 KiB. A separate untouched 300-second
cold run ended at 44.91 MiB and about 0.01% of one core. Original idle budgets
pass; the approximately 40 MiB goal remains open.

## GNOME and KDE comparisons

Use [the immutable-base VM lab](../vm-lab.md) and the current-source protocol in
[testing-vm-memory-audit.md](testing-vm-memory-audit.md). Build the same frozen
source archive with each guest's system Qt and pinned SDK. Record archive,
package and installed executable hashes plus live PID/start time. Keep graphical
interactions visible through each VM's localhost VNC listener.

Run the frozen twelve native cases and two accepted Settings/History/Activity
cycles; GNOME also repeats its live-background region case, giving thirteen
operations. Clean up known synthetic fixture processes and close every view.
Observe for 120 seconds, sampling every five seconds after a five-second warmup.
The exported `sample-wave.py` checks live identity and functional receipts. Its
original result is distinct from an untouched cold benchmark.

GNOME fell from 114.36 to 86.05 MiB RSS. KDE fell from 186.17 to 157.23 MiB.
Because baseline KDE had extra attempted opens, a conservative repeat reached
four accepted cycles and ended at 157.31 MiB, with zero Wayland SHM buffers.
Failed lock-screen or wrong-foreground attempts are preserved and excluded from
accepted-cycle counts. The extra observation makes no CPU claim.

After primary measurements, verify maximize/close/reopen, minimize/restore,
fullscreen where supported, dirty draft/Cancel, dropdown placement and native
folder dialog ownership/cancellation. Inspect actual screenshots. GNOME's
normal-size behavior was reproduced in both plain QWidget and the candidate;
the control evidence prevents mislabeling it as an introduced regression.
Additional dialog/preview work has separate memory observations and must not
replace the frozen comparison. Small popup buffers can remain; this change does
not claim to release every Qt/platform allocation.

Hosted headless CI can run the contracts and preserve JUnit/identity artifacts.
Real compositor evidence requires a graphical lab or appropriate self-hosted job.
No cloud credentials or real user payloads are needed for these memory checks.
