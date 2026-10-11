# Cold native Qt memory attribution, 2026-10-08

This bundle preserves one completed physical Cinnamon locating run and its exact
probe/sampler/build inputs. Product source was unchanged. It is not a product
saving, a statistical confidence interval, graphical acceptance, or proof of the
40 MiB target. The product baseline is 44.96484375 MiB from the existing
five-minute receipt; the standalone seven-stage probe ended at 41.3671875 MiB
after one trim. These are different workloads.

The probe starts with native QApplication, then applies the exact current custom
VisualStyle, sets its brand icon, shows its own tray icon, assigns an empty menu,
populates the current tray labels/icons/order, and finally trims once. Each
stage settles for five seconds. An external sampler reads numeric /proc counters
and sanitized mapping identities while the child waits for acknowledgement.
The menu stayed closed. No product services, windows, capture, clipboard,
credentials, configuration, uploads, or cloud work were exercised.

The empty menu added 1.1171875 MiB before trim and its actions/icons added
0.47265625 MiB. The final trim released 1.1015625 MiB of allocator-owned pages
while the live menu remained. Sequential increments cannot be subtracted from
an already-trimmed product workload to predict a production saving. The next
suggested locator is an independently started, equally aged, once-trimmed
tray-only versus full-menu comparison; it has not been run.

## Contents and provenance

`manifest.json` maps copied bundle paths to exact original absolute paths and
SHA256 values. `build-identity.json` is the original unmodified receipt: its
absolute original paths and binary hash are historical provenance, and the
binary is deliberately absent. `raw/run-001.json` preserves every stage and
mapping counter. `cold-analysis.json` retains original evidence references;
corresponding raw comparison inputs are copied into `baseline/`.
`consumed-source/src/ui/VisualStyle.cpp` and `.h` are exact compiled snapshots;
`TrayController.cpp` only supplied the current menu order and is not compiled.
`probe/README-original.md` retains the original preparation instructions.

No full configure/build stdout log was retained; the original receipt records
the exact successful commands, compiler/Qt identity, source digests and ELF
identity. There are no binaries, object files, CMake caches, private inputs or
raw process memory in this bundle. `secret-scan.json` records a count-only scan
of this explicit exported file set. No credentials.txt was opened.

## Replay in a fresh work directory

Use the standard existing C++20/CMake/Ninja/distro Qt6 Widgets toolchain (minimum
Qt6.4.2). Missing system packages require the normal administrator installation
path; do not assemble a substitute toolchain. Original build used GNU13.3.0,
CMake3.28.3, Qt6.4.2, Release `-O3 -DNDEBUG`, dynamically linked distro Qt.
A different compiler or fresh directory may change the ELF hash; the original
binary/source digests remain the recorded identity, not a reproducible-binary
promise.

The unchanged CMake source computes its source root as `../../..`. Recreate
that hierarchy in a fresh isolated tree, then copy the consumed VisualStyle
there. Do not use or overwrite the current product source to satisfy this path.
The following prepares and builds the probe; it does not run it:

```sh
BUNDLE=/home/patrick/work/put.gd/docs/evidence/2026-10-08-memory-attribution-raw/cold
REPLAY_ROOT=$(mktemp -d /home/patrick/work/put.gd/work/memory-next/cold-replay-XXXXXX)
mkdir -p "$REPLAY_ROOT/work/memory-next/cold-probe" "$REPLAY_ROOT/src/ui"
cp "$BUNDLE/probe/CMakeLists.txt" "$BUNDLE/probe/main.cpp" "$BUNDLE/probe/measure.py" "$REPLAY_ROOT/work/memory-next/cold-probe/"
cp "$BUNDLE/consumed-source/src/ui/VisualStyle.cpp" "$BUNDLE/consumed-source/src/ui/VisualStyle.h" "$REPLAY_ROOT/src/ui/"
cd "$REPLAY_ROOT"
sha256sum -c <<'HASHES'
2acfaf649c0491857ff9e9b11dcadf25946484251c85509a6e1ec86c09d21b07  work/memory-next/cold-probe/main.cpp
4f5de79097233d667c82785bfdb6ad0a30d10fd97f22b9d9fb152d09fa2f1de7  work/memory-next/cold-probe/CMakeLists.txt
fccb647f8fa8a6c9feb7a255901034c5a786345132d4b5b58aa6b6511122f3b7  work/memory-next/cold-probe/measure.py
b1d5a0662916b3899245e72ab65f5b8c0c1d8c2b00df1335fa0493f9dc3f56e8  src/ui/VisualStyle.cpp
abcdf4c74f4a53619c86a4dea4a573a5efe3381fe76d5f817fdf8b0608ec8891  src/ui/VisualStyle.h
HASHES
cmake -S work/memory-next/cold-probe -B work/memory-next/cold-probe/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build work/memory-next/cold-probe/build --parallel 2
```

Schedule a serial quiet physical run with root before executing the next command:
no overlapping product/VM builds, installs, or physical measurements. Run from
the existing native Cinnamon/X11 desktop session, with native GTK theme/fonts
and dialog integration left enabled. Do not force an offscreen platform or
replacement style. Do not open the probe menu or other windows while sampling.
The probe creates and removes its own synthetic tray/menu and quits normally.
Choose a fresh output filename; the sampler refuses overwrite:

```sh
python3 work/memory-next/cold-probe/measure.py --binary work/memory-next/cold-probe/build/native-cold-stages --output work/memory-next/cold-probe/replay-001.json --settle-ms 5000
```

Re-read the completed receipt and verify `status=complete`, `exitCode=0`, all
seven stage names, Qt version, xcb platform, native GTK mapping, compiled source
hashes and saved binary hash. The run lasts about35seconds. Preserve any future
receipt separately from `raw/run-001.json`, and release the quiet window as soon
as the process exits. No replay was executed while exporting this bundle.
