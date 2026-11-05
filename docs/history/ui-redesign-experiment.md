# UI redesign experiment — issue 40

> **Historical record.** This page describes an app version, experiment or plan that no longer exists and is kept for context only. The current design is in [overview](../overview.md); see [visual-language](../visual-language.md). Do not use it as a work list.

This is the historical 2026-10-15 visual experiment record. The user approved the
first visual round and expanded testing on 2026-10-16. Current merge-preparation
source and acceptance are recorded in the [dated acceptance receipt](../evidence/2026-10-16-issue40-redesign-acceptance.md).
Deferral statements below describe their original checkpoints.
The subsequent visual-language contract was [Graphite](../visual-language.md). Since 2026-10-29 the app style is `putgd::Ui`; the names below are the ones used at the time.

Baseline: freshly fetched `origin/main` `50fd37c`, branch `codex/ui-redesign-experiment`.
Source: user-supplied `put.gd-ui-brief.zip` and `BRIEF.md` (2026-10-15).
The user requested implementation through visual completion in a dedicated directory, local checks only until visual approval. The brief's embedded pause after survey does not override that request. The user explicitly excluded the tray icon pane. No tray source or resident styling is changed.

## Survey before implementation

The app uses C++20 / Qt 6 Widgets, Qt API floor 6.4.2, dynamic system Qt, CMake/Ninja and the official AWS SDK. Fresh main contains working S3 and Proton Drive implementations. The initial project checkout was stale; its S3-only survey was corrected before implementation.

`src/ui/SettingsWindow.*` owns settings/destination widgets. `src/ui/RecentOperations.*` owns activity; `HistoryWindow.*` owns persistent receipts, previews, sharing, guarded deletion and maintenance. `UiApplicationController` opens these on demand through the resident's private pipe. These views are being consolidated inside `MainWindow`, keeping the resident endpoint authoritative.

“Save locally” previously changed all four `actions.*.destination` fields to `local` in the draft, then required Save. It never captured or uploaded anything. Capture now selects only the upload destination, with an optional per-action destination exception. General → Keep a local copy controls local saving independently. No destination plus local copies on saves locally; no destination plus local copies off fails before acquiring content. “Reload from file” discards the draft and credential inputs, asks the resident to reload, and preserves the last accepted configuration when the external file is invalid. “Configuration details” showed config path, accepted revision and observed revision; those move to Advanced diagnostics.

The pipeline emits `OperationService::changed(QString id)` on state/progress changes and `initialized()` after journal recovery. The UI receives invalidation through `UiSession::operationsChanged()`, then reads `operation.recent`/`operation.inspect`. No separate completion/failure/cancellation Qt signal is invented: operation `state`, item `uploadStatus`, `phase`, `uploadProgress`, `copyStatus`, independent local outcomes and errors are the existing contract.

Atomic writes already use `SettingsService` with `QSaveFile`, lock and expected revision. The redesign reuses it. Secrets remain explicit private IPC credential setup, never config/autosave/UI checkpoints. Invalid edits are retained visibly; independent valid edits can persist through field patches. External revision conflicts preserve the draft until reload.

## Config mapping and defaults

Stored units/types are unchanged. `*` means each `region`, `window`, `desktop`, `clipboard`; `profiles[n]` is the selected destination.

