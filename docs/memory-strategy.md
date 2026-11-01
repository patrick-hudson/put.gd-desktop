# Resident memory strategy

The idle target is **40 MiB RSS** for the resident and any surviving product
helpers after work drains. Cold idle and settled post-use memory are separate
workloads. Open windows, active encoders, in-flight uploads and an owned large
clipboard payload have different live resource needs. PSS, private memory, swap
and CPU accompany RSS so shared mappings and active work remain visible.

## Keep the event loop; release temporary processes

The resident owns shortcuts, clipboard ownership, tray/menu, shared operation and
history services, recovery and the private command endpoint. Cinnamon/X11,
GNOME Wayland and KDE Wayland use `QCoreApplication`, avoiding graphical platform
initialization simply to wait for an action. QtGui stays linked for images and
key sequences. Other session routes retain the graphical fallback described in
[architecture](architecture.md).

`putgd-ui` starts on demand for the main window and About. Closing its last
window releases the graphical address space after a bounded safe checkpoint;
accepted uploads and native clipboard service continue. Minimize retains the
window. Pausing the resident would retain its memory and stop callbacks needed
for shortcuts, clipboard transfers and operations, so the split uses process exit.

The resident keeps these costs in temporary helpers:

| Work | Owner |
| --- | --- |
| Main window, forms and bounded thumbnail cache | `putgd-ui` |
| Image editing and sanitized export | `putgd-ui --edit-image` |
| Generic image discovery, decode and previews | `putgd-ui --image-job`, offscreen |
| Cinnamon graphical acquisition | `putgd-capture` |
| KDE region surfaces | `putgd-kde-region`; authorized KWin capture stays resident |
| Recording, review, GStreamer and FFmpeg | `putgd-record` |
| AWS SDK/module and TLS | `putgd-s3-worker`, per Linux driver call |
| Native B2 HTTP; Proton SDK/account work | `putgd-b2-worker`; `putgd-proton-runtime` |
| Update network checks and AppImage installation | short-lived `putgd-cli` |

GNOME retains its authenticated Shell bridge. Native tray menus are rendered by
the desktop. File choosers run through the XDG portal. The split preserves Qt
input, accessibility, fonts and desktop integration; it does not replace Widgets.

## Bound live data and return freed pages

Known PNG readers verify the signature and disable automatic image-plugin
discovery. Other image formats and font-dependent decoding use the temporary
[image worker](image-worker.md). Qt's PNG writer remains in the resident.
History uses indexed pages and bounded previews rather than loading every row
or remote image. Uploads stream from private immutable artifacts; file size does
not require an equally sized heap buffer. Complete KDE output frames share
storage when composition needs no crop or conversion.

Finished rasters, previews and helper processes drain with their work. Clipboard
ownership is an intentional exception: local-only image bytes remain available
for independent paste, and bounded incremental transfers can retain earlier
versions until completion or deadline. Those bytes count in the measured workload.

On glibc, `applyResidentAllocatorPolicy` fixes `M_MMAP_THRESHOLD` at 128 KiB and
`M_ARENA_MAX` at one before application construction. The editor helper uses the
same policy. Large transient allocations can unmap on free; worker churn stays
in an arena whose free pages the idle cleanup can return. Other allocators retain
their own policy. After qualifying work drains, one two-second quiet gate calls
`malloc_trim(0)`. New work stops it. It preserves live clipboard data, fonts and
widgets and has no repeating idle timer.

## Measure the complete workload

Current tools and evaluators live under [tests/performance](../tests/performance/);
[tray/progress testing](testing-tray-progress.md) covers native functional workloads.
The [earlier memory record](history/testing-memory.md) preserves superseded
experiments, including the old three-window workload.
Use production Release binaries, exact source/build identities, private synthetic
profiles and short preflighted socket paths. Preserve the frozen harness and its
parameters. Count the whole product family, including transient, escaped or
reparented children; record normal helper exit rather than inferring it from
process absence. Keep cold idle, open History, closed History and post-upload
settle intervals distinct. RSS, PSS and private bytes are distinct metrics.

The [acceptance matrix](acceptance-matrix.md) defines package/desktop-specific
memory gates, including quiet idle windows. Run Cinnamon, GNOME and KDE functional
and memory rows in parallel on the same candidate, with quiet sampling on each
host after its other work ends. Physical gates use physical machines. Preserve
samples, saved visuals, failures, identities and cleanup receipts.

For a dated example, the [2026-10-31 physical native receipt](evidence/2026-10-31-native-pass/README.md#b-379-physical-m74)
measures nightly `v26.1031.3384-nightly` on Mint 22.3 Cinnamon/X11 with a synthetic
10,000-row history and loopback destinations: cold RSS 31.18 MiB, History-closed
RSS 35.30 MiB, and post-burst settled RSS 32.56 MiB, each over a 300-second window.
These are that build's scoped memory results; the receipt separately records its
functional/performance failures and other desktops' outcomes. Acceptance always
uses evidence for the exact build and desktop being assessed.
