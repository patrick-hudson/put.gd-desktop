# Source63 candidate4 interim evidence

This is a nonterminal metadata projection exported from the host campaign. Source63 is `050b702c6a6da5c945ea8b4e138dee50e6fdbd83`, version `0.1.1-dev.63+g050b702c6a6d`. Exporting this document does not launch an app or test. Completed runs and pending acceptance checks are recorded below.

The host standard build passed all 51 registered CTest tests; two SVG cases skipped because that host lacks the optional codec. The KDE standard build produced the exact source63 package. Its full guest suite passed all 51 registered tests; one subprocess-only S3 helper case skipped internally, with no registered test omitted. The full host ASan suite also passed all 51 registered tests; its two SVG codec skips remain a host qualification, while the three actual KDE SVG paths passed. The first-cold diagnostic is 27.921875 MiB RSS, 6.2744140625 MiB PSS and 4.8046875 MiB private memory. It is one sequential attribution snapshot, not a five-minute result or proof of the 40 MiB target.

The original Qt smoke run passed clipboard text, active window and desktop. Its region-cancellation case failed and remains failed. The corrected timing interpretation places the last key receipt inside an approximate deadline bracket; neither receipt publication nor this bracket proves the product outcome. The actual journal now confirms operation `79246861-13be-4e44-911e-22fd0b566d77` is cancelled with `no_result` and no items. The later drain confirms certified normal-zero selector helper exit and no fixture residue. These receipts diagnose the timeout; they do not retroactively pass the original cancellation artifact and independent clipboard checks. The journal proves `items[]`. A separately recorded fresh Escape case subsequently passed the original cancellation and independent clipboard checks. The original failed case remains failed.

Candidate4 changes only `src/main.cpp`: on the Linux glibc KDE Core route, before creating the application, it fixes `M_MMAP_THRESHOLD` to 128 KiB. This aims to make larger transient allocations use mmap more often. Existing free arena chunks can still satisfy allocations, so the call alone establishes no memory saving. Cinnamon/GNOME routing and policy remain unchanged; current-version sibling regressions are still required.

`recipe-manifest.json` pins the existing recipes and frozen sampler/evaluator. Reproduce with C++20, dynamic distro Qt >=6.4.2, CMake presets, Ninja and the official AWS SDK. Normal host commands are `cmake --preset dev`, `cmake --build --preset dev`, and `ctest --preset dev`; existing completed source63 host tests are not rerun by this draft. The separately prepared Cinnamon Release driver uses both fixture and physical benchmark options and remains held. Heavy builds must finish before scored memory/CPU windows.

KDE runs first under the approved scope. Synthetic Qt/GTK fixtures use actual Ctrl+Shift+F9/F10/F11/F12 bindings; default shortcuts are not independently qualified. Require the original pixel and independent paste oracles, full12 matrix, SVG3 paths, guest51, native views/activity/history, live selector/clock/cursor and normal helper exits. Graphical actions remain visible via VNC5934 and the display wall at http://127.0.0.1:8765/. Keep Linux bases sealed; use owned overlays and normal old-before-new shutdown.

Before isolated launches, run the official checker against every complete resolved socket path, including harness suffixes:

```sh
python3 tools/diagnostics/check_socket_paths.py --ui-output SHORT_PRIVATE_RUN_1 SHORT_PRIVATE_RUN_2
python3 tools/diagnostics/check_socket_paths.py --socket COMPLETE_RUNTIME_SOCKET COMPLETE_WAYLAND_SOCKET COMPLETE_DBUS_SOCKET COMPLETE_QMP_SOCKET
```

Use new private short directories, reject UTF-8 paths over 107 bytes, and preserve failed commands. The original selected-run preflights are hash referenced in the evidence JSON. Recipe filenames with historical source numbers are provenance, not proof of the currently installed source. Current source63/root admissions and exact complete-family/resident identity guards remain required.

Only after native helpers are gone and the full profile is quiet, execute the unchanged ladder: two populated postnative runs and one independently fresh empty-profile cold run after guarded normal quit and exact profile preservation. Each uses five seconds external warmup, 300 seconds observation, five-second cadence, 61 samples, span >=299.5 seconds, maximum gap <=7.5 seconds and 420-second command bound. Charge the complete product family, require RSS <=41943040 bytes, and treat PSS as diagnostic. Compute CPU from actual clock ticks and elapsed time. No guest input/probes or heavy builds during sample windows. Never repeat merely to select noise.

