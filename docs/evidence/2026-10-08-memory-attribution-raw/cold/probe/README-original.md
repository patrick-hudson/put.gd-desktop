# Native cold-cost locator

Preparation/locating code only. Uses distro Qt6.4.2+ Widgets, C++20, normal
CMake/Ninja and the exact current `src/ui/VisualStyle.cpp`, without changing
product code. It never instantiates product services, captures, reads clipboard
data, uploads, or opens product windows. Its tray menu has the same labels,
icons and order as `TrayController`, with inert capture/view entries.

The owned probe's visible tray icon is intentional native integration. Preserve
the Cinnamon desktop/theme/fonts and leave the menu closed during sampling.
Root coordinates each run with quiet physical measurement conditions.

```sh
cmake -S work/memory-next/cold-probe -B work/memory-next/cold-probe/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build work/memory-next/cold-probe/build --parallel 2
python3 work/memory-next/cold-probe/measure.py --binary work/memory-next/cold-probe/build/native-cold-stages --output work/memory-next/cold-probe/run-001.json
```

The external sampler reads only sanitized mapping identities and numeric
`/proc` counters for its own child. The child waits for sampler acknowledgement
between stages. Each stage settles five seconds by default; one final trim
follows the populated menu. Compare incremental RSS/PSS/private and allocator
allocated/free counters; retained stages are not independent cold comparisons.
Source/binary hashes and compiled VisualStyle digests are recorded. Fresh output
paths are required. This is not product acceptance or a measured saving.
