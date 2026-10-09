# VM memory, current builds and replay

The memory dashboard is <http://127.0.0.1:8767/>. The VNC display wall is
<http://127.0.0.1:8765/>. Keep VNC available throughout graphical interaction.
SSH metadata collection is not graphical acceptance.

## What the original audit established

The [structured audit](evidence/2026-10-08-vm-memory-audit.json) records the old
residents sampled on 2026-10-08, before the current-build functional wave.

| Guest | RSS MiB | PSS MiB | Private MiB | Process age hours |
| --- | ---: | ---: | ---: | ---: |
| KDE | 179.22 | 66.67 | 52.98 | 10.41 |
| GNOME | 111.27 | 71.11 | 63.13 | 11.93 |
| Cinnamon | 56.11 | 32.23 | 30.45 | 2.95 |

Dashboard current values matched independent `/proc` counters and exact process
identities. The collector multiplies `VmRSS` kB by 1024; the UI divides bytes by
1048576 and labels MiB. It does not sum the VM's other processes. Card peak and
average cover the saved process session, even when a narrower chart window is
selected. A sampled peak is distinct from the operating system's `VmHWM`.

All three executable files directly linked AWS S3/core, curl and Qt Network;
they preceded the current lazy S3 module and native IPC structure. Source commits
could not be reconstructed from these binaries. Their hashes, process ages and
Qt versions are retained. They cannot be compared as the same workload with the
current 44.9648 MiB physical Cinnamon cold result.

KDE mapped 58.40 MiB RSS of LLVM/Gallium but only 7.37 MiB PSS for those libraries.
RSS counts shared resident pages in full; PSS divides shared pages among users.
KWin reported llvmpipe. This explains substantial shared graphics accounting,
but does not identify the app's own rendering context, every allocation, live
versus free heap, or a leak. PSS does not replace an RSS acceptance threshold.

## Required current-source wave

1. Freeze the candidate source commit and an archive from tracked files. Record
   both hashes. Exclude local credentials, disks and unrelated working changes.
2. Build and test Cinnamon first against its native Qt and the pinned official
   AWS SDK. Run its core, widget, integration, native functional and memory gates.
   Fix any failed Cinnamon gate before building/testing GNOME or KDE. After it
   passes, their independent builds/tests may run in parallel, each with its own
   build directory and disposable VM.
3. Install into disposable overlays using normal package management. Preserve
   immutable golden disks. Record package, app, CLI, S3 module and desktop helper
   hashes, Qt/dependency versions, runner hash and the actual running PID/start
   identity. An installed file or unchanged package version is insufficient.
4. Run the nine named native scenarios: region/window/desktop, clipboard
   text/image/files, Escape/right-click/zero-area region cancellation. Verify
   saved synthetic bytes and independent native paste. Exercise settings,
   history and activity reopen; add GTK clipboard interoperability when present.
   Preserve each attempt, including failures, not only the final passing retry.
5. After the Cinnamon functional and memory gates pass, run the independent
   GNOME/KDE profiles in parallel, with sequential actions inside each VM.
   Restore locked sessions through the visible console. Before memory
   sampling, let builds and active GUI work finish. Save all blocking profiles;
   do not treat missing Wayland evidence as a Cinnamon memory pass.
6. Measure distinct cold, post-view and post-action workloads. Keep physical
   Cinnamon measurements serial and separate from VM observations. Record
   sample interval, duration, warmup, counters, identities and concurrent load.
7. Copy out explicit evidence and packages, then stop obsolete test/build VMs.
   Preserve needed SDK caches powered off. Delete a disposable overlay only
   after checking it has no needed results/build cache or dependent overlays.
   Never remove a golden disk referenced by other overlays.

`lab.py run` now requires its verified installation receipt and checks the live
`/proc/PID/exe` hash before actions and before success. It rejects zero/multiple
residents or a stale executable surviving an upgrade. An intentional history
restart records the replacement process and checks it against the same package.
This guard verifies package identity; the wave's source/archive/build receipts
establish which source produced that package. The host regression suite covers
matching, stale, missing and ambiguous identities and runs in the Linux CI jobs.
CI host contracts are not compositor acceptance.

## Replay the read-only audit