| Current label / key | Type, stored unit, default | Redesign location / label |
| --- | --- | --- |
| `schemaVersion` | integer, `1` | unchanged, internal |
| Storage service — `profiles[n].provider` | string, `s3` or `proton-drive`; no profile initially | Destinations / Provider |
| Destination ID — `profiles[n].id` | stable string; generated for new destination | Hidden; Copy details |
| Name — `profiles[n].label` | string, S3 destination for new S3 | Editable destination title |
| Upload routing — `actions.*.destination` | enum string, `upload`; legacy `local` means no upload; `default` inherits | Capture / Destination (No destination turns uploads off) |
| Destination — `actions.*.profileId` | string or null; null initially | Settings / Capture / Destination |
| Publication — `actions.*.publicationMode` | optional enum, `default` when absent; `share` / `private` supported | Settings / Capture / Sharing |
| Shortcuts — `actions.*.shortcut` | Qt PortableText string; Ctrl+Shift+1/2/3; clipboard empty | Shortcuts and capture rail |
| Bucket — `profiles[n].settings.bucket` | string, empty new S3 | S3 / Where files go / Bucket |
| Region — `.settings.region` | string, us-east-1 new S3 | S3 / Region |
| Endpoint — `.settings.endpoint` | URL string, https://s3.us-east-1.amazonaws.com new S3 | S3 / Use custom endpoint |
| Object prefix — `.settings.objectPrefix` | string, empty | S3 / Folder in bucket |
| Storage class — `.settings.storageClass` | string, STANDARD | Advanced connection / Storage class |
| Public URL base — `.settings.publicUrlBase` | URL string or null, null | Links / Link domain with example |
| Connect timeout — `.settings.connectTimeoutMs` | integer milliseconds, 5000 | Advanced connection / seconds |
| Request timeout — `.settings.requestTimeoutMs` (S3) | integer milliseconds, 60000 | Advanced connection / seconds |
| Minimum transfer — `.settings.lowSpeedBytesPerSecond` | integer bytes/s, 1 | Advanced connection / Minimum transfer speed |
| Low-speed window — `.settings.lowSpeedTimeSeconds` | integer seconds, 30 | Advanced connection / Low-speed window |
| Object access — `.settings.aclMode` | enum, omit | Links / plain labels for omit and public-read |
| Addressing — `.settings.addressing` | enum, auto | Advanced connection / plain labels |
| Credential storage — `profiles[n].credentials.mode` | enum, keychain; S3 also session | Credentials / Stored in |
| Secret-store reference — `.credentials.reference` | string or null for session; generated opaque reference | Advanced connection / reference |
| Access key ID, Secret, Session token | transient masked/credential inputs; no config keys | Credentials; explicit Store credentials |
| Proton Connect/Refresh | resident setup methods, no secret config values | Proton account / folder refresh |
| Proton account — `.settings.accountRootUid` | opaque string, empty | Account status; ID hidden |
| Proton folder — `.settings.folderUid` | opaque string, empty | Upload folder; ID hidden |
| Proton folder name — `.settings.folderName` | optional display string | Upload folder label |
| Default publication — `.settings.shareByDefault` | boolean, true | After upload / share or private radios |
| Link expiration — `.settings.linkExpirationDays` | integer days, 0 | Advanced destination / expiration |
| Proton deadline — `.settings.requestTimeoutMs` | integer milliseconds, 120000 | Advanced destination / seconds; exact millisecond precision |
| Link password — `.settings.sharePasswordReference` | opaque reference or null, null | Explicit Store/clear link password; input never serialized |
| Launch at login — `preferences.launchAtLogin` | boolean, false | General / Startup |
| Notifications — `preferences.notifications` | boolean, true | General / Notifications |
| Clipboard delivery — `preferences.uploadClipboardBehavior` | enum, each_link; list alternative | General / After uploading |
| Local copies — `archive.keepUploads` | boolean, false | General / Keep a local copy |
| Default folder — `archive.root` | absolute string, Documents/put.gd or home fallback | General / Save to |
| Category folders — `archive.folders.{images,videos,text,other}` | absolute strings or null, null | General / Local copies |
| Naming — `naming.mode` | enum, original | Capture / File names |
| Duplicates — `naming.collisionPolicy` | enum, rename | Capture / When a name exists |
| Imported file template — `naming.fileTemplate` | optional string, {name}.{ext} | Capture / Copied & uploaded files |
| Screenshot template — `naming.captureTemplate` | optional string, Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext} | Capture / Screen captures |
| Clipboard image template — `naming.imageTemplate` | optional string, Image-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext} | Capture / Clipboard images |
| Clipboard text template — `naming.textTemplate` | optional string, Text-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext} | Capture / Clipboard text |
| Concurrent uploads — `limits.concurrentUploads` | integer, 1 | Advanced / Uploads at once |
| Files per batch — `limits.maxFiles` | integer, 100 | Advanced / Files in a batch |
| Pending operations — `limits.pendingOperations` | integer, 8 | Advanced / Waiting operations |
| File size — `limits.maxFileBytes` | integer bytes, 2147483648 | Advanced / Largest file, GiB |
| Batch size — `limits.maxBatchBytes` | integer bytes, 2147483648 | Advanced / Largest batch, GiB |
| Pending artifacts — `limits.pendingArtifactBytes` | integer bytes, 2147483648 | Advanced / Waiting uploads, GiB |
| Queued image memory — `limits.pendingImageBytes` | optional integer bytes, 67108864 | Advanced / Memory for queued images, MiB |
| Clipboard text — `limits.clipboardTextBytes` | integer bytes, 16777216 | Advanced / Clipboard text limit, MiB |
| Image pixels — `limits.imagePixels` | integer pixels, 100000000 | Advanced / Largest image size, megapixels |
| Image bytes — `limits.imageBytes` | integer bytes, 536870912 | Advanced / Largest image, MiB |
| Failed artifacts — `retention.failedArtifactDays` | integer days, 7; zero disables age expiry | Advanced / Keep failed uploads for |
| Recent entries — `retention.recentActivityLimit` | integer, 100 | Advanced / Items in recent list |
| Preview cache — `retention.previewCacheBytes` | integer bytes, 268435456 | Advanced / Thumbnail cache, MiB |
| History retention — `retention.historyRetentionDays` | integer days or null, null | Advanced / Keep history for; Forever for null |
| Desktop support | resident diagnostic read; no config | Advanced / Diagnostics |
| Reload from file | resident settings.reload; no config key | Advanced / Config file |
| Configuration details | resident metadata read; no config key | Advanced / Diagnostics |
| Import development S3 | built-in development template, not implicit setup | Add destination menu, Debug only |

