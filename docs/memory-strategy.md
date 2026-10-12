# Resident memory strategy

The retained source63 implementation meets the 40 MiB idle resident target in the measured local workflows on Cinnamon/X11, GNOME Wayland and KDE Wayland. It keeps native fonts and dialogs, the tray, all four capture/clipboard actions, and the shared upload/history services. Full release acceptance remains pending. The [cumulative code review](reviews/2026-10-11-cumulative-review.md) records correctness and test-harness follow-up work; passing the memory workload does not close those gates. Exact measurements, workloads and limitations are in [the source63 checkpoint](evidence/kde-source63/memory-summary.md).

## Keep the listener small; end graphical work

The resident owns shortcuts, clipboard ownership, the tray/menu, operation recovery and the private command endpoint. Recognized native sessions use `QCoreApplication`; ambiguous environments retain the graphical fallback and capability checks. This avoids initializing a graphical platform solely to wait for actions. It does not remove every QtGui library from the link graph.

Settings, History, Activity and About live in `putgd-ui`. The last window closing ends that process after an acknowledged bounded checkpoint. It owns neither SQLite nor storage credentials. The CLI and MCP helper forward to the same resident services. Closing a view never transfers an upload to a second engine or cancels an accepted operation.

Cinnamon acquisition uses transient `putgd-capture`; KDE region selection uses transient `putgd-kde-region`. The KDE selector returns geometry only, and the resident retains KWin authorization and pixel capture. GNOME retains its Shell bridge. Selection stays transparent over a live desktop. Startup, control frames, selection and shutdown are bounded; accepted results require matching build identity and normal helper exit. A helper that exits without required cleanup proof fails closed.

Sleeping a graphical process would retain its address space and stop callbacks the listener needs. Normal helper exit releases its graphical allocations without freezing clipboard, shortcuts or uploads. Retaining a hidden helper to improve reopen timings is outside this design.

## Load only the decoder the action needs

Text and ordinary file-list clipboard offers do not enumerate image codecs. PNG uses its verified eight-byte signature on the same open device to select the built-in handler. Mislabelled or other image data retains supported format detection. Generic image discovery, normalization and previews run in a temporary graphical worker when necessary, including font-dependent formats. The worker validates private file identities, limits, checksums, build identity and cancellation; the resident accepts output after normal exit.

Completed image buffers and preview widgets must drain with their operation or view. A local-only image remains encoded in the native clipboard owner while it is legitimately pasteable. Active incremental transfers may retain their bounded earlier version until completion or deadline. That live ownership is charged to the memory workload; purging it to make RSS smaller would break copy/paste. Uploaded results usually own only URL text.

Complete KDE output frames share storage when composition needs no conversion or crop. This avoids an unnecessary full-frame copy without changing output layout validation, pixel bounds or capture targets. History remains paginated and indexed; it does not preload remote content or all previews. There is no evidence that narrower integer types or blanket SQLite cache purges explain the resident savings.

## Reclaim free pages after work drains

An existing glibc-only quiet gate requests `malloc_trim(0)` once after qualifying work drains. New work stops the gate; live clipboard leases and background jobs prevent reclamation. It does not purge native fonts, widget state or owned payloads.

The KDE glibc resident sets `M_MMAP_THRESHOLD` to 128 KiB before application construction. This keeps large transient allocations from changing glibc’s adaptive allocation threshold and retaining freed heap pages. Other desktop routes and allocators keep their existing policy. The standalone allocator probe suggested a saving, but its incremental product benefit was not measured. The accepted result is cumulative across the retained boundaries, decoder changes, frame sharing and allocator policy.

## What passed

| Measured machine | Fresh cold peak RSS | Post-use observation |
| --- | --- | --- |
| Physical Cinnamon/X11 | 24.762 MiB | 25.004–25.293 MiB settled after three view cycles |
| GNOME Wayland VM | 27.004 MiB | 32.809 MiB peak in each of two five-minute local-native runs |
| KDE Wayland VM | 27.914 MiB | 30.051 MiB peak in each of two five-minute local-native runs |

