---
title: Qt Screenshot Sharing for Linux - Plan
type: feat
date: 2026-10-07
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-plan-bootstrap
execution: code
---

# Qt Screenshot Sharing for Linux - Plan

## Goal Capsule

Objective: A Linux user can capture a screen region, the active window, the full desktop, or clipboard content with a hotkey and paste a working public share link moments later.

Means: Qt 6 Widgets and C++20, with native desktop adapters and one shared operation layer (KTD1–KTD7).

Authority: The user’s instructions govern scope. Product requirements govern behavior; technical decisions govern mechanisms within those requirements. Implementation units and examples illustrate those contracts.

Execution: Build the Linux application, its desktop integrations, developer tooling, and release packages in the target project. This document is the implementation plan; it does not establish that any application or graphical test currently works.

Stop conditions: Do not substitute a different capture target or claim desktop support when a required integration fails. Resolve such failures before releasing that desktop profile. No changes to AWS bucket policy, account controls, or DNS are authorized by this plan.

---

## Product Contract

### Summary

Build a small resident screenshot and clipboard sharing app with a configurable native GUI and a polished, custom visual design. Its default result is an upload and a copied public URL, with S3 as the first storage provider. Provide local-only actions, optional local copies of uploads with folders by content type, persistent upload history with viewing/link copying/remote deletion, useful failure recovery, and documented CLI/MCP access to the same operations. Design the shared storage contracts for later providers.

### Problem Frame

Dropshare’s quick capture-to-link workflow is the reference. XerahS offers considerably more functionality than this product needs. The product should make frequent sharing fast while keeping configuration, desktop setup, and troubleshooting understandable.

### Key Decisions

- **Immediate sharing is the default.** Governs R5. (session-settled: user-directed — chosen over local-first capture: the user wants immediate upload and URL copying.)
- **Preserve the daily hotkey experience on Wayland.** Governs R13–R15. (session-settled: user-directed — chosen over accepting a desktop picker for every capture: maintain desktop-specific integrations where needed.)
- **Linux ships first, with future platform support preserved.** Governs R13, R26.
- **Simple setup comes first.** Governs R11, R16, R20, R35. (session-settled: user-directed — added 2026-10-08: start with a short destination setup form, then expand into advanced settings. Show human-readable file sizes. Updated by the user on 2026-10-08: use plain setting names, put detailed explanations behind info icons, expose fresh S3 fields immediately, and replace green with neutral surfaces and a restrained blue accent.)
- **Custom visual design is required.** Governs R35. (session-settled: user-directed — added 2026-10-08: the user rejects the basic Qt layout visuals and wants a custom, attractive interface.)

### Requirements

#### Capture and clipboard

- R1. Region capture shows the live desktop underneath a visually transparent selection overlay before and during dragging, with crosshairs and a rectangle outline. Windows, animations and video continue updating throughout selection. Freeze the accepted target, display layout and settings, then acquire the selected content only after the overlay is removed, with no selection outline in the saved pixels. No solid colored canvas or opaque selection fill obscures the product capture view. Synthetic performance-test backgrounds are separate, clearly labelled test fixtures.
- R2. Active-window capture targets the focused external window at invocation, including its frame where available; v1 permits visible-pixel capture on X11 and documents compositor-native occlusion behavior on Wayland.
- R3. Full-desktop capture includes every enabled monitor and desktop panels, with deterministic handling of layout gaps and per-monitor scaling.
- R4. Clipboard sharing accepts regular local files, images, and plain text, with precedence in that order; plain-text URLs are uploaded as text rather than fetched.
- R5. Each action defaults to upload immediately and copy the resulting public URL, with a configurable destination per action.
- R6. Local-only actions save to the configured local output directory and copy a suitable local result: image MIME for images/screenshots, text for text, and file URIs for local file copies.
- R7. Cancelling source acquisition publishes nothing, and acquisition/upload errors preserve the source clipboard and expose an actionable result.
- R8. Automatic result copying respects newer user clipboard changes and newer sharing invocations; skipped copying leaves an explicit copy-result action available.
- R9. File batches acquire all selected files before publication, then report individual upload results and copy successful URLs in source order when a batch partially succeeds.
- R10. Source acquisition rejects empty, unsupported, inaccessible, or excessive inputs before publication, using documented configurable resource limits.

#### Desktop experience and settings

- R11. The GUI configures shortcuts, destinations, provider-specific settings, upload naming and duplicate handling, credentials, local paths, launch-at-login, notifications, and retention/resource limits, and provides recent activity plus the persistent history under R31. Non-secret controls share the editable configuration file under R34.
- R12. Shortcut setup reports actual bindings and conflicts without silently stealing existing shortcuts; actions remain available through the GUI and CLI.
- R13. The first release covers Linux Mint 22.3 Cinnamon/X11, Ubuntu 26.04 LTS GNOME 50/Wayland, and Kubuntu 26.04 LTS Plasma 6.6/Wayland.
- R14. After one-time integration setup and OS consent, normal supported capture and clipboard hotkeys work while the app is unfocused, without a per-capture permission picker.
- R15. Settings and diagnostics report support separately for capture targets, shortcut binding, clipboard import types, and clipboard result types, including missing helpers and permission failures.

#### S3, storage, and recovery

- R16. The development S3 template defaults to bucket put.gd, region us-east-2, regional endpoint s3.us-east-2.amazonaws.com, an empty object prefix, STANDARD storage, public-read ACL, and public URL base https://put.gd. Every value is configurable through the GUI and JSON configuration file. ACL mode can explicitly omit the ACL header, and a custom public URL base is optional. These are development defaults, not a hardcoded production destination; a fresh release requires destination setup.
- R17. Existing AWS access controls are respected, with distinct errors for upload permission, ACL incompatibility, and public URL reachability; the app does not modify infrastructure to make an upload work.
- R18. Credentials use an OS secret store or an explicitly selected session-only mode, and never appear in source control, ordinary configuration, operation records, or exported diagnostics.
- R19. Failed/interrupted operations can retry their retained immutable artifacts. A candidate key may change only for a confirmed name collision under the selected policy; keys stay fixed while publication is uncertain or confirmed. Completed uploads can retry clipboard copying without another upload.
- R20. The resident app remains responsive during capture, encoding, disk access, and slow or failing uploads. Release decoded capture/clipboard payloads after immutable artifact persistence, before local saving or network transfer; retries use the retained disk artifact. History must not load an image for every row or retain full source images, and selected thumbnails are bounded and released when no longer visible. Repeated use must not grow an unbounded payload/preview heap. Native clipboard ownership can retain the result it must serve until that ownership changes.

#### Agent access and delivery

- R21. The session-local CLI exposes the four actions, operation inspection/cancellation/retry, history listing/details/link copying/remote deletion, non-secret settings, configuration path/revision/validation/reload, capabilities, and redacted diagnostics through versioned JSON.
- R22. An optional local stdio MCP server exposes the same action and inspection services without implementing another capture or upload pipeline.
- R23. Diagnostic bundles correlate operation stages and backend versions without including credentials, source pixels, clipboard content, authorization headers, or source filenames by default.
- R24. Committed documentation lets a new human or agent set up dependencies, build, test, run, package, release, and debug the app without undocumented session knowledge.
- R25. Linux releases include installable packages, dependency/license notices, checksums, and recorded graphical acceptance evidence.
- R26. Shared UI, operation, configuration, and upload code remain portable, with Windows/macOS compile checks and platform interfaces ready for later native adapters.

#### Local copies of uploads

- R27. A Keep local copies when uploading setting saves upload artifacts to a configurable default folder, with independent folder overrides for images, videos, text, and other files. In v1, videos are existing regular files; screen recording is a future feature.
- R28. When enabled, attempt local archiving before publication and report archive status independently of uploading; successful copies remain user-owned after completion, failure, or cancellation.

#### Upload filenames and duplicates

- R29. Fresh defaults preserve existing file basenames and name screenshots `Screen-Capture-yyyy-MM-dd-HH-mm-ss.png` using local action time. Clipboard images/text receive timestamped names and appropriate extensions. Advanced settings offer random names and custom filename templates with original stem/extension, date/time and UUID tokens, shared with the editable JSON configuration. Capture names apply to local saving as well as uploads. Template settings are snapshotted per accepted operation and resolved once; retries preserve names except for confirmed collision handling. Keep existing configured naming choices on upgrade. Duplicate handling remains auto-rename (default), skip or explicit overwrite. The app checks each proposed remote key, including same-batch collisions, before upload.
- R30. Duplicate handling applies to the exact bucket/prefix/object key. Auto-rename and skip never overwrite an existing object, including one created between checking and uploading. Overwrite is an explicit saved setting and uses the checked object’s ETag as a write precondition; a changed precondition or inconclusive existence check produces an actionable outcome. Retries reuse confirmed receipts and never rename an upload whose publication is still uncertain.

#### Upload history and storage providers

- R31. Maintain persistent per-item upload history across restarts, independently of retry-artifact cleanup. Users can search and filter by date, name, content type, destination, and outcome; view upload details and available image previews; open or copy the stored direct link; and see independent upload, local-copy, and remote-deletion states. A partial batch has individually actionable entries. Browsing history requires no cloud access.
- R32. Users can delete a selected history upload from its original remote destination, with explicit target/effect confirmation and individual outcomes for multi-selection. Report removed, already missing, changed/superseded, failed, uncertain, and version-retained outcomes accurately. Preserve local copies and the history record; clearing history or caches never deletes remote objects. Delete permissions or retention controls may block removal and must produce an actionable error.
- R33. Shared operations, history, GUI, CLI, and MCP use provider-neutral upload/stat/delete/capability/receipt contracts. S3 is the first implementation; adding another provider must not require rewriting capture, history, or the operation pipeline. Keep provider-specific authentication, addressing, naming constraints, versioning, and deletion semantics inside adapters and their settings panels.

#### Editable configuration

- R34. All non-secret application and provider settings live in one documented, versioned JSON file that humans and agents can edit. Valid external edits update the running app and its settings view without restart. Invalid or incomplete edits preserve the last accepted settings and show an actionable error; unsaved GUI edits survive external changes with a visible conflict. Credentials remain secret-store references or session-only values under R18. New settings do not retarget accepted operations or historical uploads.

#### Visual design

- R35. The application has a cohesive, custom, polished visual design rather than shipping the basic Qt forms as its finished interface. Design settings, setup, recent activity, history, and confirmation/error states together, with intentional typography, spacing, color, icons, navigation, and content hierarchy. Use the native Qt implementation, preserve keyboard navigation, accessible names, clear focus indicators, readable contrast, and usable layouts at supported display scales, and meet R20 and the existing performance budgets. Cosmetic completeness requires inspection of the actual rendered application; widget tests alone do not prove it. Default to a simple setup view containing the basic destination details needed to get started, with standard provider defaults and a clear Advanced settings expansion into the full existing controls. Both views edit the same validated draft/configuration; switching views preserves unsaved changes. S3 setup asks for the bucket, region, explicit credentials, and optional public URL base; endpoint/addressing, prefix, storage class, ACL, timeouts, retention and detailed resource controls belong under Advanced settings. Keep an explicit local-only setup path. A fresh installation still requires destination setup and never silently activates the development put.gd template. Size controls and ordinary summaries use readable MiB/GiB or MB/GB units rather than raw byte counts, with a consistent declared unit system; JSON retains exact byte counts and unchanged values round-trip without loss. Every editable setting has a specific, detailed help explanation reachable from an adjacent info icon by mouse and keyboard; explanations describe effect, units and relevant consequences. Keep primary labels concise, use neutral charcoal surfaces with a restrained blue accent, and make fresh S3 fields editable without an Add prerequisite. Offer searchable editable region suggestions and preserve custom regions/endpoints. Cancel discards unsaved settings and credential input and closes the window; Reload from file explicitly rereads external configuration and discards the draft. Saving gives visible in-app success feedback only after accepted persistence. The local-mode action clearly explains that all four actions save locally after Save settings.

#### Software identity

- R36. Every commit's build has a distinct, inspectable software version. Use a semantic release base plus a commit identifier; distinguish uncommitted builds. Show the compiled identity in the GUI, app/CLI version commands, diagnostics and installed package metadata. Preserve the exact source and binary identity in test evidence, including builds from exported source archives. Rebuilding after a commit refreshes identity through the normal build tools. Packaging must not relabel old binaries with a later checkout's version. Unknown archive provenance must be explicit.

#### Resident and UI lifetime

- R37. Run Settings, History, Activity and About in an on-demand UI process that can exit when its windows close. Keep a resident tray icon and menu with capture/upload actions and entries that reopen the views. Closing the UI must preserve hotkeys, clipboard ownership, accepted operations and durable recovery; Quit explicitly exits the app. Preserve native Cinnamon fonts/dialogs, safe unsaved drafts, configuration conflicts and view state across ordinary reopen. Keep credential input out of persisted drafts. UI and CLI share the resident's operation engine. Record total memory across live processes and reopen latency; moving memory between process counters does not establish a saving.

### Key Flows

- F1. Setup → configure destination/credentials → inspect desktop capabilities → install/enable integration where needed → bind shortcuts → ready. Covers R11–R18.
- F2. Invoke capture → freeze target/settings → acquire and store artifact → upload → copy result → show outcome. Covers R1–R3, R5–R8, R19.
- F3. Invoke clipboard share → snapshot supported source bytes → store artifacts → upload batch → copy ordered successful URLs → report item outcomes. Covers R4, R5, R8–R10.
- F4. Inspect a failed operation → retry retained artifact or copy existing receipt → report the new outcome. Covers R19, R21, R22.
- F5. Open history → search/filter → inspect an item/preview → open or copy its direct link. Covers R31.
- F6. Select history items → review original destination and deletion effect → explicitly delete → retain records with per-item remote outcomes and unchanged local copies. Covers R32/R33.
- F7. Human/agent edits JSON → app validates it → valid settings refresh the GUI and later actions; invalid edits or unsaved-form conflicts remain visible without losing accepted settings or drafts. Covers R11, R16, R21, R34.

### Acceptance Examples

- AE1. With another app focused, the region hotkey allows dragging a rectangle over its live, updating content; its PNG contains the latest selected pixels and no current or previous rectangle outline, is uploaded, and its public URL pastes into that other app. Covers R1, R5, R14.
- AE2. Escape during region selection leaves the clipboard unchanged and sends no upload request. Covers R7.
- AE3. A clipboard containing two local files produces two matching uploaded objects and two newline-separated links in the original order. Covers R4, R9.
- AE4. If the user copies new text during an upload, that text stays on the clipboard and the completed operation offers Copy URL. Covers R8.
- AE5. If S3 rejects public-read, the operation reports the specific configuration error and retains the artifact without changing bucket controls. Covers R17, R19.
- AE6. A local-only screenshot saves a PNG and pastes as an image, while generating zero S3 requests. Covers R6.
- AE7. With local copies enabled, a screenshot, clipboard video file, and clipboard text save into their respective configured folders while still returning S3 links. Covers R27.
- AE8. An upload failure leaves its local copy available; an unwritable archive folder reports a local-save failure while the S3 share still succeeds and offers local-save retry. Covers R28.
- AE9. With original filenames and auto-rename selected, an existing report.pdf causes the new upload to add a unique suffix, such as report-<uuid>.pdf; the original remains unchanged and the copied URL uses the final name. Covers R29/R30.
- AE10. Skip reports a duplicate without uploading it or copying an unrelated object’s URL. Overwrite replaces the checked object only when its precondition still matches; a competing write cannot turn either option into an unchecked replacement. Covers R29/R30.
- AE11. After restart and successful-artifact cache cleanup, an older image upload is searchable, shows its details and cached thumbnail when available, and copies its original direct URL. Editing the current destination does not change the record. Covers R31.
- AE12. Delete an upload from history: the selected remote object is removed, its record reports the actual outcome, and its local copy survives. A denied or uncertain delete remains visible and retryable; a known replacement at the same key is not deleted on behalf of the older entry. Covers R32.
- AE13. A test-only second provider exercises upload → receipt → history → copy link → remote delete through the same GUI/CLI services without AWS types outside the S3 adapter. Covers R33.
- AE14. With the settings window open, an agent atomically edits the JSON destination, naming policy, and image folder. The GUI shows the new values and a later action uses them; an already accepted upload and history deletion keep their original targets. Invalid JSON preserves the accepted configuration, and an unsaved GUI draft is retained with a conflict notice. Covers R16/R34.