The [raw manifest](evidence/2026-10-08-vm-memory-audit-raw/manifest.json) maps the
original temporary paths to committed copies and retains their exact hashes.
The guest collector and supplemental scripts were executed for the saved audit.
`dashboard-readonly.py` was prepared from the executed inline GET protocol and
syntax-checked; it is not a second historical measurement.

First verify the chosen existing run directories with [the lab instructions](vm-lab.md).
Use current forwarded ports and pinned host keys from their manifests. Do not
boot a base, restart an app, change settings or weaken SSH host-key checking to
collect a snapshot. If the old run was retired, select a current running overlay
and record the new identity instead of pretending to reproduce the old process.

The original collector has a fixed output filename. **Do not execute it directly
and overwrite a prior receipt.** The following preparation creates a unique run
folder and changes only its two host filesystem paths. Run from the repository:

```sh
python3 - <<'PYREPLAY'
import hashlib, json, pathlib, tempfile
root = pathlib.Path.cwd().resolve()
raw = root / 'docs/evidence/2026-10-08-vm-memory-audit-raw'
manifest = json.loads((raw / 'manifest.json').read_text())
for item in manifest['files']:
    assert hashlib.sha256((raw / item['path']).read_bytes()).hexdigest() == item['sha256']
parent = root / 'work/vm-memory-audit'
parent.mkdir(parents=True, exist_ok=True)
out = pathlib.Path(tempfile.mkdtemp(prefix='run-', dir=parent))
source = (raw / 'guest-collect-original.py').read_text()
replacements = {
    "HOST_ROOT=pathlib.Path('/home/patrick/work/put.gd')":
        f'HOST_ROOT=pathlib.Path({str(root)!r})',
    "pathlib.Path('/tmp/putgd-vm-memory-audit.json')":
        f'pathlib.Path({str(out / "guest.json")!r})',
}
for old, new in replacements.items():
    assert source.count(old) == 1
    source = source.replace(old, new)
script = out / 'collect.py'
script.write_text(source)
(out / 'replay-identity.json').write_text(json.dumps({
    'originalCollectorSha256': hashlib.sha256((raw / 'guest-collect-original.py').read_bytes()).hexdigest(),
    'replayCollectorSha256': hashlib.sha256(script.read_bytes()).hexdigest(),
    'changes': 'Host repository and output paths only; guest probe unchanged'
}, indent=2) + '\n')
print(out)
PYREPLAY
```

Use that printed directory in the next commands, replacing run names with the
current overlays selected above. Do not use the angle-bracket placeholders literally:

```sh
python3 <output-directory>/collect.py <gnome-run> <kde-run> <cinnamon-run>
python3 docs/evidence/2026-10-08-vm-memory-audit-raw/dashboard-readonly.py \
  --output <output-directory>/dashboard.json \
  --sources lab/<gnome-run> lab/<kde-run> lab/<cinnamon-run>
```

The collector retains each attempted guest read. Check every `sshExitCode`, process
error and PID/start identity; a top-level script exit alone does not prove success.
Its embedded `references` are historical values, not replay acceptance criteria.
The dashboard helper refuses to overwrite existing evidence. It requires the local
8767 dashboard and its source at `work/memory-monitor/index.html` to compare HTML.
A sandbox networking denial is a recorded failed attempt, not a zero-memory result.

For build and renderer details, send `guest-build-check.py` and (KDE only)
`guest-renderer-check.py` through the same strictly pinned SSH connection with
`python3 -` and the script on stdin. The saved guest audit contains exact executed
argv per run under `sshReadOnlyReplay`; reconstruct ports/paths from current
manifests for new runs. Save stdout as separate JSON, check remote exit status,
and match PID/start identity to the main snapshot before combining results.

Retain only sanitized JSON, script/source hashes, commands and counter summaries.
Do not export keys, complete VM directories, guest environment, configuration,
clipboard content, arbitrary mapped filenames or payloads. Counter reads are not
atomic; preserve timestamps and report mismatches rather than forcing agreement.

## Repeat a current-build post-functional observation

### Recorded current-build result

All three native profiles tested source `a3d70dc2a6c6866fc6e38dd5e001553fb5c157a2`.
Each passed nine Qt scenarios and three GTK clipboard scenarios, followed by or
interleaved with two open/close cycles of Settings, History and Activity. GNOME
also passed a live-background region check: its 80 × 60 capture contained all
4,800 pixels of the updated background and none of the selection outline.
GNOME and KDE each passed a fresh 28/28 Dev suite. Cinnamon reused the earlier
full Dev/sanitizer evidence for the unchanged product source and received fresh
installed-session tests. These are local-only, single-display cases.