Cinnamon physical functional and memory acceptance follows satisfactory KDE; GNOME follows the actual current Cinnamon gate. Remote S3/history remains pending. Preserve the previously user-adjusted KDE power/dimming qualification. Root may update this draft only from original hash-joined receipts and independently reconciled numbers. No acceptance or keeper field is true.

The tracked destinations are `docs/evidence/2026-10-11-kde-source63-candidate4.json`, `docs/evidence/kde-source63/recipe-manifest.json`, and `docs/evidence/kde-source63/reproduction.md`. These bounded metadata projections were reviewed before export. Credentials, desktop login input, payloads, pixels, clipboard contents, raw logs/config/history, full smaps and arbitrary environment remain private.

Host control-cost diagnosis remains separate: one historical authorization took 0.835 seconds and freshly hashed 345,638,106 bytes. That historical guard already streams. For the current overlay, two isolated Python pairs showed streaming SHA-256 saved about 76–99 ms and avoided about 159 MB temporary host high-water allocation compared with read_bytes, with equal digests and unchanged metadata. This establishes no product RSS saving. Actual fresh SSH calls took about 0.302 seconds for nop and 0.471 seconds for inventory. The single concurrent-inventory comparison was slower (0.16185 to 0.17038 seconds) and remained unapplied. No product changes, acceptance claims or new runtime release follow from these diagnostics.

## Archival replay and a new campaign

The 24 recipe fingerprints identify this campaign’s private, ignored `work/` scripts and receipts. They are not installed tools, are not included by this three-file documentation export, and cannot be run from a fresh clone without their original artifacts. On the existing lab, retain the original commands, root admissions, hashes, private output directories and all failed attempts. Missing original receipts must fail closed; never reconstruct an old gate or fake a passed result to make an archival wrapper run.

A fresh clone can run a new campaign using tracked `tools/lab/lab.py`, `tests/performance/bench_ui_process.py`, the production CLI, and the official socket checker. It still needs a separately provisioned sealed Linux base and protected bootstrap inputs described in `docs/vm-lab.md`; those private inputs are deliberately absent from Git. Create fresh baseline and current-build receipts, a new owned overlay, exact installed-family identities and new output paths. This is new evidence, not replay or acceptance transferred from source57/63. Preserve the recorded source/OS/Qt/glibc, power-setting and workload qualifications.

Build the chosen clean revision with the normal tools, with the native fixture enabled for lab cases:

```sh
cmake --preset release -DPUTGD_LAB_FIXTURE=ON -DPUTGD_PHYSICAL_BENCHMARKS=ON
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake -G DEB -B NEW_PRIVATE_PACKAGE_DIRECTORY
```

Do not paste an old package name, PID, socket, admission or native receipt into a new campaign. Obtain the exact package/version/manifest and current full commit from the new build. All placeholders below must be replaced with newly verified inputs; these examples have not been executed by this draft.

For each isolated native case, provision an owned overlay from a sealed base and preserve visible VNC output. The base/bootstrap files must already exist and satisfy the documented private-file guards:

```sh
python3 tools/lab/lab.py profiles
python3 tools/lab/lab.py launch --profile kde-wayland \
  --golden SEALED_KDE_BASE --run NEW_OWNED_RUN \
  --bootstrap-key PROTECTED_BOOTSTRAP_KEY --known-hosts PROTECTED_KNOWN_HOSTS \
  --ssh-port FREE_LOCAL_SSH_PORT --vnc-display FREE_LOCAL_VNC_DISPLAY
python3 tools/lab/lab.py install --run NEW_OWNED_RUN --deb EXACT_CURRENT_DEB
python3 tools/lab/lab.py cli --run NEW_OWNED_RUN --method capabilities
python3 tools/lab/lab.py cli --run NEW_OWNED_RUN --method diagnostics
python3 tools/lab/lab.py run --run NEW_OWNED_RUN \
  --scenario region-cancel-escape --clipboard-client qt --timeout 90 --keep
```

