# put.gd today

put.gd is a Linux desktop capture and sharing app. It captures a region, window
or desktop, uploads clipboard images, text and local files, and accepts files or
folders from a file manager. It can edit a still image or record the screen,
review the result privately, then upload only after an explicit action. Output
can stay local, go to S3-compatible storage, Backblaze B2 or Proton Drive, and
appear in searchable local history with the resulting link copied to the
clipboard. Global shortcuts and the tray keep these actions available while
the main window is closed. See the [user guide](user-guide.md) for everyday use.

Windows, Flatpak and OCR are out of scope. Packages are DEB, RPM and AppImage. Retired experiments and implementation checkpoints live
in [history](history/README.md).

## Processes and ownership

One resident owns accepted work. UI windows and agent clients call that engine;
closing a window does not cancel an accepted upload. The implementation is
C++20 and Qt 6 Widgets (Qt API floor 6.4.2), CMake presets and Ninja. Native
packages use dynamically linked distribution Qt. See
[architecture](architecture.md), [build](build.md) and [memory strategy](memory-strategy.md).

| Executable | Role | Links in and out | Where it runs |
| --- | --- | --- | --- |
| `putgd` | Resident: tray, shortcuts, source adapters, settings, operations, history, setup, recording coordination and updates | Native desktop APIs; private JSON socket for clients; private helper pipes | User's graphical login session; Qt Core on Cinnamon/X11, GNOME Wayland and KDE Wayland |
| `putgd-ui` | Main window and About; separate `--edit-image` editor and `--image-job` codec modes | Window helper uses the session socket plus supervised control pipes; editor/image jobs use private pipes and staging files | On demand; image jobs use Qt offscreen |
| `putgd-cli` | JSON client; update-check and AppImage-install modes | Session socket for app methods; HTTPS for update modes | Same user/session, or a resident-launched temporary update child |
| `putgd-capture` | Frozen-target X11 selection and acquisition | Resident control pipes and private PNG output; X11/Cinnamon capture APIs | Temporary graphical helper on the Cinnamon Core route |
| `putgd-kde-region` | KDE region selector | Private control pipes; geometry out, optional frozen background in | Temporary native Plasma Wayland helper |
| `putgd-record` | Recording, overlays, private review, previews and export | Resident control pipes; X11 or Wayland/PipeWire acquisition; private media files | Temporary recording session; also bounded poster jobs |
| `putgd-s3-worker` | Official AWS C++ SDK requests and private S3 module | Private framed pipes, durable-checkpoint acknowledgements and credential requests; S3 HTTPS | New Linux process per driver call |
| `putgd-b2-worker` | Native B2 HTTP and transfer work | Private newline JSON input, progress, checkpoints and result; B2 HTTPS | New process per request |
| `putgd-proton-runtime` | Packaged official-SDK integration for Proton account, file and sharing work | Private helper protocol, secret service and Proton APIs | On demand from the Proton provider; separately provisioned package input |
| `putgd-ui-catalog` | Synthetic UI reference renderer | Test fixtures to rendered reference images | Test build/CI, outside the resident |
| `putgd-desktop-fixture` | Independent synthetic capture/paste witness | Lab commands, synthetic windows and artifacts | Opt-in development lab package |

Executable definitions: [CMakeLists.txt](../CMakeLists.txt), [B2 rules](../cmake/B2.cmake),
[Proton packaging](../cmake/Proton.cmake) and
[Linux packaging](../packaging/linux/CMakeLists.txt). Recording is built when
`PUTGD_RECORDING` and its GStreamer/FFmpeg dependencies are available; shipping
packages include it. `PUTGD_LAB_FIXTURE` enables the lab fixture. Catalog rendering
belongs to test builds. Helpers verify matching builds rather than mixing installed versions. Other or ambiguous desktop routes retain a graphical resident
fallback; the environment guards are in [NativeSession.h](../src/platform/NativeSession.h).

## Capture and upload pipeline

Acquire → immutable artifact → archive/upload → history → link copy is the shared
path for finalized content. Its owners are:

| Step | Owner and contract |
| --- | --- |
| Accept and acquire | `OperationService` snapshots settings, destination and credential references and journals acceptance; the native `SourceAdapter` freezes target/clipboard identity. Pixel acquisition follows durable authorization. The caller retains the returned operation ID. |
| Commit immutable bytes | `ArtifactStore` writes private, hash-verified spools and a recoverable batch manifest. Every batch item is retained before publication begins. |
| Save locally and publish | `LocalArchive` makes atomic user-owned copies; naming and upload scheduling stay in `OperationService`. `StorageProvider` performs publication and returns an opaque receipt. Local-save and upload outcomes are independent. |
| Commit history | `HistoryStore` commits the operation and item ledger transactionally in SQLite. `HistoryService` queries, copies, manages links and prepares exact remote deletion effects. |
| Copy the result | `OperationService` serializes clipboard commits through the native adapter. Confirmed links copy after durable checkpoints, with batch ordering and user clipboard revisions respected. |