- AE15. Open settings, recent activity, and a populated history in the installed app: they share a custom visual language, readable hierarchy, and deliberate spacing. Empty, loading, success, partial-failure, and remote-deletion states remain clear. Keyboard focus and navigation are visible, and supported scale factors do not clip content. Simple setup can configure a usable destination without visiting advanced controls; expansion preserves draft values and exposes all existing options. Size inputs/summaries show declared human-readable units and save the exact corresponding byte values. Covers R35.

- AE16. Build two successive commits with the ordinary incremental command: version inspection distinguishes them. App, CLI, diagnostics, GUI and installed package metadata agree on the compiled build. Advancing the checkout without rebuilding does not relabel the package; mismatched binaries are rejected before installation. Dirty and exported-source builds disclose their provenance. Covers R36.
- AE17. Close every UI window and verify its process exits while the tray stays usable. Invoke all four actions, use CLI/MCP, paste a copied result, and complete an already-running upload with the UI absent. Reopen each view through the tray; preserve safe drafts/conflicts/state and handle helper failure or version mismatch explicitly. Repeat on Cinnamon, GNOME and KDE with visible VNC evidence and total-process memory accounting. Covers R37.

### Success Criteria

The engineering budgets for a release build on the user’s recorded physical Cinnamon machine are p95 hotkey-to-region-overlay at or below 150 ms over 30 warm invocations, idle CPU below 0.5% of one core over five minutes, and idle app RSS at or below 100 MiB. The user subsequently requested roughly halving the measured 80.9 MiB empty-history idle RSS: target approximately 40 MiB, with measured attribution and before/after release evidence. Preserve capture, storage, custom visuals, and history behavior; this optimization runs alongside the remaining implementation and desktop acceptance work. Include attributed helper overhead separately. These are proposed acceptance budgets, not measurements; establish the hardware/display/build baseline in U4 and meet or explicitly revisit them with evidence in U10.

The complete workflow must also pass the Verification Contract on every R13 profile. Internet transfer time is reported separately from local capture, encoding, and clipboard latency.

### Scope Boundaries

V1 excludes image editing/annotation, screen recording, OCR, embedded AI chat/models, additional production storage-provider implementations, automatic remote deletion, and background clipboard monitoring that reads content without an action. Manual history deletion is included; bucket-wide browsing, cross-device history synchronization, and purging unrecorded versions are deferred.

Deferred for later: screen recording, Windows/macOS capture adapters and distributables, other Linux desktops, Flatpak/Snap/AppImage, HDR fidelity, remote MCP, folders/recursive sharing, and multipart uploads beyond the selected single-PUT limit.

Considered and not built: a custom AWS signer or custom MCP protocol stack, because maintained SDKs serve these contracts; a second VM framework, because one QEMU harness gives reproducible tests; a telemetry service, because local evidence is sufficient. An archive-success prerequisite for S3 sharing is also excluded: an independent visible save error and retained source address the failure without blocking the requested upload. Reconsider only if a measured requirement cannot be met with the selected tools.

---

## Planning Contract

### Grounding and Assumptions

