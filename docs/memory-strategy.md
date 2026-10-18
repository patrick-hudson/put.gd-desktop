# Resident memory strategy

The retained source63 implementation meets the 40 MiB idle resident target in the measured local workflows on Cinnamon/X11, GNOME Wayland and KDE Wayland. It keeps native fonts and dialogs, the tray, all four capture/clipboard actions, and the shared upload/history services. Full release acceptance remains pending. The [cumulative code review](reviews/2026-10-11-cumulative-review.md) records correctness and test-harness follow-up work; passing the memory workload does not close those gates. Exact measurements, workloads and limitations are in [the source63 checkpoint](evidence/kde-source63/memory-summary.md).

## Keep the listener small; end graphical work

The resident owns shortcuts, clipboard ownership, the tray/menu, operation recovery and the private command endpoint. Recognized native sessions use `QCoreApplication`; ambiguous environments retain the graphical fallback and capability checks. This avoids initializing a graphical platform solely to wait for actions. It does not remove every QtGui library from the link graph.

Settings, History, Activity and About live in `putgd-ui`. The last window closing ends that process after an acknowledged bounded checkpoint. It owns neither SQLite nor storage credentials. The CLI and MCP helper forward to the same resident services. Closing a view never transfers an upload to a second engine or cancels an accepted operation.

Cinnamon acquisition uses transient `putgd-capture`; KDE region selection uses transient `putgd-kde-region`. The KDE selector returns geometry only, and the resident retains KWin authorization and pixel capture. GNOME retains its Shell bridge. Selection stays transparent over a live desktop. Startup, control frames, selection and shutdown are bounded; accepted results require matching build identity and normal helper exit. A helper that exits without required cleanup proof fails closed.

The graphical fallback resident (X11 sessions other than Cinnamon: Plasma X11,
GNOME X11, MATE, Xfce) constructs `QApplication` on Qt's built-in generic
platform theme (`src/ui/ResidentTheme.h`):
desktop-settings awareness is turned off and `QT_QPA_PLATFORMTHEME=generic`
is set for construction only. The variable is then restored, so `putgd-ui`
views and helpers keep the desktop's native theme. The resident's own visible
surfaces are unaffected:

- The tray menu and notifications are drawn by the desktop over D-Bus.
- The region overlay is custom-painted.
- Only a desktop without StatusNotifierItem support would see Qt's fallback
  menu in the plain Fusion style.

On Plasma X11 the KDE theme plugin otherwise creates an OpenGL context at
startup, mapping Qt Quick, KF6, Mesa and `libLLVM` into the idle resident. On
physical Kubuntu 26.04 / Plasma 6.6 X11 (Release `444f0bf`, 300 s, 61 samples)
the resident dropped from 116.1 to 35.4 MiB RSS, 29.4 to 10.6 MiB PSS and
23.6 to 8.6 MiB private. Plasma/Wayland (a `QCoreApplication` resident)
measured 29.0 MiB RSS either way. Evidence:
`work/verification/2026-10-17-kde-resident-theme/`.

Sleeping a graphical process would retain its address space and stop callbacks the listener needs. Normal helper exit releases its graphical allocations without freezing clipboard, shortcuts or uploads. Retaining a hidden helper to improve reopen timings is outside this design.

## Load only the decoder the action needs

Text and ordinary file-list clipboard offers do not enumerate image codecs. PNG uses its verified eight-byte signature on the same open device to select the built-in handler. Mislabelled or other image data retains supported format detection.

Name the format whenever the bytes allow it. A `QImageReader` with no format, and `QImageReader::supportedMimeTypes()`, instantiate every installed image plugin. On Fedora 44 with KDE's kimageformats that is 30 plugins and 12 MiB RSS, against nothing for `QImageReader(&device, "png")` and only the JPEG plugin (2.2 MiB) for `"jpeg"`. Qt 6.4.2 and 6.8.2 still load every plugin for a named format unless automatic detection is turned off ([measurements](image-worker.md#x11-integration)), and for every image writer; the resident's PNG readers turn detection off, but its PNG encodes stay on Qt's writer because replacing it saved only 0.2-0.4 MiB on default installs ([resident PNG readers](image-worker.md#resident-png-readers)). Until `cf52883` the GNOME adapter listed image types on every acquisition, including region captures, so the GNOME resident gained about 17 MiB on its first capture. GNOME screen captures now send only `image/png`. Clipboard imports list types once in the transient image job and decode non-PNG data there, as KDE already did. The editor helper names `png` or `jpeg` from the file signature. Image job children always use Qt's offscreen platform, which ships with Qt GUI itself, so codec work never connects to the compositor. Generic image discovery, normalization and previews run in a temporary graphical worker when necessary, including font-dependent formats. The worker validates private file identities, limits, checksums, build identity and cancellation; the resident accepts output after normal exit.

Completed image buffers and preview widgets must drain with their operation or view. A local-only image remains encoded in the native clipboard owner while it is legitimately pasteable. Active incremental transfers may retain their bounded earlier version until completion or deadline. That live ownership is charged to the memory workload; purging it to make RSS smaller would break copy/paste. Uploaded results usually own only URL text.

Complete KDE output frames share storage when composition needs no conversion or crop. This avoids an unnecessary full-frame copy without changing output layout validation, pixel bounds or capture targets. History remains paginated and indexed; it does not preload remote content or all previews. There is no evidence that narrower integer types or blanket SQLite cache purges explain the resident savings.