The development S3 template differs from fresh defaults: put.gd bucket, us-east-2, public-read, path addressing, https://put.gd. It is imported only explicitly. The mockup's 8 GiB limit examples do not replace the production defaults of 2 GiB.

## Scope and parity findings

- Tray pane/popover mockup excluded by direct user instruction. Preserve all existing tray paths, progress and notifications.
- Proton safe remote deletion remains unsupported by its real provider contract; share revocation remains available. A visual redesign cannot claim safe deletion that the provider cannot perform.
- History has no single-record removal endpoint. Keep actual clear/history maintenance and explicitly report that brief item rather than alter storage/history implementation.
- Copy image and Save as require an available retained original/local copy. Never substitute a thumbnail for the source or silently fetch a remote URL.
- Proton account disconnection is not part of the current setup endpoint. Preserve browser connect/account inspect/folder creation/password/sharing without pretending a local indicator signs out of the provider.
- “Make default” changes the shared capture default while preserving explicit per-action exceptions. The optional `captureDefaults` object and `destination=default` inheritance are documented below and in configuration.md.

## Verification record

The implementation is built locally against distro Qt 6.4.2 (dynamic), C++20 and the existing official AWS SDK 1.11.907. The initial isolated configure reused the shared checkout's provisioned SDK read-only:

```sh
cmake --preset dev \
  -DCMAKE_PREFIX_PATH=/home/patrick/work/put.gd/build/sdk \
  -DPUTGD_AWS_RUNTIME_DIR=/home/patrick/work/put.gd/build/sdk/lib \
  -DPUTGD_AWS_SOURCE_DIR=/home/patrick/work/put.gd/build/dependencies/aws-sdk-cpp
cmake --build --preset dev
```

Local execution uses private `/tmp/pg40a/r` and `/tmp/pg40a/t` runtime/temp directories. The resolved session socket is 31 UTF-8 bytes; preflight passes the Linux 107-byte limit. Temporary test socket candidates stay below 65 bytes. No VM, live destination or user payload is used.

