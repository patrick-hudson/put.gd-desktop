# Next resident memory attribution — source 97ca3a521714 (version 41)

Read-only review of existing results and source. No app, probe, build, SSH, VM or graphical action ran for this unit. No source/configuration changed. The JSON companion records exact evidence hashes, ranked hypotheses and acceptance controls.

Physical Cinnamon cold measured 44.39 MiB RSS (24.78 MiB PSS, 22.29 MiB private). The 40 MiB RSS target remains unmet. The cold snapshot has 2.91 MiB heap and 1.06 MiB anonymous mappings; neither means free/reclaimable pages. Startup and post-work idle cleanup already call `malloc_trim(0)` behind live-work guards.

VM quiet observations are **post-use**, after native views/captures/clipboard and a 512 MiB + 23 byte local copy; Cinnamon additionally contains the user's manual region capture. These are not matched old/new comparisons or cold budgets. Separate mapping snapshots were taken after the interval. Their PSS can change with other desktop processes sharing pages.

| Scope | RSS MiB | PSS MiB | Private MiB |
|---|---:|---:|---:|
| Physical cold | 44.39 | 24.78 | 22.29 |
| Cinnamon VM post-use | 57.65 | 37.32 | 35.91 |
| GNOME VM post-use | 65.32 | 38.77 | 35.22 |
| KDE VM post-use | 145.50 | 45.17 | 32.76 |

## Ranked next probes

1. **Locate cold resident styling costs.** `main.cpp` still applies the full settings/history stylesheet to the resident. The helper separately applies that full style. First measure application construction → full style application → icon/tray → quiescent readiness using standard isolated CMake builds and the same platform theme. If material, test resident palette plus identical menu/tooltip-only style while preserving full helper visuals. No native font or dialog change; native tray menus, actions, capture overlays and modal exclusions must remain correct. Run alternating baseline/candidate 300-second whole-family cold observations. Mapping presence provides no saving estimate.

2. **Locate KDE's first driver/theme load.** LLVM is 48.28 MiB RSS, but only 5.98 MiB PSS and 2.14 MiB private in its separate snapshot. Gallium, QtQuick/Qml and KDE theme mappings also appear. Current capture code uses translucent Widgets/layer surfaces, not explicit QQuick or OpenGL. Measure fresh current-build stages after readiness, tray interaction, helper views/native chooser close, desktop/window capture, cancelled selector and accepted selector. Keep the display on VNC, native theme and graphics configuration unchanged. Each step needs operation IDs, helper generation/normal exit and a fixed quiet period. This identifies the trigger before choosing any rendering experiment; LLVM cannot be treated as a heap leak or presumed removable RSS.

3. **Locate post-capture anonymous lifetimes.** Anonymous private footprints are 13.93 MiB Cinnamon, 8.90 MiB GNOME and 11.42 MiB KDE. Compare each against its own fresh current-build baseline; cross-machine subtraction is invalid. Instrument synthetic fixed-size image/capture stages through encoding, terminal copying and idle cleanup, then replace only the synthetic clipboard with an external synthetic owner. Record aggregate payload lease/worker/selector counts and allocator/VMA totals, not payloads or private addresses. Use ten identical cycles with 30 seconds of quiet after each, a matching no-capture control, fixed resolution/pixel format/DPR and the same final synthetic clipboard state. Preserve the settled sequence and compare final versus first within that desktop to distinguish a bounded retained buffer or allocator plateau from growth. Record frame/image/selector/cache ownership alongside live allocation and allocator free totals; one post-use snapshot cannot prove growth. The app already releases captured images after disk artifact creation; the VM checkpoint is only ~2.5 KiB. Clipboard-owned encoded data must remain available until ownership is lost.

4. **Measure SQLite/worker latency separately.** History already prepares bound queries, pages at at most 200 rows and constructs/closes a database connection per background worker. Blind page-cache purging is not a leading cold-memory fix. A synthetic populated-journal request sequence can attribute connection creation, preparation, execution and thread creation. Only test bounded statement/connection reuse if that cost dominates; retain thread ownership and explicit idle disposal, since connection reuse may trade latency for more idle memory. Preserve deletion acknowledgements and transaction safety.

## Non-starters

Blanket integer narrowing/packing lacks allocation-count evidence and risks overflow or alignment errors. Changing a type to `cstdint` does not by itself reduce size. More unmeasured periodic trimming is not a strategy. Disabling GTK/KDE fonts or native dialogs violates the user-approved experience. Clearing clipboard ownership violates paste continuity. Replacing `QApplication` or unloading Widgets is a separate native tray/overlay project: the resident currently requires `QSystemTrayIcon`, `QMenu` and X11/KDE QWidget capture surfaces. Global raster/software/EGL environment overrides need trigger evidence and native parity first.

No removable-byte or 40 MiB success prediction is made. Parent owns the next experiment selection, controlled runs and durable documentation/export.