Before any isolated app/test launch, run the tracked socket checker on every resolved application/Wayland/D-Bus/QMP path, including any harness runtime suffix. Never boot a golden disk directly, reuse an occupied run or disable its VNC output. Restore a locked desktop through its visible console before graphical evidence. Old owned VMs are normally shut down and their exact generations shown absent before new ones start.

Repeat the tracked `run` command with its original independent paint/paste and cancellation assertions for the nine Qt scenarios: `region`, `window`, `desktop`, `clipboard-text`, `clipboard-image`, `clipboard-files`, `region-cancel-escape`, `region-cancel-right-click`, and `region-cancel-zero-area`. The three GTK cases are `clipboard-text`, `clipboard-image`, and `clipboard-files` with `--clipboard-client gtk`; distro Python GI/GTK4/Cairo is required by that tracked route. Use fresh overlays for case isolation as documented, record every original result and actual hotkey binding, and preserve failures. `--keep` retains the owned VM and visible output for inspection; it is not permission to discard failed results or force-reset the machine. Supplementary SVG, focused settings/activity/history and live cursor/clock proof remain separate requirements; the generic twelve-case examples do not establish those gates automatically.

Inspect actual production outcomes by operation ID without exposing content. The lab CLI forwards to the production CLI:

```sh
python3 tools/lab/lab.py cli --run NEW_OWNED_RUN --method operation.inspect \
  --params '{"id":"ACTUAL_NEW_OPERATION_ID"}'
```

For a new populated memory campaign, complete the matched native workload and view-close/fixture-drain checks, then prove current family/identity, journal ready, zero active operations, stopped UI, and no escaped helpers. Inspect content-free production diagnostics on the machine where sampling runs:

```sh
/usr/bin/putgd-cli --socket COMPLETE_CURRENT_SESSION_SOCKET diagnostics
python3 tools/diagnostics/check_socket_paths.py --socket COMPLETE_CURRENT_SESSION_SOCKET
```

On that same machine, wait five seconds externally after the verified quiet state, without input or probes during the sample window, then use the unchanged tracked existing-process sampler:

```sh
python3 tests/performance/bench_ui_process.py \
  --mode split --pid ACTUAL_CURRENT_PID --start-ticks ACTUAL_CURRENT_START_TICKS \
  --app /usr/bin/putgd --helper /usr/bin/putgd-ui \
  --cli /usr/bin/putgd-cli --socket COMPLETE_CURRENT_SESSION_SOCKET \
  --source-revision FULL_COMPILED_CURRENT_COMMIT \
  --source-archive EXACT_CURRENT_SOURCE_ARCHIVE \
  --output /tmp/NEW_SHORT_PRIVATE_OUTPUT \
  --label post-native-a1 --seconds 300 --interval 5 \
  --acceptance-receipt ACTUAL_NEW_NATIVE_ACCEPTANCE_RECEIPT
```

The five-second warmup is external in existing-process mode; passing `--warmup-seconds` would not add it to this mode. Bound the containing command to 420 seconds. Use two separately named populated observations, then a fresh empty-profile cold observation only after normal quit, exact profile preservation, new default-local configuration and a newly verified resident identity. Do not launch a second resident merely to obtain a cold number. This generic sampler verifies peer identity/build and records all observed descendants, but does not itself establish completed workload, continuously quiet UI/operations or helper absence. Those independent lifecycle/quiet proofs must accompany a scored new campaign; copying an old private receipt cannot supply them.

Check the original raw 61 rows, span >=299.5 seconds, maximum gap <=7.5 seconds, unchanged PID/start/executable/family, peak charged RSS <=41943040 bytes, and CPU from actual clock ticks and elapsed time. Preserve parameters, manifests and raw sample/result hashes. PSS remains diagnostic. Two postnative runs plus one fresh cold run are all required; no repetitions to select favorable noise. Current-source Cinnamon physical acceptance and GNOME regression evidence follow satisfactory KDE under the approved ordering. The portable commands show available tracked protocols, not completion of those pending gates.