The complete `ctest --preset dev` run exercised 58 suites. Its first run passed 57, with one failure caused by an incomplete new Proton fixture. That attempt is preserved in `/tmp/pg40a/full-local-1.log`. Later affected-suite runs and fixes are retained rather than overwritten. Final affected-suite runs pass: gui_application (20.24 s), gui_history_view (17.27 s), gui_settings (22.89 s), plus resident split-services/endpoints, native helper lifecycle under Xvfb, settings contract and config reload. All 58 suite names have passing local runs; there was no second single final all-suite run. Final results and preserved attempts are recorded in the local review gallery receipt below.

The renderer exercises actual Qt widgets with production resident dispatch and synthetic ledger records. Twelve views cover History list/gallery, Proton/S3, General/Capture/Shortcuts/Advanced, stored credential hints, and 1060×680 compact layouts. It dispatches no capture or network actions and stores only synthetic session credentials. Those renders prove layout only; they are not native compositor, authentication or cloud acceptance.

Local review gallery and verification receipt: `work/redesign-review/README.md` (ignored, kept in this dedicated checkout). Reproduce renders with `QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR=/tmp/pg40a/r TMPDIR=/tmp/pg40a/t ./build/dev/putgd-redesign-preview OUTPUT_DIRECTORY`, after the socket preflight.

Source review found and resolved queued-path rebasing after destination removal, autosave resumption after setup, transient save retention without replay, optional-default preservation, closing while saves await setup, detailed keyboard help and widget teardown routes. The source review has no open actionable findings. The configured Anthropic transport failed before provider egress (npm ETARGET); the adversarial pass used a local fallback, so no independent cross-model review is claimed.

Full native desktop functional/memory tests, sanitizer acceptance and the declared compositor matrix remain deferred until the user approves the visual candidate. This is an unmerged local experiment.


## Resource and credential behavior

History thumbnails use an 8 MiB cache, one decoder and at most 32 queued paths. Inspector inputs are bounded to 8 MiB encoded / 16 megapixels, scaled to 900×540 and released when hidden. Copy image requires a retained local original no larger than 32 MiB / 16 megapixels. Save as streams the retained original through QSaveFile. Pixel dimensions come from the original header when available, never from the thumbnail. The existing 50-artifact pagination remains; very large captures can span pages.

The resident returns only a four-character access-key ID prefix after explicit credentials have been loaded, plus storage mode and secret/token presence. IDs of four characters or fewer are fully masked. The UI never fills stored secret values into fields, and hints never enter configuration, diagnostics or helper checkpoints. An unloaded keychain entry offers explicit Unlock rather than silently opening the OS keychain. The fixture prefix screenshot uses session-only synthetic credentials; it is not proof of an OS keychain interaction.


## Local preview feedback: 2026-10-15

Fixed the reported “Proton settings have missing or unsupported fields” error when entering an S3 bucket after switching a saved Proton destination. A rejected incomplete provider conversion had left Proton accepted, while later child-field patches were projected onto it. Edits now include the complete selected profile whenever its provider/identity differs from the accepted profile. Validation remains authoritative; a valid replacement is saved atomically and invalid drafts leave the original provider untouched. Bucket feedback now explains empty, malformed and out-of-range names without changing provider validation rules.