Cinnamon’s settled view values are not five-minute post-native peaks. The matched KDE source57 post-use baseline was 139.422 MiB; the integrated source63 result was 30.051 MiB, a 78.446% reduction. Diagnostic predecessors and standalone probes cannot establish isolated savings. The measured binaries are version `0.1.1-dev.63+g050b702c6a6d`; later documentation commits do not retest or relabel them.

The local checks cover native captures/clipboard, tray/menu, native fonts/dialogs, Settings save/cancel/reload, local History previews, independent Activity paste and normal helper exits. They do not establish current real guest S3 upload, remote History copy/delete, post-S3 memory or full U9/fresh-agent release acceptance. Arbitrary large owned clipboard payloads and optional codec workloads have separate resource limits; the 40 MiB result is not a universal ceiling for every open or active state.

## Repeatable optimization and acceptance

The local results above do not establish post-S3 residency. Issue #7's actual
live-S3 Cinnamon workload measured 49.445 MiB on source102. Source104 releases
inactive S3 drivers, clients and runtime on their original owner worker, after
accepted requests drain. It keeps SDK libraries pinned rather than unloading
code under existing callbacks. SDK destruction runs outside the admission lock,
so a new action or diagnostics request does not wait on shutdown while holding
that lock.

Two independent source104 runs measured 47.879 and 46.996 MiB after 30 operations
and 31 verified objects each. Both passed native/public-byte/link/cleanup gates;
the worse run saves 1.566 MiB. This useful cumulative change still misses 40 MiB.
Zero idle driver/client counters do not mean their mapped libraries disappeared.
The next approved candidate isolates SDK work in a temporary process; it is an
experiment until its safety, normal-exit and whole-family measurements pass.
See [live-S3 evidence](evidence/live-s3-memory/README.md) and
[the repeatable protocol](testing-live-s3-memory.md).

1. Freeze a clean candidate and save its commit, tree, generated version, compiled manifest and executable hashes. Build with the documented CMake presets, Ninja, distribution Qt and official AWS SDK.
2. Attribute the actual workload before changing code. Separate private heap, shared mappings, loaded codecs and legitimate retained clipboard bytes. A mapped library is not a removable-byte forecast.
3. Preserve approved features and compare the combined candidate against the matched baseline. Changes may accumulate; do not invent per-change savings from different diagnostic workloads.
4. Test one Cinnamon build first for ordinary candidates. The approved KDE campaign used KDE-first iteration while keeping passing sibling implementations; only after KDE was satisfactory did current-build Cinnamon/GNOME regressions run. Preserve that distinction when interpreting the saved evidence.
5. Use private synthetic profiles and fresh overlays of immutable golden disks. Preflight every resolved Unix socket with `tools/diagnostics/check_socket_paths.py`. Keep graphical interaction visible on the VNC wall and preserve failed attempts.
6. Charge the complete product family, including transient, escaped or reparented helpers. Verify unchanged process/executable identity, helper drain, sample coverage, peak RSS and one-core idle CPU. Run repeated independent post-use windows and a separate cold profile. Do not select only favorable runs or weaken a frozen harness.
7. Recompute structured samples independently, persist the result marker and experiment log, and record acceptance limits before advancing. [Saved-evidence replay](evidence/kde-source63/reproduction.md) uses Python’s standard library and can run in CI; it verifies records, not a new compositor.

Detailed contracts: [architecture](architecture.md), [UI process protocol](ui-process-protocol.md), [native listener](native-listener.md), [GNOME boundary](gnome-core-resident.md), [KDE boundary](kde-resident-boundary.md), [image worker](image-worker.md), [memory tests](testing-memory.md), and [lab lifecycle](vm-lab.md).