## Reclaim free pages after work drains

An existing glibc-only quiet gate requests `malloc_trim(0)` once after qualifying work drains. New work stops the gate; live clipboard leases and background jobs prevent reclamation. It does not purge native fonts, widget state or owned payloads.

All glibc Linux residents, including Cinnamon/X11, set `M_MMAP_THRESHOLD` to 128 KiB before application construction (`applyResidentAllocatorPolicy`). Native GNOME and KDE residents adopted it first. This keeps large transient allocations from changing glibc’s adaptive allocation threshold and retaining freed heap pages. GNOME’s chunked bridge transfers use the same policy: its redesign acceptance workload retained allocation arenas after the ordinary idle cleanup. A matched same-binary diagnostic reduced anonymous retention with the fixed threshold; the diagnostic alone does not establish production acceptance. Edit & upload (#51) extended it to every route: on Cinnamon the resident otherwise ratcheted from 29 to 140 MiB across file-source edits, because freed 4K rasters and PNG handoffs stayed in per-thread arena tops that `malloc_trim` does not shrink (live heap stayed under 1 MiB). With the fixed threshold a local four-cycle 4K probe stayed at baseline; `edit_operations_integration` checks retained free heap. The editor helper (`putgd-ui --edit-image`) applies the same threshold: on Fedora 44 one 1920x1080 preview otherwise left 36 MiB of freed rasters in its heap until the editor closed. Non-glibc allocators keep their existing policy. The original KDE standalone allocator probe suggested a saving, but its incremental product benefit was not measured. The accepted result is cumulative across the retained boundaries, decoder changes, frame sharing and allocator policy.

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
4. Test the candidate's Cinnamon, GNOME and KDE builds in parallel. Earlier campaigns ran Cinnamon first, and the approved KDE campaign iterated on KDE before its Cinnamon/GNOME regressions; keep that ordering in mind when interpreting their saved evidence.
5. Use private synthetic profiles and fresh overlays of immutable golden disks. Preflight every resolved Unix socket with `tools/diagnostics/check_socket_paths.py`. Keep graphical interaction visible on the VNC wall and preserve failed attempts.
6. Charge the complete product family, including transient, escaped or reparented helpers. Verify unchanged process/executable identity, helper drain, sample coverage, peak RSS and one-core idle CPU. Run repeated independent post-use windows and a separate cold profile. Do not select only favorable runs or weaken a frozen harness.
7. Recompute structured samples independently, persist the result marker and experiment log, and record acceptance limits before advancing. [Saved-evidence replay](evidence/kde-source63/reproduction.md) uses Python’s standard library and can run in CI; it verifies records, not a new compositor.

Detailed contracts: [architecture](architecture.md), [UI process protocol](ui-process-protocol.md), [native listener](native-listener.md), [GNOME boundary](gnome-core-resident.md), [KDE boundary](kde-resident-boundary.md), [image worker](image-worker.md), [memory tests](testing-memory.md), and [lab lifecycle](vm-lab.md).

## Source106 live-S3 checkpoint

The cumulative transient SDK worker reached 28.953 and 28.852 MiB in two complete Cinnamon live-S3 workloads, versus 49.445 MiB on source102. Current GNOME/KDE native and graphical checks passed, but production SDK failures blocked both complete live workloads and KDE production deletion. The candidate remains preserved on its experiment branch; global acceptance is pending. See [the source106 checkpoint and replayable evidence](evidence/live-s3-memory/source106/README.md). This result does not replace the earlier local-only desktop results or claim current GNOME/KDE post-S3 memory acceptance.

## Source114: post-S3 memory and transport

The cumulative change meets the quiet 40 MiB live-S3 target on all three tested
Linux desktops. Measured runtime: `0.1.1-dev.114+g0258600b752d`.

| Desktop | Five-minute peak RSS + swap | Swap | CPU, one core |
| --- | --- | --- | --- |
| Cinnamon/X11 | 29.059 MiB | 0 | 0.0100% |
| GNOME/Wayland | 36.078 MiB | 0 | 0.0167% |
| KDE/Wayland | 31.285 MiB | 0 | 0.0200% |

Owner-thread idle cleanup is retained. The official SDK now runs in a transient
worker, releasing its TLS/SDK mappings on normal exit. Credentials use private
pipes; the resident keeps the operation journal and acknowledges durable
checkpoints before effects. CLI and MCP clients still use that same pipeline.
The OpenSSL 3.5 compatibility guard preserves explicit group policies,
certificate verification and hybrid negotiation while reducing the initial
predicted key share. No uncertain mutation is automatically replayed.

The matched Cinnamon source102 peak was 49.445 MiB; 29.059 MiB saves about 20.386 MiB
(41.23%). This is a cumulative result, not an isolated library/TLS-guard saving.
Each desktop passed 12 native and 8 GUI checks and the full workload of 30 operations and 31 objects,
with exact public content, native URL paste, pixel checks and production deletion.

The earlier source104/source106 paragraphs describe historical checkpoints.
Current GNOME/KDE live gates now pass. This is one source114 full regression per
desktop; the two source106 Cinnamon profiles remain separate. Open views/active
work, a new fresh-repeat optimization decision and full release/fresh-agent
acceptance are outside this claim. Later documentation commits do not relabel
the measured runtime. See [source114 evidence](evidence/live-s3-memory/source114/README.md)
and [transport diagnosis](testing-live-s3-transport.md).