The target repository is patrick-hudson/put.gd-desktop and has no application source, CMake configuration, or existing test commands. All implementation paths below are proposed additions. [Repository](https://github.com/patrick-hudson/put.gd-desktop).

The user has given standing authorization to send project code and planning documents to Anthropic for development review. Preserve that authorization in the project’s agent instructions; it does not add a runtime AI service to the application.

The observed workstation is Mint 22.3/Cinnamon using X11. Cached package candidates include Qt 6.4.2 and QtKeychain 0.14.2; Qt/CMake development dependencies are not installed. QEMU 8.2.2 is installed, but KVM is not visible to the current execution environment. Session D-Bus access was denied, so installed portal packages do not prove runtime support or absence.

The Wayland baselines are deliberate targets rather than verified integrations. Ubuntu’s release notes identify GNOME 50, and Kubuntu’s identify Plasma 6.6. Pin actual image hashes and package manifests in the lab. [Ubuntu release notes](https://documentation.ubuntu.com/release-notes/26.04/summary-for-lts-users/), [Kubuntu release notes](https://www.kubuntu.org/news/kubuntu-26-04-release-notes/).

### Key Technical Decisions

- KTD1. Use Qt 6 Widgets, C++20, and CMake presets, with a Qt 6.4.2 API compatibility floor and a separate current distro-Qt build. Keep Qt dynamically linked and use distro security updates; do not bundle an unsupported frozen Qt runtime. (session-settled: user-directed — chosen over Rust/Tauri: native C++/Qt is the selected direction.) Qt 6.4 upstream standard support has ended, so compatibility and maintenance are separate release concerns. [Qt releases](https://doc.qt.io/qt-6.10/qt-releases.html), [Qt CMake](https://doc.qt.io/qt-6/cmake-get-started.html).
- KTD2. A single resident user-session process owns typed operations. GUI, hotkeys, and a thin CLI dispatch through those services; the CLI connects through an owner-restricted filesystem socket in the runtime directory. Version and validate request/result schemas, bound message sizes, and keep JSON stdout separate from stderr logs. This prevents duplicate hotkey registrations and differing clipboard behavior. [QLocalServer](https://doc.qt.io/qt-6/qlocalserver.html).
- KTD3. Persist operation/item metadata and receipts transactionally using Qt SQL/SQLite, with one canonical versioned JSON file for non-secret settings under KTD21 and private artifact files. Snapshot effective destination/settings at acceptance; detach source bytes during acquisition and commit immutable artifacts before publication. Retain failures for explicit retry; reconcile interrupted operations after restart without silently restarting publication. Defaults: one active upload, eight pending operations, a 2 GiB pending-artifact budget, a 100-entry recent-activity view, and seven-day failed-artifact retention, configurable in settings. Successful upload history has no automatic record expiry by default; the activity-view limit and spool cleanup do not prune it. Local-only output files are user-owned and outside automatic cache cleanup.
- KTD4. Separate capture, global-shortcut, and clipboard capabilities behind desktop adapters. Capability discovery selects a validated adapter per operation, never an implicit substitute. Generic screenshot/clipboard portals are optional disclosed fallbacks, not the primary R14 path. Screenshot portal v3 targets and permission behavior must be checked per backend; the Clipboard portal requires RemoteDesktop/InputCapture sessions. [Screenshot portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Screenshot.html), [Clipboard portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Clipboard.html).
- KTD5. Cinnamon/X11 uses XCB passive key grabs, EWMH active-window identity/frame extents, a live transparent region overlay, and Qt clipboard ownership. Freeze target/layout before selection, then obtain region pixels after the selector is withdrawn. On Cinnamon, use the compositor screenshot service and wait for its private PNG write to close before worker decoding; X11 surface unmapping alone does not prove that Muffin has removed the outline from its rendered scene. Capture the visible framed rectangle for R2 rather than promising hidden-window reconstruction. Account for lock modifiers, keymap changes, and conflicting grabs. [EWMH](https://specifications.freedesktop.org/wm/latest-single/), [QScreen capture limits](https://doc.qt.io/qt-6/qscreen.html), [QClipboard](https://doc.qt.io/qt-6/qclipboard.html).
- KTD6. GNOME/Wayland uses a small GNOME 50 Shell extension for selection/capture, bindings, and background clipboard MIME transfer. The native app owns storage, encoding, secrets, and networking. Use documented Shell/St interfaces, narrow authenticated IPC, request IDs, payload limits, and complete disable/re-enable cleanup. Do not spoof allowed D-Bus callers or enable Shell unsafe mode. Leave the clipboard shortcut unset until the user assigns it during setup. [Shell screenshot service](https://raw.githubusercontent.com/GNOME/gnome-shell/main/js/ui/screenshot.js), [GNOME 50 clipboard implementation](https://raw.githubusercontent.com/GNOME/gnome-shell/gnome-50/src/st/st-clipboard.c), [extension review rules](https://gjs.guide/extensions/review-guidelines/review-guidelines.html).
- KTD7. Plasma 6.6/Wayland uses KWin ScreenShot2, current KGlobalAccel D-Bus shortcut APIs, and a native ext-data-control-v1 clipboard adapter. Install the ScreenShot2 restricted-interface desktop entry against the actual executable. Probe interface versions; drain image-transfer FDs asynchronously with validated dimensions, stride, format, and timeouts. Data-control handles unfocused arbitrary MIME import and CLIPBOARD-only writes without depending on Klipper text APIs or an unwanted PRIMARY write. [ScreenShot2 interface](https://raw.githubusercontent.com/KDE/kwin/master/src/plugins/screenshot/org.kde.KWin.ScreenShot2.xml), [KGlobalAccel interface](https://raw.githubusercontent.com/KDE/kglobalaccel/master/src/org.kde.KGlobalAccel.xml), [KWin 6.6 protocol support](https://raw.githubusercontent.com/KDE/kwin/Plasma/6.6/src/wayland_server.cpp), [data-control protocol](https://raw.githubusercontent.com/wayland-mirror/wayland-protocols/main/staging/ext-data-control/ext-data-control-v1.xml).
- KTD8. Carry backend pixel dimensions, native/logical monitor rectangles, scale, rotation, and identity alongside each capture frame. Crop per monitor, then compose a spanning image at the highest available participating frame density, with explicit rounding and transparent layout gaps. Preserve native pixels for single-frame images, exclude the cursor by default, and invalidate selections on display changes. Widgets must not assume one primary-screen DPR. [Qt high DPI](https://doc.qt.io/qt-6/highdpi.html).
- KTD9. Use the official AWS SDK C++ S3Client inside the S3 implementation of KTD18, building only S3/core from a pinned source release and transitive dependency manifest. Only the S3 adapter target imports AWS headers. Keep one long-lived client per effective profile and a bounded executor, stream private artifact files, and marshal immutable callback results to the GUI thread. Load the private S3 module on first provider request. A permanent bounded worker initializes the SDK, owns clients and requests, drains them, and shuts down the SDK on that same thread; the module remains pinned until process exit. Local-only startup maps no AWS dependency libraries. Configure explicit connect/whole-request/low-speed timeouts and three total transient upload attempts; deletion retry rules are governed by KTD20. [SDK build options](https://docs.aws.amazon.com/sdk-for-cpp/v1/developer-guide/cmake-params.html), [async lifetime](https://docs.aws.amazon.com/sdk-for-cpp/v1/developer-guide/async-methods.html), [Qt threading](https://doc.qt.io/qt-6/threads-qobject.html).
- KTD10. For the R16 development template use verified HTTPS path-style requests to its regional endpoint, since the dotted bucket does not match S3 virtual-host TLS wildcards. Other S3 profiles use their configured endpoint, region, bucket, prefix, and storage class, with a supported addressing mode appropriate to that endpoint/bucket. Persist the candidate key, content hash, unique operation/item marker, and write precondition before PUT. Use If-None-Match: * for new objects and If-Match with the checked ETag for configured overwrite. Keep upload endpoint and public URL base distinct, encode URL path segments once, and send the configured ACL header only when selected; aclMode=omit sends none. Without a custom public URL base, derive the direct object URL from the effective S3 addressing configuration. Never substitute development values for explicit profile settings. Preserve the key/body/precondition across transport retries and ambiguous publication; HEAD with the matching item marker, hash, and size can reconcile a lost reply as success. ETags are opaque preconditions, not content hashes. A 412 after a lost successful reply must not trigger renaming or skipping that item. When reconciliation is inconclusive, expose unknown status and retain the same key. A stable key does not guarantee exactly-once versions in a versioned bucket. [S3 addressing](https://docs.aws.amazon.com/AmazonS3/latest/userguide/VirtualHosting.html), [PutObject](https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html).
- KTD11. Use QtKeychain with insecure fallback disabled and an explicit credential provider. Resolve credentials during setup/session readiness; do not let a hotkey silently fall through to ambient AWS profiles or hang on secret-store unlock. A missing/locked store yields a setup action or explicit session-only credentials. The destination test uses a synthetic object and an unauthenticated GET of the configured public URL, independently of actual user captures. Never silently change the selected ACL mode after an error to manufacture success; an explicitly configured omit mode is supported and still requires the destination test to verify public reachability. [QtKeychain](https://github.com/frankosterfeld/qtkeychain/), [Object Ownership](https://docs.aws.amazon.com/AmazonS3/latest/userguide/about-object-ownership.html), [public access controls](https://docs.aws.amazon.com/AmazonS3/latest/userguide/access-control-block-public-access.html).
- KTD12. Clipboard revisions and invocation tickets govern R8. Observe ownership changes without continuously reading payloads; only the newest accepted share with an unchanged revision auto-copies. Older jobs still retain successful receipts. Serialize interactive source acquisition and return busy_selecting for another selection request. A copy failure or suppression is distinct from an upload failure.
- KTD13. Provide an optional Python 3.12+ stdio MCP helper using the official SDK and a lockfile. Tools are inspect, perform_action, get_operation, retry_operation, copy_result, cancel_operation, list_history, get_history_entry, copy_history_link, delete_history_uploads, reload_configuration, and patch_configuration; validate and forward CLI JSON through argument-vector process calls. Inspect reports capabilities, redacted diagnostics, and the non-secret configuration path/schema/revisions/validation status. Configuration reload and expected-revision patches use KTD21 services and report config_conflict without a separate settings pipeline. Retry-operation selects failed upload/local-save/delete stages and never repeats a completed upload; copy-result reuses an existing artifact or receipt. Delete requires explicit history IDs and an effect acknowledgement equivalent to the GUI confirmation, never arbitrary URLs, object keys, prefixes, or bucket-wide targets. Existing authorization for the concrete action supplies its scope without repeated app confirmation. No shell tool, remote listener, embedded model, or separate business logic. Inspection/history listing reads local metadata only; capture, publication, clipboard writes, and remote deletion occur only through their explicitly invoked actions. [Python MCP SDK](https://github.com/modelcontextprotocol/python-sdk).
- KTD14. Use QtTest for core/GUI contracts and a separate QEMU/QMP desktop lab for genuine compositor behavior. QMP injects keyboard/pointer input and captures witness screenshots outside the guest; guest SSH installs/runs the app and retrieves evidence. Use the production CLI for diagnostics and a deterministic guest fixture for content/paste assertions. Headless GUI tests prove widget behavior, not Wayland integration. [Qt Test](https://doc.qt.io/qt-6/qtest-overview.html), [QMP](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html).
- KTD15. Ship a Linux .deb and separate GNOME extension artifact first. Include the real executable desktop entry, autostart integration, SQLite/Wayland platform plugins, notices, and reproducible dependency metadata. Test installed packages on all R13 profiles. Compile shared core/UI on Windows/macOS with unsupported native adapters; do not advertise screenshot support there. Bundled dependencies need an update/rebuild path, while distro Qt stays dynamically replaceable. [Qt deployment](https://doc.qt.io/qt-6/deployment.html), [Qt distribution obligations](https://www.qt.io/development/open-source-lgpl-obligations).

- KTD16. Resolve archive folders by content category: screenshots/clipboard images and image files use images, video files use videos, plain text/text files use text, and remaining regular files use other files. An unset override falls back to the configured default root and category subfolder. Use MIME detection with content inspection for regular files, not extension alone. Snapshot routing settings with the operation. With R27 enabled, attempt atomic collision-safe archive creation before the batch PUT per R28, without overwriting existing files. Archive outputs are user-owned; keep retry spools immutable and independent of later archive edits. Share the folder resolver with local-only outputs under R6. Leave the opt-in setting disabled until configured.

- KTD17. Snapshot the S3 naming strategy and collision policy with each operation. Default fresh configurations to original filenames and timestamped captures with auto-rename; expose random and template naming under Advanced. Existing generated-mode configurations retain their choice. Original-name mode uses only a regular file’s basename, never its full source path. Renames append a random UUID suffix, preserve the extension, and obey S3 key byte limits; cap candidate searches at 32 per item and retain the source on exhaustion. Check signed HeadObject against S3, not a cached custom-domain URL: 200 means present, a definitive 404 means absent, and 403/timeout is inconclusive. Never treat denied access as a free name. Conditional PUT is the final protection against races; classify 409/412, reconcile possible own publication first, and advance a rename candidate only after a confirmed foreign collision. Skip is a terminal non-published item with no share receipt. Resolve same-batch names before the first PUT: rename allocates distinct keys, skip retains the first item, and overwrite marks every item sharing a target key as a non-published conflict while allowing distinct-key items to upload normally. This prevents two misleading receipts for one name without blocking unrelated files. Explicit overwrite retry may refresh a failed precondition after a fresh check, but may not resolve an unknown publication by overwriting blindly. [HeadObject permissions](https://docs.aws.amazon.com/AmazonS3/latest/API/API_HeadObject.html), [conditional writes](https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-writes.html).

- KTD18. Define a small asynchronous StorageProvider contract for capabilities, naming/existence checks, upload, stat, and delete. Receipts carry a provider ID, profile ID/revision, immutable non-secret destination snapshot, versioned opaque object reference, opaque concurrency token where supported, publication identity, and the direct URL as separate fields. Do not reconstruct deletion targets from URLs or the currently selected profile. Persist a credential-store reference, never credentials; missing credentials or a removed profile yields a restore-access action for the recorded destination. Providers describe conditional-write/delete support, version addressing, deletion effects, and URL availability; unsupported required behavior fails explicitly instead of silently dropping safeguards. Use a static provider factory and typed settings validation, with S3 and a test double first. No public dynamic provider-plugin ABI, generic AWS-shaped receipt, or speculative adapters are required. The S3 implementation may use an internal lazy-loaded module with an exact application/build/transport identity check and absolute owned build/install path; it is not an extension mechanism.

- KTD19. Create one upload-history entry per confirmed item receipt in the same transaction that commits upload success, including receipts recovered after an ambiguous reply. Store item/operation IDs, timestamp, display filename, MIME/category, byte size, original destination/receipt/direct URL, local-copy reference/status, and remote-management state. Unknown publication remains visible in activity/history as an unresolved item without an invented link or delete target; skipped duplicates have no upload receipt. Keep failed attempts inspectable under their retention policy. Use indexed, paginated SQLite queries and a lazy Qt item model; thumbnails are private, bounded, independently evictable cache entries, generated from acquired image artifacts without remote fetching. Missing thumbnails show a type icon and metadata; viewing the remote file uses the stored URL in the browser on an explicit action. Store no permanent source text/video/full-file payload solely for history. History deletion outcomes remain as records, and user-requested history clearing is a separate local action that never touches the cloud. Enforce transactional schema migration and private database/cache permissions. Upload history records, URLs, and filenames are excluded from default diagnostic exports.

- KTD20. Remote deletion is a persisted, separately identified operation over confirmed history receipts. Validate selected entries, show the original destination and provider's deletion effect, then acknowledge the concrete target set; CLI/MCP carry equivalent IDs/acknowledgement. Serialize this app's writes/deletes per destination/object and snapshot the deletion request before network activity. Recheck identity/version/marker and use the provider's conditional deletion where available; known replacements produce changed/superseded instead of being adopted as a new target. Never replace a failed deletion precondition with the latest object's token. A timeout or lost reply becomes delete_unknown; reconcile the recorded target before an explicit retry. Cancellation cannot promise rollback of a DELETE already sent.

  For S3, retain returned VersionId, ETag, and item marker in the receipt. For an unversioned or mutable null-version target, signed HEAD must match the recorded item identity and DELETE uses If-Match with its saved ETag. For an immutable non-null recorded version, delete only that version: if it is current, first conditionally create a delete marker so deleting its bytes cannot expose an older version; if already noncurrent, preserve the current replacement and remove only the recorded version. Before marker creation in a suspended bucket, explicitly check the null version: proceed only on confirmed absence, and return unsupported_safe_removal without DELETE when an unselected null version exists or the check is inconclusive. Creating that marker can erase the unrelated null version. Recheck the bucket state before marker creation and document that external versioning changes during deletion are outside S3's conditional-object guard. Persist each step separately and retain the protective marker. If version removal fails after hiding the link, report hidden_version_retained with retry of that exact version, not full success. Never enumerate or purge unrecorded versions. A missing receipt/version or inconclusive versioning/identity check blocks destructive guessing. S3's ETag guard compares the current content token, not a unique version identity; document the residual race with an external identical-content replacement rather than promise full version compare-and-swap. No automatic SDK retry of a marker-creating DELETE after an ambiguous reply. [Conditional deletes](https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-deletes.html), [version removal](https://docs.aws.amazon.com/AmazonS3/latest/userguide/DeletingObjectVersions.html), [suspended versioning](https://docs.aws.amazon.com/AmazonS3/latest/userguide/DeletingObjectsfromVersioningSuspendedBuckets.html).

  Classify permissions, Object Lock/MFA requirements, conflicts, missing objects, partial progress, and unknown results. Do not bypass retention or change bucket settings. A simple delete in a versioned/suspended bucket can retain stored versions; report that effect and do not claim all versions are erased. Explain that custom-domain/browser caches can continue serving prior bytes after origin deletion; this app does not invalidate a CDN. Local copies survive all remote outcomes. [DeleteObject API and permissions](https://docs.aws.amazon.com/AmazonS3/latest/API/API_DeleteObject.html).

- KTD21. Use JSON with Qt's existing parser, one typed SettingsService validator, and config.json in QStandardPaths::AppConfigLocation; the GUI and CLI report the exact active path, accepted content revision, and reload/validation status. Publish schemas/config.schema.json, config/development.example.json, and docs/configuration.md with every non-secret setting, defaults, constraints, migration rules, and credential-reference fields. There is no parallel YAML/preferences store. GUI/CLI writes validate the complete candidate and commit with QSaveFile, direct-write fallback disabled, using private permissions compatible with the Qt 6.4.2 floor. Watch both the file and parent directory with QFileSystemWatcher, re-arm after atomic replacement, debounce bounded reads, and deduplicate own-write notifications by content revision. If watching fails, report it and use a bounded periodic rescan; explicit reload is also available. Invalid JSON, unsupported schema versions/keys, wrong types, invalid profile references, excessive files, or transient removal preserve the last accepted in-memory settings and expose structured errors without rewriting the bad file. Invalid/missing configuration on first startup opens setup/recovery; it never silently activates the development destination. Validation/reload performs no capture, clipboard-content read, provider request, or secret-store unlock prompt. [JSON parsing](https://doc.qt.io/qt-6/qjsondocument.html), [configuration location](https://doc.qt.io/qt-6/qstandardpaths.html), [atomic writes](https://doc.qt.io/qt-6/qsavefile.html), [file replacement and watchers](https://doc.qt.io/qt-6/qfilesystemwatcher.html).
  - Clean GUI forms refresh after acceptance. Dirty forms retain their draft and show which accepted revision changed; reload/rebase is required before saving. App/CLI writers use an expected file-content revision and an advisory QLockFile, re-read under the lock, and return config_conflict instead of overwriting a detected change. Direct editors remain supported; document atomic-save/locking conventions and recommend CLI patching during concurrent GUI edits. An editor that ignores the lock can race a write, so this is cooperative conflict detection, not a claim of filesystem compare-and-swap. [Cooperative locking](https://doc.qt.io/qt-6/qlockfile.html).
  - Accepted changes update display preferences and request shortcut rebinding with actual status under R12. Operation-affecting settings apply to later invocations; pending operations, retries, and historical deletion continue using their immutable snapshots/receipts. Changed destination revisions receive distinct provider-client configurations while existing requests drain. Importing the development template is an explicit setup choice; it contains credential references and no access keys.

### High-Level Technical Design

These views expose boundaries and sequencing. Product behavior is owned by R entries and mechanisms by KTD entries; implementation names may follow the project’s eventual conventions.

Future screen recording will reuse the existing boundaries. Keep target selection and geometry metadata reusable independently of one-shot still-image acquisition. A later recording session can own stream capture, encoding, and start/stop/cancel behavior, then hand a finalized video file to the same immutable-artifact, local-copy, upload, and history services used for regular video files today. Those downstream contracts use file bytes and MIME/category metadata rather than requiring QImage or PNG. Discover recording capabilities separately from still capture; working screenshots do not imply a recording-capable backend. Select recording APIs, formats, audio support, and limits when that feature is planned. V1 introduces no recording controls, hotkeys, CLI/MCP actions, session implementation, or codec dependencies.

Component topology — KTD2, KTD4, KTD9, KTD13, KTD18/KTD19:

~~~mermaid
flowchart LR
    GUI[Qt settings, history and hotkeys] --> Ops[Resident operation services]
    MCP[MCP stdio helper] --> CLI[JSON CLI]
    CLI --> IPC[Session-local IPC]
    IPC --> Ops
    Ops --> Native[Desktop adapters]
    Ops --> Store[Artifact files and SQLite journal/history]
    Ops --> Providers[StorageProvider contract]
    Providers --> S3[S3 adapter / official SDK]
    Providers --> Fake[Test provider / future adapters]
~~~

Action protocol — F2/F3, KTD2:

~~~mermaid
sequenceDiagram
    participant Client as GUI / hotkey / CLI
    participant App as Resident app
    participant Desktop as Desktop adapter
    participant Storage as Artifact store
    participant Remote as Storage provider
    Client->>App: Invoke action
    App-->>Client: Accepted operation ID
    App->>Desktop: Freeze target and acquire source
    Desktop-->>App: Bytes, geometry, revision
    App->>Storage: Commit immutable artifacts and item identities
    App->>Remote: Upload retained artifact
    Remote-->>App: Receipt or classified failure
    App->>Storage: Commit outcome and history entry
    App->>Desktop: Copy result if eligible
    App-->>Client: Outcome and clipboard status
~~~

Operation lifecycle — R19, KTD3, KTD10, KTD12:

~~~mermaid
stateDiagram-v2
    [*] --> Acquiring
    Acquiring --> Stored
    Acquiring --> Cancelled
    Acquiring --> AcquisitionFailed
    Stored --> Uploading
    Stored --> Completed: local destination
    Uploading --> Uploaded
    Uploading --> Failed
    Uploading --> PublicationUnknown
    Uploading --> Cancelled
    Uploaded --> Completed: copy succeeds or is skipped
    Uploaded --> CopyFailed
    Failed --> Stored: explicit retry with complete retained artifacts
    PublicationUnknown --> Uploaded: reconciled successful receipt
    PublicationUnknown --> Stored: explicit same-key retry
    CopyFailed --> Completed: explicit copy retry
~~~

Source data flow — R4/R9, KTD3/KTD8/KTD9:

~~~mermaid
flowchart LR
    Source[Screen frames or clipboard MIME] --> Snapshot[Detached immutable snapshot]
    Snapshot --> Encode[PNG / UTF-8 text / regular-file bytes]
    Encode --> Spool[Private durable artifacts]
    Spool --> Archive[Optional user-owned local copy]
    Archive --> Upload[Stream through storage provider]
    Upload --> Receipt[Provider object reference and direct URL]
    Receipt --> History[Durable upload history]
~~~

Dispatch branches — R6/R7/R15/R18, KTD4/KTD11:

~~~mermaid
flowchart TD
    Start[Action invoked] --> Support{Required capabilities ready?}
    Support -- No --> Setup[Actionable setup failure]
    Support -- Yes --> Acquire[Acquire and store source]
    Acquire --> SourceOK{Acquisition complete?}
    SourceOK -- No --> Stop[Cancel or fail without publication]
    SourceOK -- Yes --> Destination{Local or remote?}
    Destination -- Local --> Local[Local result]
    Destination -- Remote --> Secrets{Provider and credentials ready?}
    Secrets -- No --> Retain[Retain artifact and show setup action]
    Secrets -- Yes --> Put[Upload]
~~~

Mode/result matrix — R5/R6/R9, KTD12:

| Source | Remote result (S3 first) | Local result |
|---|---|---|
| Screenshot/image | Public URL text | Saved PNG and image MIME |
| Plain text | URL to UTF-8 text object | Saved text and captured text |
| File list | Ordered successful URLs | Stored local copies and file URI list |

Automatic copy eligibility applies to both columns per R8; an explicit copy-result action remains available.

Versioned interface sketch — R21/R22/R34, KTD2/KTD13/KTD21:

| Contract | Information carried |
|---|---|
| Invocation | Contract version, request ID, action, explicit destination override |
| Accepted operation | Operation ID and resolved destination/backend; item identities populate after acquisition |
| Status/result | State, per-item receipts/errors, artifact references, requested/final upload names and collision outcomes, separate archive/upload/clipboard outcomes |
| History query/entry | Versioned cursor/filter, immutable entry ID, recorded destination and provider receipt, direct URL, preview availability, independent remote/local states |
| Remote deletion | Explicit entry IDs and effect acknowledgement; operation ID, per-item progress/outcomes, original object reference and immutable precondition |
| Configuration | Active file path, schema version, accepted/disk revisions, effective non-secret settings, validation errors; reload or patch with expected revision |
| Capabilities | Per-operation support, required interaction, reason, tested version |
| Error | Stable code, retryability, actionable next step; no secrets or source content |

Configuration profile sketch — R16/R18/R34, KTD21. This is the explicit development template's S3 profile, not a complete settings file or an automatically installed destination:

~~~json
{
  "id": "dev-s3",
  "provider": "s3",
  "credentialRef": "s3/dev-s3",
  "options": {
    "bucket": "put.gd",
    "region": "us-east-2",
    "endpoint": "https://s3.us-east-2.amazonaws.com",
    "objectPrefix": "",
    "storageClass": "STANDARD",
    "aclMode": "public-read",
    "publicUrlBase": "https://put.gd"
  }
}
~~~

The enclosing config.json has schemaVersion, stable profile IDs, action-to-profile mappings, and the remaining R11 settings. aclMode can be public-read or omit; publicUrlBase can be null for an endpoint-derived URL. Schema constraints and typed validation govern supported values. Renaming/removing a profile never remaps old receipts. Keep actual credential values in the R18 credential store or explicit session-only mode.

### System-Wide Impact

GUI, hotkeys, CLI, MCP, recovery, and tests share operation IDs, stored artifacts, and error semantics. GUI, CLI, and external JSON edits pass the same validator and expose the same accepted revision. Valid file changes refresh the GUI; validation errors and dirty-form conflicts remain visible. Operation-affecting edits affect subsequent operations; retries retain the original destination and preserve the key except for a confirmed name collision under the operation’s snapshotted policy. Changing destination requires a new explicit share.

Source files are copied into private storage before upload; originals are never removed. Retention cannot delete in-flight artifacts, and user-owned local outputs are not cache entries. Unknown publication and cancellation may leave remote objects, which must be reported rather than described as rollback.

R27's video category accepts existing video files in v1 and is ready to route finalized recordings when that deferred feature is added. Archive folders can sit on different volumes; their write attempts finish before the batch publication boundary, and their outcomes are independent of upload outcomes. A pending setting change does not reroute an existing operation. If an archive write fails, retain its retry source under the configured pending-artifact budget and offer Retry local save without reuploading a completed object.

R29/R30 check remote filename collisions, not identical content. Local archives retain their independent never-overwrite policy, even when S3 overwrite is selected. Confirmed rename candidates can change before publication; once a write may have succeeded, its key is fixed until reconciliation. Copy only URLs for this operation’s confirmed uploads, using final keys in source order; an all-skipped batch leaves the clipboard unchanged. The overwrite setting explains that existing share URLs can point to replacement content and that custom-domain caches may delay its visibility. Record required metadata-read permissions in destination setup; do not change bucket policy to make checks work.

R31–R33 make receipts durable management identities, independent of artifact retention and current settings. Changing a profile does not rewrite historical endpoints, namespaces, object references, or direct URLs. Removing a profile cannot repoint its history to another provider; credentials can be restored for the recorded destination. Mark known overwritten entries superseded; without a retained immutable version, their delete action cannot target replacement content. A copied old direct URL can now serve replacement content, so show that state alongside the link. Copying a history link is an explicit clipboard write and invalidates older automatic-copy eligibility under R8.

Remote deletion does not erase an operation's successful-upload fact, retry the upload, delete local copies, or remove its history identity. Missing/deleted entries retain outcome metadata; ordinary Copy link/Open actions are disabled for a known removed object, while details keep the historical URL. A receipt reports last-known remote state rather than implying continuous cloud synchronization. A fresh upload at a previously deleted key is a distinct item, not a retry of deletion. History/cache clearing cannot run remote deletion, and in-progress deletion records cannot be pruned.

Read-only diagnostics must not acquire pixels or clipboard content, prompt for permissions, or contact a storage provider. Exported bundles redact public URLs and filenames by default; explicit history details/actions can return the requested receipt and link.

### Risks and Execution-Time Evidence

| Risk / prerequisite | Decision and evidence required |
|---|---|
| Full Wayland parity | U7/U8 prove unfocused text/image/file acquisition and every result type alongside capture; missing support blocks that profile’s release, not the Qt bootstrap. |
| GNOME API changes | Package/test for GNOME 50 first; pin the extension compatibility range and test disable/re-enable. Store approval is optional and not a prerequisite to direct installation. |
| Portal version mismatch | Diagnose runtime interface/backend versions, never infer them from package names or latest documentation. |
| S3 public-read incompatibility | U3 classifies existing ownership/access errors and tests public reachability; infrastructure changes require a separate instruction. |
| Duplicate checks and concurrent writes | U3 proves signed HEAD classification and conditional PUT, including 403 ambiguity, a competing writer, and a lost successful reply. An unsupported SDK/endpoint must fail clearly rather than send an unprotected write. |
| History targets and remote deletion | U2/U3 prove original-destination routing, changed-object rejection, exact-version handling, partial deletion, and lost replies; deny/inconclusive results never become successful deletion. Versioned tests do not change the user's bucket configuration. |
| History growth and preview cache | U2/U5 prove indexed pagination and bounded preview loading with a synthetic 10,000-entry ledger; U10 checks memory/responsiveness with that ledger on the physical machine. |
| External configuration edits | U2/U5 prove atomic replacement/re-arming, invalid-edit recovery, dirty-form conflicts, cooperative revision checks, and unchanged accepted operation/history targets; U10 verifies live edits in every installed desktop profile. |
| Future storage providers | U2's second test provider exercises receipt/history/delete contracts without AWS-shaped assumptions. Missing provider capabilities are actionable setup errors. |
| GNOME clipboard allocation | Prefer bounded MetaSelection transfer where St.Clipboard would buffer an unlimited offer; native IPC validates bytes before decoding. |
| Malformed image/FD transfer | Native adapters check dimensions, stride, format, byte budgets, EOF, and deadlines before constructing images. |
| SDK lifetime / shutdown | U3 stress tests in-flight cancel/restart/shutdown and resource draining, including late callbacks. |
| VM acceleration unavailable | U6 performs a lab readiness check; use a host that exposes KVM. TCG may provide slow functional evidence, never performance evidence. |
| GPU/compositor differences | U10 repeats critical checks on the physical user machine; VMs do not prove hardware-specific HDR, driver, or multi-GPU behavior. |
| Dependency/build feasibility | U1 verifies the exact Mint toolchain and pinned S3/core build before feature construction. |
| Performance budgets | U4 establishes the real hardware baseline; U10 measures release builds and fixes demonstrated bottlenecks. |

Runtime backend behavior, actual display topology, dependency patch versions, and VM acceleration are deferred to the named executable gates. These do not block beginning U1. They must not be represented as already verified.

### Delivery Sequence and Proposed Layout

First prove a Cinnamon region capture → synthetic S3 upload → pasteable URL using U1–U4, with minimal settings from U5. Build the VM harness in U6 before completing GNOME/KDE work in U7/U8. MCP and full settings can develop after their shared contracts exist; packaging and release evidence finish in U10.

| Area | Proposed location |
|---|---|
| Domain contracts, jobs, storage, settings | src/core/ |
| Configuration schema, development template, guide | schemas/config.schema.json, config/development.example.json, docs/configuration.md |
| Resident orchestration, IPC, CLI | src/app/, src/cli/ |
| Capture, shortcut, clipboard adapters | src/platform/ |
| Qt settings, history, tray, status | src/ui/ |
| Storage contract and providers; credentials | src/core/StorageProvider.*, src/storage/s3/, src/secrets/ |
| GNOME helper | integrations/gnome/ |
| Optional MCP helper | tools/mcp/ |
| Desktop VM lab and fixtures | tools/lab/, tests/fixtures/, tests/desktop/ |
| Headless contracts and GUI checks | tests/unit/, tests/gui/, tests/integration/ |
| Build, packaging, docs | cmake/, packaging/, docs/, AGENTS.md |

---

## Implementation Units

| Unit | Work packet | Primary files | Depends on |
|---|---|---|---|
| U1 | Bootstrap reproducible builds | CMakeLists.txt, CMakePresets.json | — |
| U2 | Shared operations, storage, and CLI | src/core/, src/app/, src/cli/ | U1 |
| U3 | S3 upload/deletion and secret storage | src/storage/s3/, src/secrets/ | U2 |
| U4 | Cinnamon/X11 workflow | src/platform/x11/ | U2, U3 |
| U5 | Settings and resident UX | src/ui/ | U2, U3, U4 |
| U6 | Graphical VM lab and development package | tools/lab/, tests/fixtures/, packaging/linux/ | U1, U2, U4 |
| U7 | GNOME Wayland adapter | integrations/gnome/, src/platform/gnome/ | U3, U5, U6 |
| U8 | KDE Wayland adapter | src/platform/kde/ | U3, U5, U6 |
| U9 | Agent interface and documentation | tools/mcp/, docs/agents.md | U2, U3, U4, U6 |
| U11 | On-demand UI with a persistent tray | src/app/, src/ui/, src/main.cpp, CMakeLists.txt | U2–U5, U7–U9 interfaces |
| U10 | Packages, performance, release gates | packaging/, docs/release.md | U4–U9, U11 |

### U1. Bootstrap reproducible Qt/C++ builds

Goal: A clean checkout builds the app skeleton and test targets on the minimum Linux baseline.

Requirements: R24–R26. Dependencies: none.

Files: CMakeLists.txt, CMakePresets.json, cmake/Dependencies.cmake, cmake/dependency-lock.json, src/main.cpp, tests/unit/test_build_contract.cpp, .github/workflows/build.yml, README.md, AGENTS.md.

Approach: Implement KTD1/KTD9 build boundaries and dev, release, ASan/UBSan, and TSan presets. Keep AWS SDK build options separate from app C++20 settings; import SDK targets and pin source/transitive hashes. Add Linux headless tests and shared core/UI compile jobs for Windows/macOS with native stubs. Record dependency provisioning, supported compilers, and the standing Anthropic review authorization in AGENTS.md.

Execution note: Seek a clean dependency compile/link proof on Mint before building features.

Patterns to follow: Official Qt CMake and AWS CMake targets; no existing repository pattern.

Test scenarios:
1. A clean baseline build links Qt, QtKeychain, SQLite support, and the selected SDK without downloading floating dependencies.
2. A build with tests disabled excludes test-only dependencies.
3. Windows/macOS jobs compile shared code without importing Linux-only D-Bus/XCB headers.

Verification: Repeatable build presets and platform compile results exist, and AGENTS.md accurately describes the bootstrap.

### U2. Shared operations, artifacts, history, settings, and CLI

Goal: Every entry point can invoke and inspect one recoverable operation pipeline.

Requirements: R4, R6–R11, R15, R16, R19, R21, R23, R27–R34. Dependencies: U1.

Files: src/core/Operation.*, src/core/ArtifactStore.*, src/core/HistoryStore.*, src/core/StorageProvider.*, src/core/StorageReceipt.*, src/core/Settings.*, src/core/SettingsService.*, schemas/config.schema.json, config/development.example.json, tests/unit/test_settings.cpp, tests/integration/test_config_reload.cpp, docs/configuration.md, src/core/ClipboardPayload.*, src/core/LocalArchive.*, src/core/UploadNaming.*, src/app/OperationService.*, src/app/HistoryService.*, src/app/LocalServer.*, src/cli/main.cpp, tests/unit/test_payload.cpp, tests/unit/test_operations.cpp, tests/unit/test_archive.cpp, tests/unit/test_upload_naming.cpp, tests/unit/test_history.cpp, tests/integration/test_storage_contract.cpp, tests/integration/test_cli.cpp, docs/architecture.md, docs/storage-providers.md, docs/cli.md, docs/errors.md.

Approach: Establish KTD2/KTD3/KTD12 contracts with fake native/provider seams. Implement versioned schemas, transactional journal recovery, file acquisition, MIME precedence, configuration snapshots, capability reporting, and copy-result retry. Implement KTD21 canonical file loading, shared validation, migrations, atomic writes, replacement watching/fallback, accepted revisions, and conflict reporting. Settings inspection is secret-free and read-only. Configuration acceptance may refresh preferences and request shortcut rebinding under KTD21/R12; it performs no capture, clipboard-content read, provider request, or secret-unlock prompt. CLI patching uses expected revisions. Provide the explicit development-template setup path without implicitly activating it. Implement KTD16 routing and the R28 archive boundary in the shared pipeline. Add KTD17 naming, same-batch collision resolution, persisted candidate/precondition identities, and terminal duplicate-skip outcomes. Implement KTD18/KTD19 provider receipts and paginated history, plus KTD20 deletion orchestration against a test provider; U3 supplies S3-specific behavior. Expose history and deliberate deletion through the same typed CLI/GUI services. Preserve the generic finalized-file boundary for future recording; the existing video-file cases prove downstream compatibility without a recorder implementation.

Test scenarios:
1. Files plus text on the clipboard select files; image plus text selects the image; empty/unsupported MIME produces no upload.
2. One unavailable file in a two-file acquisition prevents all publication and reports the failing item.
3. Reverse-order job completion or an intervening clipboard ownership change follows R8 without changing upload receipts.
4. Restart after stored/unknown/uploaded states preserves artifacts and keys and does not silently publish again.
5. Local destination returns the R6 result and issues zero uploader calls.
6. Malformed, oversized, unknown-version, and wrong-session CLI requests fail with structured errors; an accepted operation ID is returned before interactive acquisition so a client can inspect or cancel it.
7. Covers AE7/AE8. Image/video/text/other-file fixtures use their overrides or default subfolders; an unwritable/full destination reports archive failure while the immutable spool still uploads and supports local-save retry.
8. Same-named files never overwrite archives; cache cleanup and a later archive edit do not remove user-owned copies or change retry bytes.
9. Naming preserves extensions and handles Unicode, long keys, case-sensitive keys, prefixes, and two same-named batch files. Skip emits no receipt; an all-skipped batch performs no clipboard write. With two report.pdf files plus notes.txt under overwrite, both report.pdf items become non-published conflicts before any PUT, while notes.txt uploads and contributes its URL normally.
10. Changing collision settings affects only later operations; restart preserves in-flight candidates, write conditions, and item markers without automatic renaming or publication.
11. Covers AE11. Success/receipt reconciliation commits exactly one history entry per item; cache cleanup and restart preserve link, destination, local-copy status, and remote status. Paginated filtering/search over 10,000 entries does not load the entire ledger or thumbnail set.
12. Versioned history migrations preserve populated records and fail transactionally. Explicit history clearing makes zero provider calls and preserves local files; in-progress deletion identities are retained.
13. Covers AE13. A non-S3 test provider uses its own opaque file/version identifiers and URL format for upload/stat/delete. Shared history and CLI require no bucket/key/ETag fields or AWS headers; unsupported capabilities produce structured errors.
14. Delete acknowledgement binds selected history IDs/effects; invalid, skipped, local-only, changed, or ambiguous-publication entries issue no destructive provider call. Partial deletion has per-item states, survives restart, and never retries an upload.
15. Covers AE14. Repeated atomic file replacements refresh the accepted settings once per revision, including watcher re-arming/fallback; own-write events do not loop. GUI/CLI/file round trips preserve all non-secret controls.
16. Invalid syntax/types/keys/schema/profile references, excessive input, and a temporarily removed file retain accepted settings and the original bad file. Fresh invalid/missing configuration enters setup without activating put.gd. Read/validate/reload makes zero capture, clipboard-content read, provider, or secret-unlock calls; accepted changes may refresh preferences and request shortcut rebinding with actual R12 status.
17. Cooperative writers with stale expected revisions return config_conflict and preserve the file; failed atomic writes leave prior bytes intact. Mid-upload config changes affect only later actions, not accepted retries or original-receipt deletion.

Verification: GUI/hotkey-ready typed operations and the real JSON CLI agree on IDs, state, and errors; file/schema/CLI validation agree and no source payload appears in diagnostics.

### U3. S3 upload/deletion, public URLs, and secret storage

Goal: Retained artifacts become correctly addressed S3 objects and usable provider receipts; recorded uploads can be deleted with accurate outcomes.

Requirements: R5, R9, R16–R19, R20, R28–R33. Dependencies: U2.

Files: src/storage/s3/S3Provider.*, src/storage/s3/CollisionChecker.*, src/storage/s3/PublicUrl.*, src/secrets/CredentialStore.*, tests/unit/test_public_url.cpp, tests/integration/test_s3.cpp, tests/integration/test_s3_delete.cpp, tests/integration/test_collision_policy.cpp, tests/integration/test_keychain.cpp, tests/fixtures/s3_server.py, docs/s3.md, docs/credentials.md.

Approach: Implement KTD9–KTD11/KTD17/KTD20 inside KTD18's S3 adapter against a controllable offline HTTP/S3 fixture. Verify the pinned SDK supports signed conditional PUT and DELETE headers. Make cancellation stop queued work and suppress late result copying while request resources drain. Provide an explicit synthetic destination test, metadata-check/deletion capability results, and per-item retries. Record effective versioning/deletion effects without changing them; document upload/read/delete and version-delete permissions separately. Preserve credential references and original endpoint/bucket/version details in provider receipts, without leaking secrets into history.

Test scenarios:
1. A synthetic PNG uses the configured region/endpoint/bucket/prefix, ACL mode, storage class, content type, object key, and public URL base. Exercise both the development template and a different explicit profile; omit sends no ACL header, and a null public URL base derives the effective endpoint URL. No R16 value is hardcoded into routing.
2. Prefixes/spaces/Unicode encode once, and the custom share domain is never the signing endpoint.
3. Ownership-enforced/public-access-blocked/auth failures are terminal and do not change policy or silently switch the configured ACL mode.
4. A server that stores a PUT but drops its reply produces unknown/reconciled status and retains the same key and body on retry.
5. A two-item partial batch retains successful receipts and retries only failed items.
6. Locked/unavailable/denied keychain access never writes credentials to fallback storage or diagnostics.
7. Slow responses, cancellation, and shutdown retain valid SDK lifetimes and leave the GUI event loop responsive.
8. Covers AE8. An archive failure leaves uploading enabled, with distinct archive status; a later upload failure or cancel retains any committed local copy.
9. Covers AE9/AE10. Existing keys exercise rename, skip, and configured overwrite; final URLs match stored bytes. HEAD 403, timeout, and exhausted rename candidates produce explicit outcomes without an unchecked PUT.
10. A competitor creates or changes a key between HEAD and PUT: conditional create never overwrites it, and conditional overwrite fails on changed/deleted content rather than refreshing the precondition automatically.
11. A conditional PUT succeeds but loses its reply, then a retry receives 412: matching operation/item/hash metadata recovers the existing receipt without renaming, skipping, or uploading another object. Without conclusive metadata, publication stays unknown at the same key.
12. Covers AE12. Signed deletion addresses the recorded endpoint/bucket/key rather than an edited profile or public URL. Identity/precondition mismatch and HEAD 403 issue no unguarded DELETE; access/retention/MFA errors preserve history and local copies.
13. Unversioned, enabled-versioning, suspended/null-version, and already-missing fixtures report actual removal effects. Deleting a current immutable version in an enabled bucket hides it before removing its bytes; deleting a noncurrent recorded version leaves the replacement intact. A suspended bucket with an unrecorded null version beneath the selected current non-null version returns unsupported_safe_removal without DELETE and preserves both; a conclusive absence allows guarded marker creation, while denied/inconclusive null-version reads block it. Partial version removal retains its marker and reports hidden_version_retained; unrecorded versions are never purged.
14. A DELETE or marker creation succeeds but loses its reply: reconciliation/restart retains the exact target and step, does not refresh to a replacement object, and does not blindly create another marker. Concurrent overwrite/delete, 409/412, unresolved absence, cancellation, and shutdown remain correctly classified.

Verification: Offline integration tests exercise the real SDK adapter and versioning failure cases. A live synthetic fixture later proves upload, unauthenticated custom-domain GET, and guarded removal of that fixture only, without using a user screenshot or changing bucket controls. Versioned live evidence uses a preconfigured disposable test profile when available; it never enables versioning on put.gd as a test side effect.

### U4. Cinnamon/X11 capture and global actions

Goal: The four hotkeys work on the user’s Cinnamon session through the complete operation pipeline.

Requirements: R1–R8, R12–R15, R20. Dependencies: U2, U3.

Files: src/platform/x11/X11Capture.*, src/platform/x11/X11Shortcuts.*, src/platform/x11/X11Clipboard.*, src/platform/Geometry.*, src/ui/RegionOverlay.*, tests/unit/test_geometry.cpp, tests/desktop/cinnamon.yaml, docs/desktops/cinnamon.md.

Approach: Implement KTD5/KTD8/KTD12, freezing desktop/window targets, display geometry and settings before overlay focus changes. Keep desktop content live during region selection and acquire its latest pixels after the overlay is withdrawn. Native session-thread requests handle target/selection/clipboard ownership; PNG decoding, encoding and storage run on workers. Keep target/geometry handling separate from still-frame acquisition as described in the future-recording boundary. Establish the physical performance baseline.

Test scenarios:
1. Covers AE1/AE2. A region drag captures exact fixture content; Escape/right-click cancels without publication.
2. Two external windows with an occluding window produce the documented R2 target, never the app’s own overlay.
3. All-monitor capture includes panels, negative origins, rotated layouts, and defined gaps.
4. Caps/Num Lock and keymap changes preserve bindings; a conflicting shortcut reports failure.
5. Text/image/two-file clipboard fixtures upload matching bytes and paste the intended result into a separate app.
6. A layout change during selection aborts stale acquisition; no active/disappearing window returns a target error.

Verification: Real Cinnamon hotkeys, artifacts, public-link/paste behavior, and release-build timing are recorded. QtTest geometry checks alone do not satisfy this unit.

### U5. Settings, upload history, tray, status, and local workflow

Goal: Users can configure every supported action and understand setup and failures from the GUI.

Requirements: R6, R11, R12, R15, R16, R18–R20, R27–R35. Dependencies: U2, U3, U4.

Files: src/ui/SettingsWindow.*, src/ui/ShortcutEditor.*, src/ui/RecentOperations.*, src/ui/HistoryWindow.*, src/ui/HistoryModel.*, src/ui/TrayController.*, tests/gui/test_settings.cpp, tests/gui/test_operations_view.cpp, tests/gui/test_history_view.cpp, docs/user-guide.md.

Approach: Establish and apply a custom Qt visual system for all user-facing windows: typography, spacing, palette, icons, navigation, and reusable controls. Treat the existing basic forms as functional scaffolding; redesign their composition and visual hierarchy before release. Bind widgets to KTD21 shared settings/services, with stable accessible object names for tests. Expose the configuration path/revision and validation status; refresh clean forms on external acceptance and preserve dirty drafts with conflict/reload handling. All R16 fields are editable, including explicit ACL omission and an optional public URL base. Add destination testing, secret-store/session-only selection, conflict/capability status, copy/retry actions, optional login startup, and notifications. Keep settings/history reachable through the launcher/CLI when a tray icon is unavailable. Add the opt-in upload-copy setting, default-folder chooser, and per-category folder overrides through KTD16, plus local-save status and retry without reupload. Add generated/original upload-name selection and rename/skip/overwrite policy controls, with duplicate-check permission errors and per-item renamed/skipped/conflict outcomes. Implement R31 history search/filters/details, lazy image thumbnails, open/copy-link actions, selected-item remote-delete confirmation/progress, and distinct clear-history/cache controls. Present original destination, last-known state, superseded links, version-retained results, and local-copy status plainly. Provider settings/capabilities drive available controls without S3 assumptions in the history view.

Test scenarios:
1. Saving invalid endpoint/path/shortcut settings shows actionable validation and does not partially replace valid settings.
2. Switching one action to local mode follows AE6 while other action destinations remain unchanged.
3. Changing destination during an upload affects only later operations.
4. Upload success plus copy failure shows the URL and copy-only retry; partial batches show successful and failed items.
5. Missing tray integration and missing/disabled Wayland helpers still leave settings and setup instructions reachable.
6. Changing only the video folder leaves other mappings intact; disabling upload copies leaves local-only saving active and removes no existing files.
7. Rename is the initial duplicate policy; overwrite requires explicit selection and explains its effect on existing links. Changing that setting leaves pending operations unchanged, and upload overwrite never changes the local archive policy.
8. Covers AE11. Older and partial-batch items remain searchable after restart/cache eviction; a missing preview uses a type icon. Copying a selected old link is deliberate and respects the newer clipboard state when another job later completes.
9. Covers AE12. Deletion confirmation identifies original targets/effects, cancellation sends no DELETE, and successful/denied/uncertain/partially removed items have distinct visible states. Local copies survive; clearing history/cache never removes remote objects.
10. A populated 10,000-entry ledger stays responsive during paging/filtering and bounded thumbnail decoding; history browsing works offline and makes zero provider calls. Known removed links are disabled for ordinary open/copy actions; details preserve the historic receipt.
11. Covers AE14. External file edits update clean forms, preserve dirty drafts with a conflict notice, and cannot silently overwrite a detected newer revision. Invalid edits show field/parse errors with the accepted settings still active; a later valid replacement recovers without restart.

12. Covers AE15. Inspect rendered settings, setup, activity, populated history, and confirmation/error states for the custom visual design, including keyboard focus and supported display scales. Retain screenshots of the finished design and correct clipping, inconsistent styling, and unclear hierarchy.
13. Complete initial setup using only the simple section, then expand Advanced settings without losing the draft. Both views and an external JSON edit agree. Save/reload size limits in declared MiB/GiB or MB/GB units, including fractional values, and preserve exact unchanged byte counts. Validate overflow or invalid size input through the existing settings contract.

Verification: Headless/in-process Qt GUI tests prove form/state behavior; real sessions prove tray, launch-at-login, notifications, and clipboard paste. Actual rendered screenshots and visual review prove R35; default-looking functional forms do not satisfy this gate.

### U6. Reproducible graphical VM lab

Goal: Humans and agents can run and inspect genuine desktop workflow tests repeatedly.

Requirements: R13–R15, R23–R25. Dependencies: U1, U2, U4.

Files: tools/lab/lab.py, tools/lab/profiles/*.yaml, tools/lab/image-lock.json, tests/fixtures/desktop_fixture.cpp, tests/desktop/common.yaml, tests/desktop/test_lab.py, packaging/linux/CMakeLists.txt, packaging/linux/app.desktop.in, docs/testing.md, docs/vm-lab.md.

Approach: Implement KTD14 with QEMU/QMP, disposable qcow2 overlays, an ephemeral guest SSH channel, and localhost-only interactive display access. Add a minimal CPack .deb target that installs the built app, required runtime components, and launcher entry into each guest. U7/U8 extend this target with their integration assets; U10 completes release packaging and lifecycle validation. Profile readiness checks confirm acceleration, native session type, portal/helper versions, display layout, and input routing. Pin base images/checksums and retain sanitized evidence. Use the user-provided Ubuntu26.04.1 and Fedora44 ISOs to provision reusable base disks once; each test boots a separate disposable qcow2 overlay. Fedora is an additional smoke-test environment, while the R13 release profiles remain unchanged. Never commit local installer media or credentials.txt. Provide bounded lab operations for launch/reset/input/screenshot/collect; these are development tools, separate from production MCP.

Execution note: Prove one complete Cinnamon fixture run before expanding the lab to GNOME and KDE.

Test scenarios:
1. Two runs from a golden image install the development .deb, start with identical app/settings/clipboard fixtures, and use independent writable disks without source-tree dependencies.
2. QMP drives a real shortcut, region drag, and paste; stored artifact pixels and pasted content match the guest fixture.
3. An actual Wayland session is required for Wayland profiles; Xwayland fallback fails a Wayland profile, while missing KVM is reported and any TCG result is labelled functional-only.
4. A deliberately wrong target/crop/clipboard result fails the scenario and preserves witness screenshots and logs.
5. Timeout/crash cleanup terminates the VM and preserves diagnostics without leaking credentials or exposing control ports.

Verification: One command interface documented in the repository runs a named desktop profile and exports machine-readable results plus inspectable evidence.

### U7. GNOME 50 Wayland integration

Goal: GNOME delivers the R14 workflow, including background clipboard file/image/text sharing.

Requirements: R1–R15, R18, R23–R25. Dependencies: U3, U5, U6.

Files: integrations/gnome/extension.js, integrations/gnome/metadata.json, integrations/gnome/schemas/*.xml, src/platform/gnome/GnomeAdapter.*, tests/desktop/gnome.yaml, tests/integration/test_gnome_bridge.cpp, docs/desktops/gnome.md.

Approach: Implement KTD6/KTD8/KTD12 with narrow helper requests and bounded content transfer. Extend setup and packaging with install/enable/version status, clipboard shortcut assignment, and clean removal.

Execution note: First prove region/window/full capture, all three background clipboard import types, and local image/file/text paste in the VM before polishing extension UI.

Test scenarios:
1. After one-time setup, all actions execute from another app without a per-capture consent picker.
2. Image/file/text offers import correct bytes, and URL/local-result paste works without focusing the Qt app.
3. Missing/disabled/incompatible extension reports capability failure and does not silently use a different capture target.
4. Disable/re-enable and Shell/session restart remove/recreate bindings, signals, and IPC endpoints without duplicates.
5. Oversized MIME offers, forged/stale request IDs, transfer timeout, and selection cancellation produce bounded failures.
6. Fractional mixed-DPI layout changes and a clipboard change during upload follow R3/R8.

Verification: Installed app plus extension passes the GNOME native Wayland profile, with captured pixels, uploaded bytes, and independent paste evidence.

### U8. Plasma 6.6 Wayland integration

Goal: KDE delivers the four actions without a per-capture picker or focus workaround.

Requirements: R1–R15, R23–R25. Dependencies: U3, U5, U6.

Files: src/platform/kde/KWinCapture.*, src/platform/kde/KdeShortcuts.*, src/platform/kde/DataControlClipboard.*, src/platform/kde/protocols/*, packaging/linux/app.desktop.in, tests/integration/test_kwin_transfer.cpp, tests/desktop/kde.yaml, docs/desktops/kde.md.

Approach: Implement KTD7/KTD8/KTD12 and install the restricted-interface entry against the real executable. Use generated Wayland protocol bindings rather than an X11 clipboard tool. Make current shortcut APIs and returned bindings explicit.

Execution note: Prove the installed desktop entry and all clipboard MIME/result types before treating capture API discovery as parity.

Test scenarios:
1. Actual global shortcuts capture focused-window, selected-region, and all-monitor fixture content.
2. An absent/mismatched desktop permission entry fails clearly; the installed real executable succeeds.
3. Delayed/truncated/oversized FD image transfers fail without blocking the UI or reading beyond bounds.
4. Data-control imports file/image/text while unfocused and writes CLIPBOARD without directly changing PRIMARY.
5. Missing protocol/shortcut service, conflicting binding, and compositor restart update capabilities and recover safely.
6. Fractional scale, rotation, gaps, and monitor removal preserve the KTD8 mapping or invalidate acquisition.

Verification: The installed .deb passes the native Plasma Wayland profile, including clipboard interop with a separate application.

### U9. MCP parity and agent documentation

Goal: An agent can act and debug through stable tools, and work from committed instructions.

Requirements: R21–R24, R27, R31–R34. Dependencies: U2, U3, U4, U6.

Files: tools/mcp/pyproject.toml, tools/mcp/uv.lock, tools/mcp/share_mcp/server.py, tests/integration/test_mcp.py, docs/mcp.md, docs/agents.md, AGENTS.md.

Approach: Implement KTD13 as a strict adapter to the production CLI. Document tool effects, operation/deletion lifecycles, history pagination/filters, original-destination receipts, explicit deletion acknowledgement, redacted inspection, VM evidence collection, and source/build/test/package maps. Document the provider contract and how to add an adapter and its capability tests. Document active configuration discovery, the JSON schema/development example, external-edit reload behavior, expected-revision CLI patches, atomic-save/locking conventions, and invalid-edit/conflict recovery. Add examples using synthetic data.

Execution note: MCP implementation and headless contract tests can begin after U2/U3; completion waits for the working Cinnamon scenario and documented U6 lab runner.

Test scenarios:
1. An official SDK client discovers schemas and performs an action with matching CLI operation IDs and results.
2. Inspection/history queries generate zero capture, clipboard-content reads/writes, permissions prompts, or provider network activity.
3. Invalid parameters, unavailable session, paths containing spaces, malformed CLI output, and startup timeout return useful structured errors.
4. Capture/upload tools state the configured publication effect; helper invocation never uses a shell or receives raw credentials.
5. Retry/cancel/copy-result behavior matches the GUI/CLI, including completed uploads and unknown publication.
6. History listing/details/copy-link/delete match GUI/CLI entry IDs and per-item outcomes. Missing target IDs/effect acknowledgement issue no DELETE, and deleting never changes the current upload destination or retries a successful upload.
7. Configuration inspection/reload/patch agrees with GUI/file validation, discovers the active path/revision, returns structured conflict/errors, and performs no cloud or capture activity. A synthetic agent edit updates the live settings view.

Verification: CLI/MCP contract tests pass and a fresh agent follows the docs to build, run a desktop scenario, collect diagnostics, and identify a simulated failure.

### Required UI iteration after the first custom visual pass

The user accepts the modern direction but finds the current interface sloppy and its copy too much like AI-generated marketing. Iterate before AE15 acceptance: remove slogans and inflated prose, use short task labels and concrete help, tighten spacing and alignment, improve visual hierarchy and populated/empty/error states, and inspect installed windows at the required sizes/scales. Preserve functional controls, stable object names, keyboard access, settings conflicts, per-item outcomes, and exact deletion confirmations. Screenshots of the first pass are a baseline, not final visual acceptance. The subsequent user request adds a simple getting-started section with standard defaults, progressive disclosure into Advanced settings, and human-readable size controls/summaries. This next pass remains required before visual acceptance. Run this independent presentation work alongside native acceptance and memory measurement where file ownership, build directories, and desktop sessions do not collide.

Retain structured test parameters and logs for repeatability: scenario/profile/fixture, exact hotkeys and timing, pre-action geometry/layout/scales, package and runner hashes, expected/actual hashes, operation IDs, outcomes, and JUnit. Keep per-attempt records rather than overwriting earlier results. CI retains headless CTest reports; graphical acceptance requires a native desktop lab/self-hosted job and cannot be claimed from hosted headless CI. Never include credentials, VM authentication material, or real user payloads in these records.

### U11. On-demand UI with a persistent tray

Goal: Closing Settings, History, Activity and About releases their process while
the resident tray, shortcuts, clipboard and operations keep working.

Requirements: R4–R15, R19–R22, R24, R26, R31–R37. Dependencies: existing U2–U5,
U7–U9 service and desktop interfaces. This user-approved follow-up runs before
final U10 release acceptance; it does not renumber the earlier units.

Approach: Keep one resident operation/history/configuration owner. Add an
asynchronous UI client over the existing private command transport; retain CLI
and MCP compatibility. Move view construction and native settings dialogs into
a separately packaged UI executable. Keep the tray/menu and capture adapters
resident initially; further toolkit removal requires measured evidence and must
preserve the tray. Add bounded service methods for currently direct-only setup
actions, preserving credential isolation, destination-test journaling and exact
deletion acknowledgement. Preserve safe draft/view state without retaining
widget trees or serializing credential entry fields. Supervise helper activation,
duplicate requests, exit, failure and version agreement.

Test scenarios:
1. AE17 across all three desktops, including an ongoing operation when the last
   UI window closes and each tray action while the UI is absent.
2. Settings save/reload/Cancel, external-file edits, conflicts, session credentials
   and native folder dialogs behave as before. UI exit clears typed secrets.
3. History pagination, preview disposal, copying links and confirmed deletion use
   the resident's exact IDs and outcomes. No second database/operation owner.
4. Repeated open/close, simultaneous activation, helper crash, unavailable resident
   and mismatched helper versions give bounded outcomes; Quit shuts down cleanly.
5. Compare total RSS/PSS/private memory with UI absent and present, cold startup,
   post-capture/upload/history and reopen latency using the frozen protocols.
   Save source/package/process identities, raw results and visible native evidence.

Verification: Normal dev/release builds, meaningful headless client/lifecycle
contracts, sanitizer checks for asynchronous ownership changes, and current-source
Cinnamon/GNOME/KDE graphical acceptance. The 40 MiB target remains a measurement
goal, not an assumed outcome of the split. See [the analysis](../resident-ui-process.md).

Implementation checkpoint (2026-10-08): the resident tray and on-demand helper
are implemented. Dev 41/41 and the 16 affected ASan/UBSan and instrumented-Qt TSan
contracts pass, as do the Release build and separate MCP suite. The completed
review's four confirmed defects have caller fixes and regression evidence.
See [the implementation receipt](../evidence/2026-10-08-ui-process.json).
GNOME ownership and setup-result recovery now have reproduced regressions and
passing bounded fixes (see the linked follow-up receipts in verification.md).
The frozen source-41 build now passes the three desktops' native capture/clipboard
matrix, actual tray actions, normal helper close/reopen, draft/conflict/Cancel,
native-dialog and local-work continuity subsets. All three post-use VM memory
observations are guarded and exported; retain RSS/PSS/private together, especially
for KDE's shared graphics mappings. Full AE17 and remote-storage cases remain
open. Physical cold and matched empty-view results are recorded in
[the current-source evidence](../evidence/2026-10-08-ui-process-native-memory.json).
All four matched physical runs pass: closed whole-family RSS falls from 65.09
to 45.04 MiB, with PSS/private savings of 15.00/14.70 MiB. Retain the split for
that benefit while recording higher open-state PSS and slower Settings reopens.
The independent five-minute cold run remains at 44.390625 MiB; 40 MiB is unmet.
Next locate resident styling, KDE's first graphics/theme load and repeated-capture
buffer lifetimes using each desktop's own baseline. Prepared SQL queries already
exist; avoid blind cache changes and integer narrowing without allocation evidence.
A bounded four-run Qt styling probe found only 0.63 MiB settled RSS difference;
keep product visuals unchanged. It is cost attribution, not a product saving.
KDE graphics-load stages and controlled repeated-capture lifetimes remain the
next locating work.
Reuse the three current test VMs; shut
down superseded guests before starting replacements. Stop superseded app residents
after checking their identity and terminal operations; do not leave old versions
running on the dashboards while their replacements build. Keep the needed VM
running for its build, then verify the newly installed version before launching it.

### U10. Packages, performance, and release acceptance

Goal: Installable releases meet the desktop contract and have reproducible support evidence.

Requirements: R13–R15, R20, R24–R34, R36–R37. Dependencies: U4–U9, U11.

Files: packaging/linux/CMakeLists.txt, packaging/linux/autostart.desktop.in, packaging/gnome/*, .github/workflows/release.yml, tests/desktop/package_smoke.yaml, tests/performance/bench_capture.cpp, docs/release.md, docs/troubleshooting.md, THIRD_PARTY_NOTICES.md.

Approach: Complete KTD15 and the Verification Contract using the development package target established in U6 and extended in U7/U8. Exercise fresh install, upgrade, login startup, removal, extension lifecycle, and versioned settings recovery. Inventory licenses, dependency security maintenance, and rebuild inputs before distribution. Build and validate locally before any externally authorized publication.

Execution note: Packaging is verified by installed-session smoke tests, not tests that mirror installer files.

Test scenarios:
1. A fresh VM installs the .deb/extension and completes setup and all actions without source-tree files; it does not activate the put.gd development template automatically.
2. Upgrade preserves customized JSON values, keychain references, retained operations, populated upload history, and deletion outcomes through schema migrations; uninstall removes integration hooks while preserving user-owned outputs.
3. Release builds meet the Success Criteria with the populated 10,000-entry history fixture and stay responsive during history browsing, an upload burst, and induced slow network.
4. The package contains required platform/SQLite plugins, desktop authorization entries, notices, and checksums.
5. A physical Cinnamon run confirms capture geometry, clipboard interop, and actual hardware performance beyond VM evidence.

Verification: Release evidence records every required profile/case, package install outcome, measured budget, and remaining explicitly scoped limitation.

---

## Verification Contract

### Test Layers and Boundaries

| Layer | Runs where | Proves | Does not prove |
|---|---|---|---|
| Unit/contract QtTest | Headless CI | MIME parsing, geometry, state/retry/retention, configuration validation/revisions, history/migrations, provider-neutral receipts | Actual compositor or hotkeys |
| In-process GUI QtTest | Offscreen/Xvfb | Settings/file refresh/conflicts, history search/paging/previews, deletion acknowledgement/states, retry actions | Wayland permissions or background clipboard |
| Offline SDK/IPC/MCP integration | CI with fixture servers | Real adapter requests, upload/deletion lifecycle, versioning effects, JSON/config reload/tool parity, second-provider contract | AWS bucket policy or public custom domain |
| Desktop acceptance | Logged-in QEMU guests and physical host | Global keys, selection, focused target, native Wayland clipboard, paste | Every GPU/driver combination |
| Live S3 smoke | Explicit synthetic fixture | PUT/ACL, unauthenticated public URL bytes, guarded fixture deletion and recorded effect | General capture correctness or instantaneous cache invalidation |

U1 creates the dev/release/sanitizer CMake configure/build/test presets; U6 creates the lab profile runner; U10 creates package validation. These are future repository interfaces, not existing or already-run commands. Exact recipes belong in AGENTS.md and docs/testing.md once the tooling exists.

### Graphical Profiles

| Profile | Baseline | Required evidence |
|---|---|---|
| cinnamon-x11 | Mint 22.3 / Cinnamon X11 | Real user host plus disposable VM |
| gnome-wayland | Ubuntu 26.04 LTS / GNOME 50 | Native Qt Wayland session, installed Shell helper |
| kde-wayland | Kubuntu 26.04 LTS / Plasma 6.6 | Native Qt Wayland session, installed ScreenShot2 entry and data-control |

Each manifest records ISO hash, desktop/compositor/portal/interface versions, Qt platform/backend, app build, display layout, and acceleration. Use two virtual displays with different resolutions/scales plus single-display coverage. Run guests serially if host resources are limited; start with two vCPUs and 6 GiB RAM per guest, then record any required adjustment.

QEMU/KVM is the primary lab; VirtualBox is an optional human reproduction route. QMP key/pointer injection avoids reliance on xdotool inside Wayland. Query supported QMP features and use witness dumps for each display head. Keep host–guest clipboard sharing disabled so it cannot fake the app’s clipboard success.

### Required Desktop Scenarios

Every profile verifies upload-copy enable/disable, content-category folder routing, preserved local copies after upload failure, archive-failure behavior, and remote filename policies with final-name URL paste and all-skipped clipboard preservation. Every profile runs region selection and cancellation, active-window target/focus changes, full desktop, clipboard text/image/two files, local-only result paste, shortcut conflict, offline upload, copy suppression after a new clipboard owner, and installed-package startup.

Every profile also opens persisted history after restart, searches a past item, views available preview/details, copies its direct link into another app, and deletes a selected synthetic upload through the GUI. Verify its remote outcome and surviving local copy/history record. History browsing works offline and uses no cloud listing calls; selection cancellation and local history/cache clearing issue no remote deletion.

Every profile edits config.json externally while the settings GUI is open, using atomic replacement through the lab SSH channel. Verify valid values appear without restart, invalid edits leave accepted values active, dirty forms retain drafts with conflicts, and later actions use the new destination/folder/naming settings while accepted operations keep their original snapshots. Record actual shortcut rebind status when bindings change.

Geometry cases include mixed DPI/fractional scaling, negative origins, gaps, rotation, and display removal during selection. The fixture paints known solid regions and reports intended content/geometry; compare artifact dimensions and stable interior pixels, avoiding decoration/font antialiasing as the oracle. For native interop, repeat image/text paste and file-copy acquisition with a normal GTK/Qt app or file manager.

Every pass needs the actual saved/uploaded bytes plus paste into a second app. A screenshot of the uploader’s own window or a portal capability response is insufficient.

### Failure, Resource, and Native Code Checks

Controlled fixtures cover timeouts, auth/ACL failures, malformed responses, lost successful PUT replies, partial batches, retry identity, unavailable keychain/helper, malformed IPC/native image metadata, cancellation, restart, and shutdown with requests in flight.

History/delete fixtures additionally cover database migration failure, cache eviction, edited/removed destination profiles, partial selection outcomes, replaced objects, missing objects, denied deletion, immutable/noncurrent/null versions, protective markers, interrupted multi-step deletion, and lost DELETE replies. The non-S3 test adapter validates that opaque object references and provider capabilities survive history/CLI/MCP round trips.

Run ASan+UBSan and a separate TSan job over app-owned core/providers/adapters, with dependency instrumentation limitations recorded. Exercise rapid hotkeys, bounded queue/storage exhaustion, late callbacks, and pending-artifact cleanup. Exclude sanitizer/TCG runs from performance results.

Live S3 verification uses only synthetic bytes and unique test objects; record receipt/hash/public GET and the deletion/cleanup outcome. Never delete pre-existing user objects or mutate bucket versioning/retention to create a test. Exercise versioned service behavior through a preconfigured disposable test profile when available and deterministic offline fixtures otherwise; label the evidence accordingly. No credentials are baked into VM images or committed fixtures.

### CI and Evidence Gates

Each change runs appropriate headless and interface tests. Changes to a desktop adapter also require its real-session profile before declaring that behavior verified. The full three-profile matrix runs on a lab/self-hosted runner with a real display session before release; hosted CI is not assumed to expose KVM.

Artifacts include results JSON/JUnit, app/CLI/MCP trace IDs, relevant desktop logs, expected/actual image dimensions and hashes, QMP witness frames, clipboard sink results, upload receipts, and package/version manifests. Sanitize diagnostic exports per R23; intentional synthetic fixture captures remain useful test artifacts.

Windows/macOS CI proves compilation of shared components only. Physical Cinnamon validation and measured release-build budgets supplement VM tests.

---

## Definition of Done

- All R1–R37 requirements are implemented or, for future native platform support under R26, satisfied by the portable boundary and compile checks.
- Each U-ID’s scenarios pass at its stated layer, and every R13 profile has complete real-session evidence for the required workflow.
- AE1–AE15 pass, including public link bytes, duplicate-name policy, persistent history, remote deletion outcomes, provider-neutral contracts, and independent paste behavior.
- No graphical profile is called supported because mocks, offscreen widgets, or portal discovery passed.
- The Success Criteria have recorded physical-machine measurements and no unexplained regression.
- A clean environment can build, test, install, debug, and package through committed documentation; CLI/MCP effects and errors agree with the GUI.
- History survives artifact cleanup and upgrade, remains searchable offline, and reports the original storage destination and accurate deletion state. Remote removal preserves local copies; clearing history never deletes remote data.
- AWS types remain inside the S3 adapter, and the second test provider passes upload/history/link/delete contracts through shared services.
- The documented JSON file and GUI represent the same accepted non-secret configuration; external edits reload with validation/conflict handling, and every development S3 default is configurable.
- Credentials and source content are absent from ordinary configuration, source history, default exported diagnostics, and VM images.
- Release packages contain dependency notices, checksums, integration hooks, and a documented update/rebuild path.
- Abandoned prototypes, temporary workarounds, dead adapters, and unused dependencies are removed from the deliverable.

## Implementation checkpoint: 2026-10-08

This checkpoint records progress without changing the acceptance requirements.
Detailed evidence and limitations remain in [verification.md](../verification.md).

| Work | Implemented and observed | Remaining acceptance |
| --- | --- | --- |
| U5 / R35 | Collapsed simple setup, standard S3 defaults, explicit local-only choice, retained advanced draft, exact MiB/GiB controls and readable history/activity sizes; host and native Qt tests pass | Complete installed visual/focus/error/confirmation inspection at supported scales; full AE15 remains open |
| U10 memory | Deferred AWS initialization and persistent views created on demand; unchanged five-minute physical Cinnamon background run falls from 87.0 to 55.95 MiB peak, with original idle budgets passing; repeated native window activation/reuse passes | Approximately 40 MiB target remains unmet. Savings precede first view/S3 use; measure post-use and populated-history workloads separately |
| U10 capture latency | Reproducible supervisor retains hardware, binaries, parameters, operation IDs, logs and results; five warmups and 30 measured physical invocations complete | Measured p95 painted overlay is 235.821 ms and fails the 150 ms budget; locate and correct the cost before claiming acceptance |
| U6 display observation | Normal desktop selector inspected with a drawn rectangle; KDE VNC incremental corruption reproduced alongside a clean direct guest framebuffer | Exact viewer cause unconfirmed; investigation parked at the user's request, with no viewer source changes |

The integrated application passes all 25 dev tests; four affected SDK/GUI targets
pass memory/undefined-behavior checks with leak detection and instrumented-Qt thread
checks. These do not replace real desktop, live publication/deletion, package
lifecycle, or full U9 walkthrough gates. Structured memory comparisons, including
the rejected candidate, are retained in
[the machine-readable record](../evidence/2026-10-08-resident-memory.json).


The subsequent fresh Cinnamon KVM overlay walkthrough passed normal package
installation, native region capture, independent Qt/GTK clipboard checks and a
local official-SDK MCP/CLI parity/configuration recovery subset. A GTK fixture MIME
negotiation defect was fixed without weakening byte comparisons; its original red
attempts remain recorded. See
[the structured receipt](../evidence/2026-10-08-fresh-cinnamon.json) and
[verification](../verification.md) for parameters, operation IDs and limitations.
This advances U6/U9 evidence; it does not close the full desktop matrix, remote S3,
lifecycle or remaining performance gates.


## User refinement: 2026-10-08

The user requested detailed info icons for every setting, plain labels, neutral
Material/Apple-inspired surfaces, immediately editable simple S3 setup, region
suggestions, filename templates with preserved-file/timestamped-capture defaults,
Cancel and explicit reload/save feedback. These update R20/R29/R35 and supersede
the earlier generated-name default and literal coaching heading. The implementation
keeps Qt Widgets and the same validated configuration, collision protections and
provider-neutral operation pipeline. Payload and preview ownership checks accompany
this pass so images do not accumulate in the resident heap.

This refinement is implemented in the shared settings, artifact and operation
services. The installed Cinnamon form has been inspected with fresh editable S3
fields, neutral/blue styling, detailed info dialogs, searchable regions, all four
filename templates, accepted-save feedback, Cancel and Reload from file. The
latest native region and GTK clipboard-image checks preserve saved/pasted pixels
and timestamped local names. Standard tests pass 25/25; eight affected contracts
pass ASan/UBSan with leak detection and instrumented-Qt TSan. A separate production
CLI/schema check passes. Review findings about schema drift, queued timestamps
and History reopening during deletion have reproduced failures and tested fixes.

Decoded image ownership is released before slow persistence consumers; cancelled
queued images return their capacity. The configurable pending-image limit defaults
to 64 MiB, and History retains one bounded selected preview rather than an image
per row. These checks prove lifetime/admission behavior, not post-use RSS or the
40 MiB target. Full AE15 scales/states, remote S3 and remaining desktop, lifecycle,
MCP and performance gates remain open. Exact parameters, hashes and receipts are
in [the structured record](../evidence/2026-10-08-settings-refinement.json).

The memory continuation preserves Cinnamon-native fonts and dialogs by explicit
user choice. The reviewed private CMake S3 module defers mapping the official SDK
until network work begins. A same-observer five-minute physical comparison falls
from 56.270 to 51.887 MiB peak RSS, with idle CPU at 0.003331% of one core. The
7.789% improvement is retained at optimization revision `cadd9ed` and integrated
into the canonical source; all 26 combined development tests pass. Approximately
40 MiB and post-S3 memory acceptance remain open. Three empty-view cycles finish
at 79.551 MiB; populated history and upload costs require separate measurements.
[The SDK evidence](../evidence/2026-10-08-sdk-memory.json) retains both original and
final comparisons, exact identities, full raw results and the actual review.
Commands and observation limits are in [testing-memory.md](../testing-memory.md).
A temporary custom-style probe saved only 0.719 MiB, below the declared threshold;
keep the custom appearance and pursue larger measured opportunities.

The synthetic post-use allocator probe found 9,547,776 bytes (9.105 MiB) of reclaimable RSS after five desktop/image cycles, with allocated arena bytes unchanged at 3,366,480. Add a bounded idle heap reclamation pass after payloads and operation workers drain, preserve native clipboard ownership and live UI state, and measure repeated-use RSS, idle CPU and next-action latency. This R20 follow-up is separate from the fixed cold-idle optimization metric: it cannot establish the 40 MiB target. [Protocol, complete sanitized raw results and temporary probe source](../evidence/2026-10-08-payload-allocator.json) are retained for replay.

### R1/R20 implementation checkpoint (2026-10-08)

Live transparent region selection is implemented across the adapter paths. Native
Cinnamon evidence verifies a changing background, normal and rapid-release pixels
without the outline, independent Qt paste, and Escape/right-click cancellation.
New GNOME/KDE graphical and mixed-display evidence remains required.
[The bounded record](../evidence/2026-10-08-live-region.json) preserves the exact
source, binary, protocol and sanitizer identities.

Production glibc cleanup runs once after accepted capture/clipboard work and local
image/text decoding become idle. It also covers cancelled acquisition. Five local
capture/image cycles finished at 66.473 MiB, compared with 75.648 MiB in the earlier
attribution build; the maximum observed allocator call was 750 microseconds.
Final coherent development contracts passed 25/25 and affected memory/thread
checks passed. This is separate from cold idle and post-S3 memory acceptance.
[Cleanup evidence](../evidence/2026-10-08-heap-reclaim.json) and
[replay instructions](../testing-memory.md) retain exact parameters and structured
results. Keep approximately 40 MiB, populated-history/upload memory, next-action
latency and the remaining desktop/release gates open until measured.

The post-use lab runner now attempts restoration even when VNC input cleanup fails,
saves the failure status, and propagates the error. Fault injection covers input,
restoration, combined and pre-start failures; this is orchestration proof, not a
new graphical acceptance result. Combined SDK/live-selection/cleanup development
verification passes 26/26 (112.90 seconds).

### Native IPC memory follow-up (2026-10-08)

The user approved replacing the Linux resident's Qt local sockets with native
Unix sockets to remove its eager Qt Network dependency. Preserve the versioned
JSON protocol, private socket ownership, live-resident protection, framing and
client limits, deadlines, response delivery, and CLI/MCP behavior. Other platforms
retain Qt local sockets; native Cinnamon fonts/dialogs remain required. The
experiment is bounded to three variants/two hours with no new dependencies.

The reviewed implementation is committed as `1fdfff0`. All 28 development
contracts and four affected Release, ASan/UBSan and instrumented-Qt TSan contracts
pass. IPv6 compatibility and pending-reply disconnect regressions were reproduced
and fixed without loosening validation. The final isolated five-minute physical
comparison measured 46.020 MiB versus 52.012 MiB for the retained reference
(11.52% lower). Native GTK remains loaded, with no operations or view activation
observed. The earlier 45.625 MiB sample predates the disconnect fix and is retained
separately. Combined integration passed 28 development and four affected Release contracts;
the committed whole app measured 46.188 MiB over another five minutes. Keep 40 MiB and
post-upload memory acceptance open. [Raw results and commands](../evidence/2026-10-08-local-ipc.json)
record the baselines, fix, review receipts and all comparisons.

### Journal latency follow-up (2026-10-08)

Investigate SQLite connection reuse, prepared-statement reuse and page-cache
management alongside the memory work, as requested. Preserve durable operation
ordering, crash recovery and private journal files. Attribute the actual waiting
call before choosing a change; lower memory use alone does not prove lower latency.

The temporary native trace measured warm p95 160.004 ms against the unchanged
150 ms gate. All 35 operations cancelled without artifacts. Long journal
initialization intervals overlapped the previous cancellation commit; the trace
has not split permissions, foreign-key setup and schema-version reads. Repeated
query preparation is present, but is not established as the cause of that wait.
Retain separate tests for populated history and capture/cancellation journaling.
[Source identities, parameters and complete traces](../evidence/2026-10-08-region-latency-raw/manifest.json)
remain available for replay; the timing gate remains open.

The standalone rollback/FULL probe completed twelve fresh-process cases with two
matched repeats. Prepared-statement cost was small: explicit prepare medians
were 0.062–0.202 ms across three durable states. Connection/statement reuse had
no consistent timing winner; durable commits and controlled lock contention
dominated the synthetic timings. This does not identify every waiting call in
the actual product. No product SQLite change was made from this probe, and the
150 ms gate remains unpassed. The controlled reader's `beginNs` is overwritten
by its later write, so absolute reader start is unavailable; phase durations and
writer timestamps remain usable. [Evidence](../evidence/2026-10-08-sqlite-locating.json)
and [the bounded report/replay](../evidence/2026-10-08-sqlite-locating-raw/report.md)
preserve all samples and source/build/library identities, without databases or
payload bytes. Synthetic cache RSS findings do not establish product memory
savings or populated-history/recovery/concurrency acceptance; no WAL case ran.

The approved startup reclamation follow-up is kept as `68e5549`, integrated as
`fc69d38`. It requests one pass through the existing two-second idle gate after
successful recovery. The unchanged five-minute physical comparison fell from
46.1875 to 44.9648 MiB, a 1.2227 MiB (2.65%) saving, above the predeclared 1 MiB
threshold. Native Cinnamon fonts/dialogs remain enabled; no dependency was added.
Three native empty-view cycles reopened Settings, History and Activity, ending
at 73.8242 MiB after hiding them. Track that post-use retention separately from
cold idle. [The evidence](../evidence/2026-10-08-startup-reclaim.json) retains the
test-first regressions, source review, frozen commands and complete measurements.
The 40 MiB target remains open. Further work needs an attributed source of savings
outside the once-only startup request; repeated trimming is not this experiment.

The next post-view investigation is native backing-store retention: source and
buffer-size attribution account for roughly 8 MiB of Xorg shared mappings after
the three empty windows close. Preserve widget state and unsaved drafts, native
fonts/dialogs, complete repaint and minimize/reopen behavior while measuring any
release strategy. This is separate from the cold 40 MiB target; heap live/free
allocation accounting is still needed before proposing additional cleanup.

Evaluate integer-width and member-layout suggestions only against measured live
object counts and actual ABI sizes. Fixed-width aliases alone are not a memory
optimization. Keep file sizes, limits and counters wide enough for validated
values and API/serialization contracts; do not introduce packed structs or blind
narrowing. Any selected layout change needs repeatable compiler/architecture
metadata, before/after sizes, range tests and workload RSS/latency evidence.


### Current-source cross-desktop memory prerequisite (2026-10-08)

User-directed: memory work must keep cross-functional tests moving in parallel
across Cinnamon/X11, GNOME/Wayland and KDE/Wayland. Freeze one candidate source
revision/archive; build with each distro's normal Qt/SDK toolchain, run the core,
widget and integration contracts, then exercise native capture, clipboard,
cancellation and view lifecycle scenarios. Retain exact package and actual
running executable identities; reject an old resident surviving installation.
Older guest code must never stand in for current-candidate evidence. Independent
profiles build/test in parallel, with separate run/build state; quiet physical
measurements retain their serial workload after the functional wave settles.

The 8767 dashboard audit confirmed valid RSS for older VM builds, including
179.22 MiB KDE RSS with 66.67 MiB PSS. No dashboard fix or leak verdict follows
from that snapshot. Refresh the test wave on current source, record every blocked
profile, and keep cold/post-view/post-action memory distinct. Stop superseded
VMs after copying needed packages and evidence; retain necessary SDK/build caches
powered off and never remove a referenced golden disk. [Replay and workflow](../testing-vm-memory-audit.md)
and [the original audit](../evidence/2026-10-08-vm-memory-audit.json) capture the
reason for this requirement. The 40 MiB target and outstanding desktop/release
acceptance gates remain open until their own evidence passes.

Execution evidence: [the current-source matrix](../evidence/2026-10-08-current-desktop-matrix.json)
now records 12 passing native cases per profile, repeated Settings/History/Activity
opens, GNOME's additional live-background region proof, and new 28/28 Dev suites
on GNOME/KDE. Cinnamon reused the unchanged product's earlier full Dev/sanitizer
evidence and passed fresh native cases. A common two-minute post-use observation
measured RSS 83.30/114.36/186.17 MiB for Cinnamon/GNOME/KDE. The old three VM
processes were stopped. The runner checks live package identity and retains
failed attempts; 11/11 host contracts pass and their JSON/log reports are wired
into Linux CI. This closes the bounded current-source test follow-up, not the
remaining cloud, mixed-display, package lifecycle, latency or 40 MiB gates.

### Memory improvements on all three desktops (2026-10-08)

User-directed: GNOME and KDE are optimization targets as well as functional
regression profiles. Measure each desktop before and after the same candidate
and workload. A Cinnamon saving alone does not establish a Wayland improvement.
Keep native integration enabled, including Cinnamon fonts and folder dialogs.

Read-only attribution found 26.68 MiB of resident Wayland shared buffers on
GNOME and 26.72 MiB on KDE after the current native test wave. Two fresh physical
Cinnamon view runs ended at a median 73.59 MiB RSS, including 8.02 MiB of Xorg
shared buffers. The next proposed experiment releases native drawing resources
after Settings, History and Activity close while retaining their logical state.
Unsaved drafts, selection, filters, active dialogs, minimize behavior, rapid
reopen, complete repaint and first-reopen latency are acceptance requirements.
The user approved this experiment. Candidate 001b has completed its frozen
measurements: physical Cinnamon median RSS fell from 73.59 to 65.38 MiB;
GNOME post-use RSS fell from 114.36 to 86.05 MiB; KDE post-use RSS fell from
186.17 to 157.23 MiB. These are standalone candidate measurements. Native state/dialog checks passed
with recorded compositor limits; the change is integrated as `0a2b104`. The
[replay and structured evidence](../testing-view-surfaces.md) preserve source and
binary identities, failed attempts and comparison caveats. The candidate's
five-minute cold run ended at 44.91 MiB, so the 40 MiB target remains open.

Record RSS, PSS and private memory independently. KDE's shared LLVM/Gallium
libraries explain part of its higher RSS but are not a reason to discard the RSS
goal. Preserve current-source build identities, all native functional cases and
visible VNC evidence for every candidate. Save the frozen measurement parameters,
commands, failed attempts and raw structured observations. The [attribution and
replay notes](../testing-memory-attribution.md) distinguish this post-use work
from the still-open approximately 40 MiB cold-start target.

### On-demand UI process investigation (2026-10-08)

The user approved a process split and explicitly requires keeping the tray icon.
Build an on-demand UI process which exits after
its windows close, preserving native fonts/dialogs, safe drafts, live status,
clipboard ownership and ongoing uploads. The CLI already calls the resident;
keep one operation engine behind both interfaces. The tray and capture overlays
also depend on Qt Widgets, so moving the large windows alone does not unload it.
The [feasibility analysis and experiment gates](../resident-ui-process.md) record
the current dependencies, IPC gaps and required measurements. The split is implemented and retained after source-41 native and memory checks.
Its measured closed-family RSS saving is about 20 MiB; cold idle still exceeds
40 MiB. Replacing Qt with a custom visual framework was considered; the current
decision keeps Qt and native integration while measuring remaining costs.

### Breakout view refinement follow-up (2026-10-08)

Retain U11's on-demand UI with a persistent resident tray. The measured source-41
closed-family RSS saving is about 20 MiB; the 40 MiB cold target remains unmet.
The user approved a separate bounded Settings-startup investigation from the
333 ms mapped-reopen baseline, targeting about 200 ms without retained helpers,
new dependencies, changed native fonts/dialogs or memory regression. Locate costs
before implementation and preserve exact source/overlay/binary identities.

The next U5 visual pass clarifies a saved empty destination list: show Add S3
destination as the primary action and hide inactive destination/credential forms.
Preserve first-launch unsaved S3 setup. Align destination and credential inputs in
both simple and advanced views. History prioritizes names/date/independent outcomes,
retains MIME/destination in the selected inspector, and stacks that inspector on
narrow windows. Activity favors filenames, keeps full IDs in routing/tooltips and
uses compact commands. No image cache or persistent UI helper is added.

Dev 41/41 and affected GUI 3/3 pass, including meaningful empty-form/alignment
failures before their fixes. The completed local/Anthropic review has no retained
actionable findings. These are headless/scoped checks: frozen current-source
Cinnamon/GNOME/KDE views, normal close/reopen, independent Copy result paste and
small-window inspection remain required before visual acceptance. ASan/UBSan and
native replay outcomes will be recorded separately; prior witnesses are not
relabelled. See [the refinement checkpoint](../evidence/2026-10-08-ui-refinement.json).

Source `f56dc72` (version `0.1.1-dev.43+gf56dc72de4fe`) freezes the view
refinement. The affected ASan/UBSan GUI suite passes 3/3. All three previous lab
residents were stopped after identity/idle checks, and their complete profiles
were preserved before building replacements. Current native acceptance is in
progress; build execution is not graphical acceptance.

The original-source startup diagnostic attributes reopened Settings construction
and showing to about 116–129 ms, native QApplication initialization to 56–59 ms,
and bootstrap/snapshot/activation to under 7 ms. The independent mapped observer
adds roughly 108–140 ms after CLI acknowledgement. These instrumented observations
locate costs and establish no product saving. Measure hidden-page construction
and polish before selecting a lazy-page change. Preserve the unchanged primary
harness and reject changes that merely report readiness earlier.


Startup attribution follow-up: the complete diagnostic replay material is exported
in [view-startup attribution](../evidence/2026-10-08-view-startup-attribution.json)
and [the replay guide](../testing-view-startup.md). Hidden advanced page construction
and show-phase Polish account for about 41–44 ms on two reopens. One isolated lazy
page experiment is underway; no latency improvement or 200 ms achievement is
claimed. Its draft/validation, native desktop and memory gates remain required.
Node-based GNOME ownership tests have been replaced by the desktop's existing GJS;
no Node installation is part of guest testing. Lab logins remain protected local
bootstrap input and are entered through visible masked console fields, never test
arguments or structured logs.

The clean lazy-page checkpoint `6f6372e` (version
`0.1.1-dev.46+g6f6372e79d8b`) passed twelve native cases per desktop, lazy-page
Tab/Shift-Tab traversal, visible trays and normal Settings/History/Activity helper
exit. Two alternating physical runs per build measured 332.50 ms for clean45 and
295.43 ms for the trial: 37.07 ms faster, below the approved >40 ms keep threshold.
Both original-source and matched-source decisions are inconclusive. Preserve the
experiment and its [confirmation evidence](../evidence/2026-10-09-lazy-settings-confirmation.json);
do not integrate it or claim the ~200 ms target. The product retains source45's
reviewed tray correction and source43's independent UI improvements.

Further memory work must locate resident initialization and KDE graphics-library
loading before changing them. A Qt Core/Gui resident with a native tray protocol
is a hypothesis, not a measured saving; retain Qt Widgets for the on-demand views,
native fonts/dialogs, all capture/clipboard behavior and whole-family accounting.
The 40 MiB cold target remains open. Preserve distinct cold, post-use guest and
open-view measurements, fixed parameters, exact commands, failed attempts and
RSS/PSS/private observations. Agents must preflight complete Unix socket paths
and restore locked sessions with protected local lab input through the visible
masked console rather than handing routine lab unlocking back to the user.

Future candidate testing starts with one Cinnamon build and its required
functional and memory checks. If either gate fails there, fix or reject it before
building or testing GNOME/KDE. Once
Cinnamon passes, remaining independent desktop checks can run in parallel;
cross-desktop acceptance still requires their current-version evidence.

The fresh source45 five-minute physical cold reference is 45.414 MiB RSS,
25.506 MiB PSS and 23.066 MiB private, with 0.0033% of one CPU core and no
activation or operations. It passes the original idle budget, but exceeds the
40 MiB target. See [cold-reference evidence](../evidence/2026-10-09-cold-reference45.json).
Cinnamon-only initialization locators measured about 11.34 MiB settled RSS for
QCoreApplication plus a session-bus connection, 37.32 MiB for QGuiApplication
and 40.04 MiB for QApplication. These small programs implement no tray, capture
or operation service; their difference identifies a promising boundary and is
not a product saving. See [initialization evidence](../evidence/2026-10-09-native-initialization.json).

The next memory investigation targets the basic listener and tray, deferring
graphical initialization until work needs it. Existing native Unix resident IPC
remains the first choice; CLI communications costs need separate attribution.
Several compatible improvements may form one candidate: acceptance is based on
the combined whole-family result and preserved features, not a requirement that
one isolated change reach 40 MiB. Save individual and integrated comparisons
separately, and never add deltas measured against different baselines.


The clean native-listener candidate `676f6e6` reached the physical Cinnamon
40 MiB memory target in two independent cold/closed measurement pairs: cold
24.676/24.555 MiB peak RSS, closed family 25.229 MiB median RSS versus 45.600 MiB
reference. Native fonts/dialogs, tray and on-demand views are preserved. Source49
passed host contracts and twelve visible native Cinnamon cases. This experimental
branch is not integrated: region selector mapping/painting regressed by about
100 ms median and both the candidate and matched original control miss the
150 ms timing gate. Current-source GNOME/KDE each passed twelve native cases and normal view
helper exit. Post-use VM RSS/PSS was 60.219/34.001 MiB GNOME and 138.559/36.995 MiB
KDE, compared with 28.520/18.383 MiB Cinnamon. Their graphical fallback is unchanged;
a Wayland 40 MiB RSS result remains unachieved. These VM observations are separate
from the physical target and have no matched old-guest baseline. See
[native-listener replay](../testing-native-listener.md) and
[structured evidence](../evidence/2026-10-09-native-listener.json). Retain this
memory result while fixing capture responsiveness; do not weaken its timing gate
or infer a Wayland 40 MiB win from Cinnamon.


Native view inspection also found a remaining polish task: Activity's narrow
Details column elides size/status text. Refine that presentation with readable
column allocation or an accessible selected-item detail view, preserving the
MiB/GiB display and full tooltip explanations. Clean painting and native helper
lifecycle checks establish functionality, not completed visual design.

The source63 KDE Settings inspection also found that the header still says
“Saved settings are active” while a notification checkbox has an unsaved draft.
The next visual pass should distinguish unsaved changes from the active saved
configuration, with clear state after Cancel, Save, and Reload. Verify the draft
and persisted document separately; a successful save must keep its visible
feedback. This wording refinement does not substitute for those functional checks
or authorize changing the frozen memory workload.

### Independent desktop memory acceptance (2026-10-09)

User-directed: retain the passing Cinnamon memory changes if GNOME or KDE misses
its memory target. Treat each desktop's measurements and remaining work
separately; a Wayland memory miss does not trigger a Cinnamon rollback. Improve
GNOME first, then KDE, while preserving the passing Cinnamon behavior and
measurements. Feature regressions still require correction before that desktop
can be accepted. Keep cold, post-view, post-capture and post-upload workloads
distinct, and record each build identity and whole-family RSS/PSS/private memory.
Do not promote a Cinnamon result into GNOME or KDE acceptance.

The approved GNOME experiment changes only GNOME startup routing and its bridge
constructor, preserving Cinnamon's environment guard and capture implementation.
Rebuild and test Cinnamon to detect shared-startup regressions; do not refactor
its implementation while optimizing GNOME. The verified source55 GNOME
post-local-native baseline is 65.527 MiB peak RSS, 38.920 MiB PSS and 35.348 MiB
private over 61 samples/300 seconds. This is a populated VM observation, with
remote-history acceptance pending; it establishes neither cold nor full-parity
acceptance. The experiment retains native on-demand settings/history fonts and
folder dialogs and tests the existing native tray on GNOME.

Candidate `578f6c29` (version `0.1.1-dev.56`) passed the unchanged Cinnamon
regression workload: twelve native cases, 117.6 ms region-selection p95,
24.6 MiB cold peak RSS over 300 seconds, and 25.1 MiB worst settled RSS after
closing views. Native dialogs/fonts and transparent live region selection were
verified; all three view-helper generations exited normally. This one-pair
regression confirms preservation of the already passing Cinnamon implementation.
GNOME memory is still unmeasured, and review found a GNOME Qt-argument routing
regression that must be corrected before retaining its candidate. See the
[bounded regression record](../evidence/2026-10-10-gnome-candidate-cinnamon-regression.json).

Live S3 provider testing now passes against the isolated synthetic root object:
upload, unsigned public URL byte comparison and guarded deletion succeeded.
Ordinary fresh root-name HEAD returns missing as required for collision checks.
The actual source55 Cinnamon resident also uploaded one native synthetic text
item, passed unsigned public-byte verification, and copied its actual URL into
the independent native receiver. The operation and History receipt identities
match. Actual History Copy link also passes after a verified clipboard reset;
the selected test object was guardedly removed, its History entry shows Removed,
and the original configuration was restored. The view helper exited normally
and the owned test fixture was stopped. A single post-cloud diagnostic measured
49.14 MiB RSS; this is not the frozen memory acceptance workload, and post-upload
memory remains an open gate. The test resident then exited normally, with its
exact process generation and session socket absent. Credentials remained local
and session-only throughout; no memory-zeroization claim is made. Follow
[the repeatable live S3 procedure](../testing-live-s3.md) with fresh synthetic IDs.


### Current GNOME boundary checkpoint (2026-10-10)

The corrected isolated candidate is `2a02118ffc7bc` / version
`0.1.1-dev.57+g2a02118ffc7b`. Its GNOME Core route preserves Qt GUI argument
parsing by falling back to QApplication when graphical or unknown arguments
are supplied. Cinnamon implementation remains unchanged.

Current-source57 Cinnamon regression passed the twelve native cases, native
tray/dialog/font and clipboard checks, live region selection, and normal view
helper teardown. Physical region-selection p95 was 113.6 ms; the 300-second
cold run peaked at 24.62 MiB RSS, and settled closed-view RSS was at most
25.19 MiB. A separate unchanged-workload supplement verifies all three normal
helper exits. These are the existing cold/view regression workloads, not
post-upload acceptance; preserve the separate 49.14 MiB source55 post-cloud
diagnostic and its open gate.

Source56 GNOME passed all fifty registered guest tests and the twelve native
capture/clipboard cases. Root also verified the native tray, production
notification delivery in GNOME's notification center, native fonts/folder
dialog, live background and crosshair, fresh independent parent/file Activity
pastes, and normal helper exits. The transient notification banner was not
observed. This is local-native diagnostic evidence with remote History
link-copy still pending; source56 remains a locating candidate because its Qt
argument regression is corrected only in source57. The unchanged 300-second
locating run measured 63.01 MiB RSS, against the 65.53 MiB reference, and missed
the target. A bounded process-environment check proved that the lab-launched
resident lacked WAYLAND_DISPLAY and therefore used QApplication: these native
checks prove the graphical fallback, not the intended Core route.

Read-only user-manager environment provenance confirms that it publishes
wayland-0 and its owned native socket; the lab copied an earlier compositor
environment. GNOME-only test-launch discovery now uses that verified client
environment, preserving the product guard and Cinnamon implementation. Clean
source57 passed all fifty registered guest tests, installed normally and
actually selected the Core route. Its initial 26.72 MiB RSS snapshot is not
memory acceptance. Its current-version local-native capture/clipboard, tray,
notification, native dialog, live selector, Activity and normal-exit gates passed.
Both 300-second post-use runs passed at 32.77 MiB peak RSS, with worst CPU
0.0334% of one core. The user-approved 30-minute extension completed the fresh
empty-profile cold confirmation at 26.95 MiB peak RSS and 0.0067% of one core,
with 61 valid samples over 300.000 seconds. Independent numeric/lifecycle review
confirmed normal shutdown and the unchanged populated profile. Source57 is kept
in its clean isolated checkout for this scoped GNOME VM memory experiment;
main integration and remote History link-copy remain pending. Cinnamon's
implementation remains unchanged. The two orchestration failures before sampling
are preserved and documented rather than counted as observations.
Follow [the repeatable GNOME procedure](../testing-gnome-memory.md). The corrected
discovery currently lives in the pinned run adapter; incorporate it into the
repository lab runner with focused contracts before claiming clean-checkout replay.

Keep deterministic parameters, source/build/process identities, all failed
attempts and structured logs. Fix only the specific test-input problem when
an owned fixture fails its privacy checks; do not weaken application, clipboard
or timing oracles. GNOME work precedes KDE, and neither desktop inherits
Cinnamon acceptance. The complete native, remote, geometry, visual, MCP and
release gates in this plan remain required.

### KDE memory baseline follows the checkpoints (2026-10-10)

User-directed: proceed to KDE while preserving the accepted Cinnamon and GNOME
memory work. Use unchanged source57 for the baseline, then isolate any KDE
implementation candidate. New candidates still pass Cinnamon functional and
memory checks first; shared startup changes also require GNOME regression.

KDE's standard development/release builds and package completed against its
distro Qt. All 49 registered tests passed. The exact registry matches the
source57 host; GNOME has one additional optional GJS ownership test. Preserve
the rejected cross-distro count assumption and original successful JUnit rather
than rebuilding to satisfy a number. The package is installed and the live
resident matches source57. All twelve native Qt/GTK capture and clipboard cases
passed on KDE's single-head Wayland session. Focused tray, native folder-dialog,
production notification, live background, Activity copies and normal helper exits
passed. Actual cursor alpha proves the complete native crosshair; the installed
QEMU's legacy VNC mask loses its partially transparent pixels, so full crosshair
visibility on the display wall remains a documented infrastructure limitation.
The five-minute KDE baseline passed measurement integrity at 139.42 MiB peak RSS,
33.96 MiB PSS, 21.15 MiB private memory and 0.0667% CPU of one core. All 61
samples covered 299.958 seconds, with no live UI/helper and constant RSS.
The 40 MiB RSS target remains unmet; current cold memory was not measured.
At this baseline checkpoint, no KDE optimization candidate had been built.
Historical KDE memory figures are not this baseline. These local checks do not establish remote History behavior,
multi-item Activity subset copy or the full geometry profile.

Follow [the KDE measurement procedure](../testing-kde-memory.md). Preserve
native dialogs/fonts, tray/menu, live transparent selection, focus/output
guards and normal helper exits. Attribute cold and post-use RSS/PSS/private
memory outside scored sampling windows before choosing changes. Record actual
baseline and experiment evidence separately; a KDE memory miss does not undo
the passing sibling desktops.


### KDE memory experiment ordering — approved 2026-10-10

Optimize and iterate on KDE first. Establish a satisfactory KDE candidate using
its functional and memory checks before starting both Cinnamon and GNOME
regressions. Both regressions remain required before accepting the candidate.
Do not run the full three-desktop matrix after every small KDE change. Retain
the accepted Cinnamon/GNOME routes and existing frozen measurement gates.


### KDE generic-image parity and next candidate — approved 2026-10-10

Source60's limited native diagnostic reached 40.195 MiB RSS after text, window,
desktop and Escape checks; this remains above the 40 MiB target and is not the
full acceptance workload. A distro Qt probe also exposed a reachable abort when
font-bearing SVG is decoded inside the Core resident. Preserve that failure;
all previously supported formats remain part of feature parity.

The user approved one further combined candidate and six additional hours:
use a temporary windowless image-job mode in the existing Qt view helper for
generic codecs/font services, retaining direct PNG paths, native dialogs/fonts
and normal helper cleanup. Combine this with a guarded KDE-only full-output
frame-sharing path; shared Cinnamon/GNOME composition remains unchanged.
Record clean versioned source, host integration checks, actual SVG clipboard
and regular-file preview evidence, then KDE functional/memory confirmation.
Run both exact-candidate sibling regressions only after KDE is satisfactory.
This approval does not establish a passing candidate or memory result.


### Combined KDE memory target — clarified 2026-10-10

The target applies to cumulative changes. Each change need not independently
reach 40 MiB; evaluate the complete candidate against the original source57
baseline while preserving all functional, idle-CPU and memory gates. The user
approved one fourth KDE-only allocator candidate within the existing ten-hour
limit and unchanged deadline. Its clean checkpoint is source63, version
`0.1.1-dev.63+g050b702c6a6d`.

The fourth candidate combines the Core listener/tray, transient region/image
helpers, explicit PNG paths and frame sharing with a fixed glibc allocation
threshold. A diagnostic after selected real capture/clipboard/SVG checks reached
29.813 MiB RSS. This is a promising snapshot, not acceptance. Complete KDE
native/UI and repeated cold/post-use checks, then current-candidate Cinnamon
and GNOME regressions. Preserve the passing sibling routes and every failed
attempt. Remote-storage/history and full declared desktop gates remain separate
until verified on the current version. See [the KDE procedure](../testing-kde-memory.md).


### Current63 regression checkpoint — 2026-10-11

The user approved a two-hour extension for existing regression tests through
2026-10-11 05:28 UTC. This adds no optimization candidates, dependencies or
desktop-setting changes. The cumulative KDE candidate passed its local native
and focused GUI gates, two post-use five-minute windows at 30.051 MiB peak RSS,
and a fresh cold window at 27.914 MiB. The matched source57 post-use baseline was
139.422 MiB. These are integrated results, not allocator-only contributions.

Current63 Cinnamon passed twelve native Qt/GTK cases and focused tray,
settings, native fonts/folder chooser, local History, Activity independent
clipboard paste and live selector checks. Physical capture timing passed at
121.273 ms p95, with all 35 operations cancelled without results. The physical
cold five-minute run peaked at 24.762 MiB; three original closed-view cycles
settled at 25.004–25.293 MiB RSS. A separate unchanged-workload supplement
verified normal GUI exit generations 1, 2 and 3. The original and supplemental
runs remain separate because the supplement adds three read-only queries.

Current63 GNOME regressions are the remaining local acceptance gate. Preserve
the passing Cinnamon implementation, and park completed unused lab VMs normally
before resuming GNOME. Keep VNC output visible. Verify the current package against the guest before
installation; if distro package names differ, build the same source commit with
the guest’s standard CMake/Ninja toolchain and official SDK. Do not force-install
or rewrite package dependencies. The source63 GNOME host-package reuse attempt
was rejected after an actual dependency check; its normal guest build is running. Remote S3/history and full U9
acceptance remain pending; local memory results do not satisfy those gates.
The content-free samples and reproducible checks are recorded in
[the source63 evidence](../evidence/kde-source63/reproduction.md).

### Current GNOME regression checkpoint — 2026-10-11

The exact source63 guest build and all52 registered tests passed using the normal
CMake presets, Ninja and CPack. All12 native Qt/GTK action cases passed on the
current resident, and the synthetic fixtures drained normally. Settings Cancel
preserved the entire configuration and revisions; Save changed only the intended
notification setting; Reload matched the saved document and revisions; the final
restore returned the entire original configuration. These checks do not establish
the remaining focused History/Activity/live-selection gates or GNOME memory
acceptance. Preserve the passing KDE and Cinnamon checkpoints while completing
those gates. Portable metadata is in [the source63 evidence](../evidence/kde-source63/reproduction.md)
and `../evidence/kde-source63/gnome-guest-build-a1.json`, `gnome-native12-a1.json`
and `gnome-settings-a1.json`. No payload or raw configuration is included.

The regression-only window ended at `2026-10-11T05:28:20.881985Z`. Before that
cutoff, the actual native GTK folder dialog cancelled normally and local History
displayed11 entries plus the selected synthetic image preview. Settings and History
helpers exited normally (generations3 and4). The same current GNOME resident/VM
remains available with UI stopped, operations0, tray present, journal ready and the
original configuration restored. Activity parent/item copy, live selection/cursor,
final local packet and two post-use plus one fresh-cold300-second GNOME memory
windows remain required. An additional30 minutes was requested but not approved;
do not treat this checkpoint as full regression or memory acceptance. See
[the partial GNOME checkpoint](../evidence/kde-source63/gnome-focused-checkpoint-a1.json).