Clipboard input prefers local file URLs, then image MIME, then text; a text URL
is uploaded as text, never fetched. File-manager selections use the same engine,
with folder ZIP or individual-file handling described in [file-manager upload](file-manager-upload.md). Upload concurrency and retained
artifact limits come from the accepted configuration.

The [image editor](image-editor.md) keeps its source private and submits only a sanitized PNG. [Recording](recording.md) keeps its working clip private through
Stop, trim, masks, preview and Save a copy; only Upload admits an export to the
pipeline. Restart never resumes or publishes an unsubmitted edit or recording.
Retry uses retained bytes and saved destination identity, reconciles unknown
publication first and skips completed uploads. Copy failure never causes reupload.
See [architecture](architecture.md) for journal, clipboard and deletion semantics.

## Desktop adapters

Capture, shortcut and clipboard capabilities are reported separately. Session
routing and native connection/protocol checks determine available actions.

| Desktop | Integration and requirements | Details |
| --- | --- | --- |
| Cinnamon/X11 | Native XCB clipboard/shortcuts and frozen window/output identities; RandR and XFixes. The temporary capture helper uses Cinnamon's compositor capture service and native region overlays. Window capture includes visible occlusion. | [Cinnamon/X11](desktops/cinnamon.md) |
| GNOME Wayland | Matching enabled `putgd@put.gd` Shell extension, session D-Bus and the supported Shell/GJS runtime. The extension owns native selection, pixels, shortcuts and clipboard transfer; the resident authenticates the bridge. A StatusNotifier host such as AppIndicators displays the tray. Extension upgrades require a fresh login to load the installed version. | [GNOME](desktops/gnome.md) |
| KDE Wayland | KWin ScreenShot2 and an executable-matching restricted-interface desktop entry; KGlobalAccel; ext-data-control or its supported wlr predecessor; native output metadata. Region selection needs the per-window LayerShellQt API. The Qt 6.4 compatibility build reports that selector unavailable. | [Plasma](desktops/kde.md) |

GNOME and KDE residents keep independent native integrations without initializing
Qt's graphical platform. KDE retains screenshot authorization in the resident;
its selector returns a rectangle. GNOME delegates compositor-owned work to Shell.
Other X11 sessions use the graphical X11 route. Required test coverage includes
Mint, Ubuntu, Kubuntu, Fedora and Debian, with real Cinnamon, GNOME/KDE and
X11/Wayland combinations specified by the [acceptance matrix](acceptance-matrix.md).

## Storage providers

| Provider | Current behavior | Details |
| --- | --- | --- |
| S3 (`s3`) | Official AWS C++ SDK; AWS and compatible services with the required conditional-write semantics. Destination settings select bucket, endpoint, naming and URL behavior. Exact receipts and guarded version-aware removal preserve object identity. | [S3](s3.md) |
| Backblaze B2 (`backblaze-b2`) | Native b2api v3, public or expiring authorized links, version-ID receipts and removal. Collision handling includes a documented concurrent-name exposure window; explicit overwrite is refused. | [B2](b2.md) |
| Proton Drive (`proton-drive`) | Privately packaged official-SDK helper; browser account connection, selected My files folder, private uploads or explicit share links, optional link expiry/password. Third-party integration maintained by put.gd. | [Proton Drive](proton-drive.md) |

`StorageProvider` and `ProviderRegistry` keep core, UI, history and desktop code
provider-neutral. Capabilities describe supported safeguards and effects;
receipts retain original destinations, credential references and opaque object
identities separately from URLs. Unknown outcomes remain unknown until explicit
reconciliation. Deletion uses a prepared acknowledgement of exact IDs/effects.
See the [provider contract](storage-providers.md).

## User interface

The main window has History, Destinations and Settings pages with shared capture,
clipboard, editor and recording actions. Activity is a compatibility name for
History. History has list/gallery views, filters, paging, previews and item
management. Destinations provides provider-specific setup; ordinary settings
save through a revision-checked path, while credentials require explicit storage.
The resident tray exposes actions, upload progress, notifications and window access.

The editor and recording review have on-demand windows and explicit Upload actions.
[Portal file dialogs](file-dialogs.md) select files and local exports. App-owned surfaces share `putgd::Ui`, `Theme` tokens, IBM Plex fonts and
IconArt glyphs. System, dark and light appearance use one persisted preference;
open windows follow changes. The tray follows the panel tone. See
[visual language](visual-language.md), [appearance](appearance.md) and the
[UI catalog](ui-catalog.md), whose synthetic reference images are compared in CI.

## Agent interfaces