After all builds and GUI actions finished, the same sampler observed each
resident concurrently for 120 seconds, after five seconds of warmup, at five-second
intervals. All 25 RSS samples were unchanged in each process; swap was zero.

| Current guest, after functional tests | RSS MiB | PSS MiB | Private MiB | Idle CPU, % of one core |
| --- | ---: | ---: | ---: | ---: |
| Cinnamon | 83.30 | 54.51 | 48.22 | 0.0083 |
| GNOME | 114.36 | 71.58 | 54.79 | 0.0083 |
| KDE | 186.17 | 64.35 | 37.40 | 0.0167 |

The large KDE RSS remains on current code. Of its 186.17 MiB, 148.77 MiB is
shared resident memory counted in full by RSS. That is real accounting, not a
dashboard conversion error. The counters do not attribute every allocation or
prove absence of leaks. Keep RSS as the requested budget metric; PSS and private
memory explain it rather than replace it. These used VM sessions are not paired
comparisons with older guests or with the separate 44.9648 MiB physical Cinnamon
cold run. The approximately 40 MiB target remains unmet.

The first Cinnamon GTK test exposed a runner bug: it reused the replaced Qt
fixture's X11 position. Independent QMP geometry fixed it, and all three GTK
cases passed on retry. KDE's first GTK text attempt lacked the normal GI Cairo
binding; it passed after installing the guest package. Both failures remain in
the receipts. The lab's host suite now passes 11/11, including rejection of stale
residents and invalidation of an earlier package receipt when reinstall fails.
The CI step was executed locally with JSON/log output; no new hosted Actions run
is claimed.

Old host QEMU PIDs 1794904, 1691093 and 1673704 were stopped, with an independent
host-namespace check confirming their absence. Their writable disks retain prior
evidence and SDK/build caches offline. Golden disks were not altered. Do not use
a sandbox's restricted `/proc` to infer that a host VM exited.

### Replay

The current-source wave is recorded separately in
[evidence/2026-10-08-current-desktop-matrix.json](evidence/2026-10-08-current-desktop-matrix.json).
Use its archive/source identity, build recipes and per-case parameters to prepare
a new wave. Do not reuse an old passing test ID after changing the package.
Run all 12 scenarios and the visible view smoke on each profile first; inspect
all failed attempts and their resolutions. Verify `lab.verify_running_app(run)`
still matches the running identity in the successful scenarios, and close the
owned synthetic fixtures. Do not close the background resident or change native
fonts/dialogs. Wait for builds and other graphical tests to finish.

The saved guest sampler reads process identity, `/proc/stat` and
`smaps_rollup`; it performs no GUI, configuration, payload or network operation.
Its exact source is under the current wave's raw directory. Each invocation takes
`--pid`, `--start-ticks`, `--sha256`, `--seconds 120` and `--interval 5`. It waits
five seconds before the first sample and verifies executable identity before and
after sampling. Stream that source through `lab.ssh_args(run)` plus
`['python3', '-', ...arguments]`, saving stdout into a new JSON path. Use a 155-second
host timeout and check every remote exit code. Run the three independent SSH
observations concurrently, with no builds or GUI actions overlapping them.

Record the source/archive, runner and guest sampler hashes, profile, package,
PID/start/executable identity, VNC port, vCPUs/VM memory, operation count, scenario
IDs and timestamps before launching. Preserve every RSS/PSS/private/shared sample,
CPU ticks and clock tick rate. Check the same resident identity again afterward.
Keep stdout/stderr receipts and the exact argument vectors. Do not use an observed
RSS decrease between older and newer workloads as an optimization percentage.
GNOME's extra live-background region check is explicitly recorded as one additional
operation; all of these are post-use VM observations, not the untouched physical
cold baseline or proof of an absence of leaks.

The raw manifest maps historical working paths to committed copies. It includes
every attempted scenario's JSON/JUnit, the test event journals, build recipes and
logs, memory samples, selected synthetic PNG witnesses and review receipts.
Large raw PPM witnesses and synthetic saved payloads remain at their local paths
with hashes in the manifest; they are not silently represented as committed
binary evidence. Hash-check the committed copies before replay. Create new run
and output directories rather than overwriting a historical receipt.
