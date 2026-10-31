# Dated lab tooling and replay

These are evidence-side prototypes, not installed application changes. The original frozen C++ benchmark was **not compiled**. `b_campaign.py` calls unchanged Python memory/fixture/slow observers from frozen commit `00ab777f2413a1fbd7670a7259dc614eced896e6`; `b_native.py`, `b_history.py` and `b_tc.py` are distinct supplemental observers with per-attempt hashes. Source snapshots/failed attempts remain indexed in the private archive.

`native-local.patch` applies to the private driver checkout at base `caf52592d`. It admits **local + native + physical + current held lock + clipboard keeper only**, verifies synthetic cover before retaining native frames, and runs native local frames through strict foreign-window masking. It permits the existing synthetic fixture/keyring/payload-export helpers under that same admission. It never opens the live gate to physical placements. The unused AppImage-entry experiment is excluded. Guest behavior is retained. Applying the patch is a prerequisite for `PYTHONPATH=. python3 docs/evidence/2026-10-31-native-pass/lab/native_admission_checks.py`.

The final patched private checkout used a separate verified-pattern filename and a typed-label readback frame. Initial A workers used an earlier equivalent admission patch with a shared verified-pattern filename; its exact input archive is retained privately. Do not attribute the later diagnostic source to their earlier run.

No permissions for a new physical session follow from copying these commands. Acquire and check a new lock, confirm capacity, identify the real native session, preserve clipboard and MIME associations, prepare pinned SSH/VNC/forwarded-agent access without copying a private key, and use new short private roots. Socket preflight must include the full runtime suffix. Keep the wall visible and reject foreign/unattributed pixels before saving.

A's invocation, from a staged private driver checkout on each builder, was:

```sh
nice -n 10 python3 tools/lab/surface_driver/run.py \
  --desktop mint-cinnamon-x11 --host m74 --placement native \
  --host-profile /tmp/np31/host.json --lock-session np31-ddd3576a \
  --session-root /tmp/n31 --package deb --destination local --recipes outcomes \
  --catalog /tmp/np31/catalog --build 4d3840c6108c22546d83c43a1b7d4c77b1df9d6d \
  --out /tmp/np31/a1 --campaign native-pass --coverage /tmp/np31/a1-coverage.json
```

Other profiles: `fedora-gnome-wayland/f44/rpm`, `kubuntu-kde-wayland/k72/deb`, `aurora-kde-wayland/aurora-amd/appimage` (AppImage bin-dir `/tmp/np31/bin`). k72 a2 used root `/tmp/n32`; m74 a3 used root `/tmp/n33`, exact recipes `outcomes.edit_upload,outcomes.history_delete`, and attested user-owned uinput. Full recipe inputs/commands are in retained original command logs. Use run.py's actual `--recipes` selection format from those logs when replaying a subset.

B's wrapper expects `/tmp/np31/src` (private driver staging), `/tmp/np31/frozen-src` (unchanged frozen archive), exact downloaded DEB, `/tmp/np31/history-thresholds.json` copied from the approved 2026-10-25 policy, and `/tmp/np31/host.json`. It discovers exactly one active local Cinnamon X11 session; it never fabricates DISPLAY/session identity. It verifies installed executable/package hashes, uses private phase roots and a scoped clipboard keeper, and starts only a disposable loopback S3 fixture. No cloud input is needed.

```sh
NATIVE_OUTPUT=/tmp/npb2 NATIVE_GUARD=/tmp/bg2 \
 nice -n 10 python3 /tmp/np31/b_campaign.py \
 capture-empty seed grow capture-10k memory-10k history-10k links native-burst slow slow100

NATIVE_OUTPUT=/tmp/npb3 NATIVE_GUARD=/tmp/bg3 NATIVE_SEED_ROOT=/tmp/npb2/seed \
 nice -n 10 python3 /tmp/np31/b_campaign.py \
 capture-empty capture-10k links native-burst history-10k slow100
```

Only reuse a stopped, verified private fixture, copy it per phase, and never start the seed itself. `check_socket_paths.py --socket` was run on every complete prospective runtime socket before launch; the wrapper also retains the frozen per-phase checks. Output/guard roots must be new. The supplied wrappers contain this run's exact host/session/asset paths and are not general cross-host launchers.

The 100 KB/s shaper refuses an existing non-noqueue loopback root; only its exact disposable fixture destination port gets an nft mark. A new HTB root gives unmarked traffic a high-rate default class. Cleanup verifies its exact handle/table identity and restores the original noqueue root without flushing other nft tables. The first tc attempt's parser/class-setup failure and subsequent exact recovery are retained; counters are diagnostic and must not prevent cleanup. Never reuse its table name or delete another session's qdisc.

Light workstation validation only: 4 admission-boundary tests, 5 outcome-recipe tests and 10 clipboard tests. Direct script invocation without `PYTHONPATH=.` first failed import; corrected invocation passed. No CMake/Ninja/ctest/app compilation occurred.

The final B4 retry used the B4 source snapshots, after checking B3's clipboard and network restoration:

```sh
NATIVE_OUTPUT=/tmp/npb4 NATIVE_GUARD=/tmp/bg4 NATIVE_SEED_ROOT=/tmp/npb2/seed \
 nice -n 10 python3 /tmp/np31/b_campaign.py native-burst history-functionals
```

B4 changes only burst acquisition-drain readiness/current operation phase names and the tagged app's renamed, duplicated `Expand preview` label (require exactly one large visible label in the owned AT-SPI tree; never the small expand button). It does not repeat or replace B3's latency/search/scroll measurements. The warm capture limit is evaluated by `assess.py` against drawable paint, correcting the wrapper's extra root-pixel grading without rewriting historical receipts.