`LocalServer` exposes schema-1 newline JSON on the owner-only Unix socket at
`$XDG_RUNTIME_DIR/putgd/session.sock`, with no TCP listener. `putgd-cli` forwards
methods to that resident. The optional Python 3.12+ `putgd-mcp` helper uses the
official MCP SDK over stdio and calls the production CLI as an argument vector.
None owns a second capture or storage engine. Inspect capabilities and current
configuration, keep operation IDs, and inspect uncertain effects before retrying.
The interfaces cover editor, recording, history, configuration, diagnostics and UI controls. See [CLI](cli.md), [MCP](mcp.md), [agent workflow](agents.md) and [UI protocol](ui-process-protocol.md).

## Configuration and private storage

| Data | Default Linux location |
| --- | --- |
| Configuration | `$XDG_CONFIG_HOME/putgd/config.json`, normally `~/.config/putgd/config.json` |
| Journal, artifacts, previews and private staging | `$XDG_DATA_HOME/putgd/`, normally `~/.local/share/putgd/`; history is `history.sqlite` |
| Logs and update state | `$XDG_STATE_HOME/putgd/`, normally `~/.local/state/putgd/` |
| Session commands | `$XDG_RUNTIME_DIR/putgd/session.sock` |
| Credentials | OS secret store or resident-memory session cache; Proton uses its helper's secret-service namespace |

Configuration and receipts hold credential references. Secret inputs stay out of
configuration, journals, payloads, logs, diagnostics and safe UI checkpoints.
Actual roots and legacy-layout migration outcomes are reported by the resident.
See [configuration](configuration.md), [credentials](credentials.md),
[diagnostics](diagnostics.md) and [UI state](ui-process-state.md).

## Packaging, channels and updates

DEB/RPM packages install executables, private libraries/runtime helpers, desktop
entries, GNOME extension and file-manager integration from `packaging/linux/`.
The portable AppImage has AppRun and an audited runtime/media layout under
`packaging/appimage/`; file-manager entries are per-user. Package audits track
identities, dependencies, licenses, source and SBOMs. See [release](release.md),
[repositories](package-repositories.md), [build](build.md) and [CI](ci.md).