Wheel events over the Settings tab bar are ignored; explicit tab clicks and keyboard navigation remain available. The rail now labels Capture region / Capture window / Capture desktop / Upload clipboard (Import clipboard until #239), provides a keyboard-accessible information dialog, and shows per-action tooltips/accessible descriptions based on saved local, private or shared output settings. The footer summarizes mixed routes accurately.

The three targeted regressions were observed failing before the changes and passing afterward. Full affected Settings and helper-lifecycle checks pass. Source review for this increment was a targeted manual review because the checkout already contains the wider, unmerged experiment; no shipping or full desktop/memory acceptance is implied. Feedback logs and updated synthetic renders are preserved in the local review directory.


The initial local preview was configured with `PUTGD_BUNDLE_PROTON=OFF`, so its Connect action failed before it could emit a Proton sign-in URL. The preview now builds with `cmake --preset dev -DPUTGD_BUNDLE_PROTON=ON` using the existing official-source runtime copied from the Proton demo build. CMake verifies its executable hash, protocol, helper source hashes, version lock and notices before bundling it. No account or credential data is copied. The real compiled-runtime input-validation test, synthetic Proton transport suite and local helper-lifecycle suite pass with the bundle enabled; this does not establish live Proton authentication.

Proton connection progress and failures now appear beside Connect, including whether the default browser opened. A synthetic GUI regression checks that Connect dispatches the accepted Proton HTTPS login URL exactly once, displays browser guidance, and shows a failed connection beside the button. The URL handler in that test replaces the external browser; no real login is started by the test.


## Dropdown popup feedback: 2026-10-15

The themed fixed-choice popups clipped their rows, including the three-option Capture → Destination control. The first isolated two-option reproduction needed 52 px of row space but had a 40 px viewport. Changing only the delegate failed and was reverted. A normal list-popup mode probe initially passed, but omitted the production application-wide stylesheet. That incomplete startup context allowed the first fix to pass local tests while remaining broken in the live helper.

With both stylesheet layers loaded, instrumentation showed the actual mismatch: the combo reported list-popup mode (`SH_ComboBox_Popup == 0`), yet retained `QComboMenuDelegate` and two 6 px spacer margins created under the application theme. Replacing the delegate alone left the cached margins and still failed. A full `StyleChange` notification changed the delegate to `QComboBoxDelegate`, reset both spacers to 0, and fitted all three 19 px rows inside a 57 px viewport. Temporary diagnostic code and failed delegate variants were removed.

The final fix keeps `combobox-popup: 0` in the scoped redesign stylesheet and ensures its combo boxes are polished, then sends them a `StyleChange` event after applying that stylesheet. Polishing first is required for newly created controls: a pre-polish notification still sees the original application style. Qt then refreshes its own popup delegate, margins and style settings together. No shared ChoiceBox or tray styling changes were needed. The relevant Qt 6.4.2 behavior is in [stylesheet style hints](https://github.com/qt/qtbase/blob/v6.4.2/src/widgets/styles/qstylesheetstyle.cpp#L5257) and [QComboBox style-change handling](https://github.com/qt/qtbase/blob/v6.4.2/src/widgets/widgets/qcombobox.cpp#L2728).

Popup regressions now load the same application-wide and scoped themes as production. Cases cover 1/2/3/8 choices, editable/fixed controls, first/last selected at opening, complete row visibility, mouse and keyboard selection, plus bounded scrolling and keyboard selection in a 60-option list. An additional test opens the actual three-choice Capture → Destination control in a 1060×680 MainWindow. Previous tests rendered closed controls, and the initial popup tests omitted the application stylesheet; neither was sufficient to establish this behavior.

Failed probes, corrected socket-path accounting, the earlier incomplete-context receipt and the final verification are preserved in the local review directory. These remain local widget checks, not compositor or desktop acceptance. Full desktop/compositor and memory testing is deferred pending visual approval.

Related tracker history: #26 / PR #31 cover closed-dropdown wheel behavior; #37 / PR #39 cover draft preservation. Neither fixes popup geometry. This increment stays under redesign experiment #40.


## Capture defaults, exceptions and save feedback: 2026-10-15

The user selected a persistent default with explicit per-action exceptions. Capture now starts with one Default capture output card. Per-action exceptions stays on the Capture tab, behind an expander; each action has Use default. Unchecking it reveals that action's own output, destination and sharing choices. Existing differing routes open as exceptions. The default is initialized from the existing region route when loading a legacy configuration; simply opening the UI does not rewrite that configuration. The first capture-setting edit adopts inheritance for matching legacy routes while retaining differing routes as explicit exceptions.

The version-1 configuration accepts an optional `captureDefaults` object containing exactly `destination` (`upload`/`local`), `profileId` (existing ID or null), and `publicationMode` (`default`/`share`/`private`). An action can select `destination: default`, with null profileId and no separate publication mode, to inherit those three output values. Its shortcut remains independent. Explicit actions retain the previous format, even when their current value equals the default. `Settings::effectiveAction` resolves this for the production immutable Settings snapshot and UI descriptions; accepted operations retain their captured settings when a later default changes. Legacy documents remain accepted without adding any new fields.

Capture routing is saved as one coupled change through the existing revision-checked resident write path. Other pending fields remain separate, including shortcut edits. Selecting Make default on a destination changes the inherited capture default and preserves exceptions. Removing a destination also clears its default reference safely. Credentials remain explicit Store/Unlock operations; no secrets are part of defaults, configuration, save feedback or UI checkpoints. The UI-state classifier allows only the three known nonsecret default fields.

The lower-left summary describes saved output for future captures, without suggesting an upload is in progress. Change capture output opens Capture settings. A permanent global save status explains that settings save automatically and distinguishes waiting, saving, confirmed success, invalid/unsaved edits, conflicts and failed writes requiring explicit Retry. Success is displayed only after the accepted settings and the draft match; credential input instead explains that Store is required.

Added red-before-implementation core and GUI coverage for inheritance, explicit exceptions across default changes/reopening, checkpoint classification, and pending/invalid/successful/failed save feedback. The existing per-action independence test now checks the effective runtime route, since an inherited local route is encoded as default. Schema tests compare inherited documents with the production CLI validation endpoint and retain legacy compatibility. Rendering uses synthetic fixture data at standard and compact sizes; these are local checks, not desktop/cloud or memory acceptance. The original broad build was interrupted to prioritize the preview and focused regression targets; its log is preserved.


## Independent local copies and destinations: 2026-10-15

The user clarified that General's local-copy setting controls local saving independently of the destination. This supersedes the separate local/upload Output selectors described in the earlier capture-default iteration. Capture now contains only Destination and Sharing, plus per-action destination exceptions. No destination (uploads off) disables remote output for that route; it does not enable local copies. General's checkbox is now Keep a local copy. It applies to captures and clipboard imports, with or without an upload.

| Destination enabled | Keep a local copy | New action |
| --- | --- | --- |
| Yes | Off | Upload only |
| Yes | On | Save locally and upload |
| No | On | Save locally only |
| No | Off | Reject with output_required before source acquisition |

The version-1 storage fields remain compatible: archive.keepUploads retains its existing name and now gates local output for every new action. Existing local routing means remote output is disabled. An unconfigured upload route with no profile also counts as no remote output. A configured destination that later fails to upload still reports its actual upload failure; it is not silently replaced with another destination. Local-save and upload errors remain independent. Previously accepted operation snapshots retain their original behavior.

The UI derives remote routing from the destination selection. It retains legacy stored values until the user edits routing, so opening settings or changing only local copies does not rewrite destinations. Default changes still affect inheriting actions and preserve explicit exceptions. Sharing controls are disabled when uploads are off. Default and exception notices explain both outputs, including the failure state; rail tooltips and the saved output footer include local-copy behavior.

The first-launch local-only check also exposed a misleading unsaved status from the provisional, incomplete connection editor. When all upload routes are explicitly turned off, that editor is now parked outside the saved configuration using the existing classified draft mechanism. Enabling local copies can then reach confirmed All changes saved with no configured profile. Add destination can restore the parked connection draft.

Verification for this increment uses the production operation service with synthetic payloads and a synthetic provider, covering all four output combinations for explicit and inherited routes, plus an unconfigured upload route. The neither-output cases reject before source acquisition or upload dispatch. GUI checks cover default changes, explicit exceptions, independent local-copy changes, save/reopen, first-launch local-only setup, and existing popup behavior. The real resident/shortcut boundary is checked under a private bus and Xvfb for a clear output error with no helper launch or operation history entry. Fifteen synthetic renders include General, default capture, compact capture, expanded exceptions and the neither-output warning; these prove layout only. Full desktop/cloud/memory acceptance remains deferred pending visual approval.


## Save feedback requires a real change: 2026-10-15

The user clarified that an untouched settings window, or focusing and leaving an existing value unchanged, must not claim that changes were saved. The success message is now tied to a changed revision acknowledged from an actual save initiated by that window. Initial display, reopening, reload and helper checkpoint restore do not produce a success message. The static Settings save automatically explanation remains visible. Invalid drafts, pending writes and failed saves still retain their distinct feedback.

Unchanged field proposals are compared against the accepted value plus any pending ancestor proposal before entering the debounce queue. Reverting to the accepted value removes the obsolete field proposal. A final whole-document comparison also prevents a redundant write after queued edits cancel one another. Pending ancestor proposals remain in the comparison so reverting a field cannot accidentally discard the override of an older root edit.

The GUI regression observed both original failures: the initial saved claim, and a no-op editing-finished signal dispatching settings.save. It uses the existing client failure sentinel to prove that no save request is consumed by unchanged input, verifies revert-before-debounce and no duplicate write after a real save, and retains real-save/error/retry and first-launch checks. This is a local UI correction; full desktop and memory acceptance remains deferred.


## Quiet autosave feedback: 2026-10-15

The user requested a single neutral explanation that changes save automatically, with extra feedback only for an actual issue. This supersedes the preceding success-confirmation design. The permanent Settings save automatically text remains. Routine waiting, saving, reload, dirty-draft and success messages are removed from the global indicator, including Some changes are not saved — check your edits. Specific validation messages remain beside affected fields. Write failures offer Retry saving; external conflicts explain that settings changed elsewhere and offer reload. Credential inputs retain a neutral Use Store reminder because their explicit storage behavior is separate from settings autosave.

The no-op proposal/write guards remain unchanged. Existing UI assertions now verify quiet status during editing and successful writes, specific validation feedback, actionable write-failure feedback, and quiet status after retry. No new persistence or checkpoint fields were introduced.


## Proton account identity and folder loading: 2026-10-15

Connected Proton destinations retain their account email across destination changes and UI checkpoint restore. A successful account request that finishes while another destination is selected updates the original destination, including its account root; it cannot overwrite the selected destination. Results from an obsolete settings revision are rejected. The pinned Proton helper now returns the authenticated My Files root owner email with account inspection. The email stays in the classified private UI state and is not written to destination configuration or upload history.

Folder queries show Loading folders… beside the folder controls while preserving the known account identity. A failed refresh clears the loading indicator, reports the actual error, and retains the previous email and folder. Returning to Destinations no longer forces an extra query each time; the explicit Refresh folders action still queries. Initial navigation to a connected destination queries once, and account completion marks that destination's folder list as loaded for the current window.

The runtime was rebuilt with the normal locked provisioner and official SDK commit 8ffae007b0fbb8bcef53514719d6f093bb32dcf9. Its 32 helper tests, compiled version and 11 no-account protocol cases, and compiled large-stdout regression passed. The original CMake rejection of the stale helper manifest is preserved with the corrected provisioning/configuration commands in the ignored local receipt. Runtime hash validation remains enabled. Local navigation, failure recovery, checkpoint, provider-contract and UI lifecycle checks are recorded there; full desktop/cloud/memory acceptance remains deferred pending visual approval.


## Explicit Proton folder refresh: 2026-10-15

Refresh folders is now a separate button beside Upload folder. The dropdown contains folder choices only. The button is unavailable before connection and while a setup request is running; the existing Loading folders… feedback, account email and selected folder are preserved during a refresh. Old private UI checkpoints discard the former refresh-menu entry when restored, retaining the selected folder. No configuration or runtime protocol fields changed. Verification uses the existing GUI settings regression and synthetic regular/compact renders; full acceptance remains deferred.