The target is cumulative: the complete source63 candidate combines the preceding KDE Core/image-worker changes with allocator tuning. Each individual change need not reach 40 MiB. Every required functional, CPU, memory and current-version sibling gate still applies. The fresh single-case cancellation check passed separately with the original 90-second action deadline and independent paste oracle. After SVG and combined four-action coverage, one diagnostic measured 29.813 MiB RSS, 7.394 MiB PSS and 5.680 MiB private memory. This is not a five-minute observation. All twelve current-version Qt/GTK native cases subsequently passed, including the three cancellation paths and independent paste checks. Native tray, settings Save/Cancel/Reload, KDE font integration and folder dialogs, local History rendering, Activity parent/item independent paste, and normal UI/capture-helper exit have now passed. The original settings document was restored. A corrected fresh live selector retained the same operation while the clock advanced from 9:42 to 9:43 PM; its themed crosshair matched the frozen RGB and actual ARGB-alpha geometry, then Escape produced cancelled/no_result/empty_items with certified normal helper exit. Earlier setup failures and timed-out selectors remain preserved. Remote History/S3, fresh cold confirmation and current-source sibling regressions remain pending.

The first scored populated observation is exported in `post-native-a1.json` with its unchanged, content-free 61 raw rows in `post-native-a1.samples.jsonl`. It measured 31,510,528 bytes (30.051 MiB) peak RSS and 0.05333% of one core over 300.014 seconds, compared with the matched original 146,194,432-byte (139.422 MiB) baseline. This is a cumulative candidate result, not an attributed allocator-only saving. The second populated window and fresh cold window must still confirm it; this first observation does not authorize keeper acceptance. Raw configuration, payloads, credentials and full smaps are excluded from this tracked export.

A second independent populated window also peaked at 31,510,528 bytes (30.051 MiB), with 61 samples over 300.048 seconds and 0.04999% of one core. Its unchanged content-free rows and bounded metadata are exported as `post-native-a2.samples.jsonl` and `post-native-a2.json`. Both populated windows passed; the fresh cold window and current-source sibling regressions are still pending.

The exported samples can be checked without a VM, Qt, Node.js or AWS access. This checks recorded measurements; it cannot establish new desktop acceptance. Run from the repository root:

```sh
python3 - <<'PYTHON'
import json, math
from pathlib import Path
for name in ("post-native-a1", "post-native-a2"):
    metadata = json.loads(Path(f"docs/evidence/kde-source63/{name}.json").read_text())
    rows = [json.loads(line) for line in
            Path(metadata["trackedSamples"]).read_text().splitlines()]
    assert len(rows) == 61 and [r["index"] for r in rows] == list(range(61))
    span = rows[-1]["elapsedSeconds"] - rows[0]["elapsedSeconds"]
    gaps = [b["elapsedSeconds"] - a["elapsedSeconds"]
            for a, b in zip(rows, rows[1:])]
    assert span >= 299.5 and all(0 < gap <= 7.5 for gap in gaps)
    for row in rows:
        assert row["processCount"] == 1 and row["uiProcessCount"] == 0
        assert row["escapedProcessCount"] == 0
        process = row["processes"][0]
        for key in ("pid", "startTicks", "sha256", "device", "inode"):
            assert process[key] == metadata["resident"][key]
        for key in ("rssBytes", "pssBytes", "privateBytes"):
            assert row["totals"][key] == sum(p[key] for p in row["processes"])
    peak = max(r["totals"]["rssBytes"] for r in rows)
    cpu = ((rows[-1]["familyCpuTicks"] - rows[0]["familyCpuTicks"])
           / metadata["system"]["clockTicksPerSecond"] / span * 100)
    assert peak == metadata["evaluation"]["postNativePeakRssBytes"]
    assert math.isclose(cpu, metadata["evaluation"]["idleCpuPercentOneCore"], rel_tol=1e-12)
    assert peak <= 41943040 and cpu < 0.5
    print(f"{name}: {peak / 1048576:.3f} MiB, {cpu:.5f}% CPU, {len(rows)} samples")
PYTHON
```

The separately launched empty-profile cold observation also passed: 29,270,016 bytes (27.914 MiB) peak RSS, 61 samples over 299.996 seconds and 0.04667% of one core. `fresh-cold-a1.json` and `fresh-cold-a1.samples.jsonl` preserve its content-free measurement data. The old populated profile was kept intact after normal quit; the installed binaries were unchanged. History and operation counts and UI/capture-helper generations stayed zero. The three required KDE memory windows are now confirmed. Current-source Cinnamon/GNOME regressions and remote S3/history remain pending, so source63 is not yet accepted as a keeper.