`nightly` is the integration trunk and default branch. Nightly packages publish
as GitHub prereleases, through [app.put.gd/dl](https://app.put.gd/dl) permalinks and
signed apt/dnf repositories at [pkg.put.gd](https://pkg.put.gd/). Stable promotion
uses `tools/releases/promote.py --open` from `nightly` to `main`; its tagged draft
is published manually after exact-asset acceptance. There is no candidate channel.
CalVer `YY.MDD.N` comes from the tagged commit's UTC date and full-history count;
versions are never bumped by hand. See [versioning](versioning.md) and the
[release runbook](release-runbook.md).

`UpdateService` schedules daily checks through a temporary CLI child. Repository
installs use the system updater and repository channel. An AppImage can explicitly
verify, install and restart an update, with active work guarded. See [updates](updates.md).

## Lab and acceptance

Five physical builders host builds and visible native sessions: f44 (Fedora 44
GNOME), d69 (Debian 13 Plasma), k72 (Kubuntu 26.04 Plasma), m74 (Mint 22.3 Cinnamon)
and aurora-amd (Aurora Fedora 45 Plasma). Use desktop locks with heartbeat and
verified cleanup before release; [remote builders](remote-builders.md) defines
pins, capacity, session ownership and Aurora's Toolbx build environment.

QEMU runs use fresh disposable overlays of immutable goldens stored on builders
and in private S3. VNC remains visible on the workstation wall, with saved visuals,
exact source/build/session identities and independent functional artifacts.
[Boat candidate builds](boat-candidates.md) and [snapshots](boat-snapshots.md) provide
disposable cloud build/sandbox capacity. The [surface driver](../tools/lab/surface_driver)
works native controls and independent witnesses; its frozen Tier 1 outcome suite
and [prerequisites](surface-driver-prerequisites.md) govern reproducible runs.

The automatic [nightly desktop controller](../tools/lab/nightly_desktops.py) runs
from d69's [timer](../tools/lab/putgd-nightly-desktops.timer) and posts a red/green
table to pinned [issue #1003](https://github.com/patrick-hudson/put.gd-desktop/issues/1003).
Each build needs desktop functional and memory evidence. Offscreen tests, fixtures
and earlier results establish their stated scope; hardware gates use the required physical host. See [acceptance matrix](acceptance-matrix.md),
[VM lab](vm-lab.md), [verification](verification.md) and [evidence retention](evidence-retention.md).

## Where to read next

| Document | Read it for |
| --- | --- |
| [README](../README.md) | Product introduction, downloads and quick start. |
| [AGENTS](../AGENTS.md) | Repository rules and authorized development workflow. |
| [CLAUDE](../CLAUDE.md) | Agent documentation entry points. |
| [Third-party notices](../THIRD_PARTY_NOTICES.md) | Dependency licenses and attribution. |
| [Acceptance matrix](acceptance-matrix.md) | Required distro, desktop, package and hardware gates. |
| [Agents](agents.md) | Production interfaces and agent operating procedures. |
| [Appearance](appearance.md) | Theme selection, desktop following and tray contrast. |
| [Architecture](architecture.md) | Process, operation, clipboard and deletion contracts. |
| [Aurora VM](aurora-vm.md) | Aurora golden profiles and native Plasma testing. |
| [B2](b2.md) | Native B2 setup, uploads, links and version deletion. |
| [Boat candidates](boat-candidates.md) | Exact-source candidate builds on disposable workers. |
| [Boat snapshots](boat-snapshots.md) | Reusable sandbox recipes and package targets. |
| [Build](build.md) | Dependencies, official SDK, presets and packaging. |
| [CI](ci.md) | Job selection, required checks and artifact coverage. |
| [CLI](cli.md) | Session JSON methods and client behavior. |
| [Configuration](configuration.md) | Schema, roots, migration, routing and retention. |
| [Credentials](credentials.md) | Secret-store references, setup and cache lifecycle. |
| [Cinnamon/X11](desktops/cinnamon.md) | X11 capture, clipboard and shortcut behavior. |
| [GNOME](desktops/gnome.md) | Shell extension setup and authenticated bridge. |
| [KDE](desktops/kde.md) | KWin permissions, data control and region selection. |
| [Diagnostics](diagnostics.md) | Logs, crash records and shareable redacted packages. |
| [Errors](errors.md) | Structured failures and recovery actions. |
| [Evidence retention](evidence-retention.md) | Preserve, archive and restore verification evidence. |
| [File dialogs](file-dialogs.md) | Portal chooser behavior and automation. |
| [File-manager upload](file-manager-upload.md) | Menu installation, folder handling and destination choice. |
| [Image editor](image-editor.md) | Editing tools, sanitized output and explicit submission. |
| [Image worker](image-worker.md) | Private codec jobs, file validation and resource bounds. |
| [Lab storage](lab-storage.md) | Storage inventory, retention and disposable-run cleanup. |
| [MCP](mcp.md) | Locked Python helper setup and tool contracts. |
| [Memory strategy](memory-strategy.md) | Resident budget, process lifetimes and measurement. |
| [Neon VM](neon-vm.md) | Released and development Plasma comparison profiles. |
| [Package repositories](package-repositories.md) | Signed apt/dnf installation and channel changes. |
| [Proton Drive](proton-drive.md) | Account/folder setup, private uploads and sharing. |
| [README sources](readme/sources.md) | Provenance of product images used in the README. |
| [Recording](recording.md) | Acquisition, private review, audio, edits and export. |
| [Release runbook](release-runbook.md) | Ordered promotion and publication procedure. |
| [Release](release.md) | Channels, package assets and source distribution. |
| [Remote builders](remote-builders.md) | Host access, native sessions and desktop locks. |
| [Remote goldens](remote-golden-images.md) | Locate, verify, transfer and restore immutable images. |
| [Remote displays](remote-vm-displays.md) | Loopback VNC tunnels and display-wall exposure. |
| [S3](s3.md) | SDK transport, compatible services and safe object operations. |
| [Storage providers](storage-providers.md) | Provider-neutral capabilities, receipts and checkpoints. |
| [Surface prerequisites](surface-driver-prerequisites.md) | Probe setup, admission and restoration rules. |
| [Testing](testing.md) | Test suites and verification entry points. |
| [Shortcut dispatch](testing-shortcut-dispatch.md) | Rebinding, input and native shortcut checks. |
| [Tray progress](testing-tray-progress.md) | Native progress, concurrency and notification recipes. |
| [ThreadSanitizer](thread-sanitizer.md) | Instrumented dependency setup and thread checks. |
| [Troubleshooting](troubleshooting.md) | Diagnose common session and storage problems. |
| [UI catalog](ui-catalog.md) | Render and compare synthetic UI reference states. |
| [UI protocol](ui-process-protocol.md) | Session/control transports and helper lifecycle. |
| [UI state](ui-process-state.md) | Safe RAM checkpoints and state classification. |
| [Updates](updates.md) | Check scheduling, channel choice and AppImage installation. |
| [User guide](user-guide.md) | Capture, sharing, history and settings workflows. |
| [Verification](verification.md) | Evidence inventory and acceptance requirements. |
| [Versioning](versioning.md) | Commit-derived calendar versions and build identities. |
| [Visual language](visual-language.md) | Shared controls, tokens, typography and UI behavior. |
| [VM lab](vm-lab.md) | Golden protection, disposable runs and cleanup receipts. |
