# Design

How put.gd works today: processes, contracts, formats and features. Start with
the [overview](overview.md) and [architecture](architecture.md).

## Guarantees

- One resident owns accepted work. Closing a window does not cancel an accepted upload.
- Acquire → immutable artifact → archive/upload → history → link copy is the shared path for finalized content.
- Shared core, GUI, history and platform contracts are provider-neutral; AWS headers stay under `src/storage/s3/`.
- Preview, editing, Save a copy, cancellation and restart never authorize a recording's upload. Only explicit Upload admits a validated export to `OperationService`.
- Recovery marks interrupted uploads unknown and never automatically publishes. Local-only actions never call a provider.
- Clipboard acquisition never fetches URLs.
- Configuration and journals contain references, never secret inputs.
- Update checks install nothing without the user.
- The idle target is 40 MiB RSS for the resident and any surviving product helpers after work drains.
- The synthetic UI catalog and widget tests establish design and widget behavior only.
- Windows and Flatpak are out of scope.
- Text is recognized only on an explicit action, and nothing recognized is stored. Recognition runs only in the temporary `putgd-ocr` helper, so the resident and `putgd-ui` never link or load Tesseract, Leptonica or ZXing-cpp.

## Documents

- [put.gd today](overview.md): the features, executables, pipeline and where to read next.
- [Architecture](architecture.md): process boundaries and the contracts shared by GUI, hotkeys and agent clients.
- [Resident memory strategy](memory-strategy.md): the idle target and what keeps the resident small.
- [UI process protocol](ui-process-protocol.md): the resident's supervision of `putgd-ui` and its command transport.
- [UI process state](ui-process-state.md): the `putgd-ui` view state contract.
- [Temporary image worker](image-worker.md): the source contract of `putgd-ui --image-job`.
- [Storage provider contract](storage-providers.md): what a `StorageProvider` must do.
- [Screen recording](recording.md): the recording contract, helper, review, export and packages.
- [Scrolling capture](scroll-capture.md): a region scrolled with injected wheel events and stitched into one tall image: the helper session, frames and injectors per desktop, the control loop, proof rules, limits and end reasons.
- [Image editor](image-editor.md): the Capture region & edit workflow, privacy boundaries and agent control.
- [Text recognition and QR codes](text-recognition.md): Copy text from a capture, the editor's text tools and Quick redact, History's Copy text and Show QR code, the confined `putgd-ocr` helper, untrusted content, languages, the clipboard rules and the agent contract.
- [Capture monitor, delay and repeat](capture-delay.md): the monitor under the pointer, the capture countdown and Repeat last region.
- [File dialogs](file-dialogs.md): open, save and folder dialogs through `putgd::FileDialog`.
- [File manager upload](file-manager-upload.md): **Upload with put.gd** from a file manager.
- [Updates](updates.md): the daily check, feed contract and per-format update paths.
- [Logs and diagnostics](diagnostics.md): persistent logs, retention, unclean exits and diagnostics packages.
- [Configuration](configuration.md): `config.json`, its folders and its fields.
- [Credentials](credentials.md): credential references, secret storage and unlock.
- [Appearance and themes](appearance.md): System, Dark and Light, theme tokens and tray tint.
- [Clipboard after quitting](clipboard-keeper.md): the helper that holds a copied result on Budgie and Hyprland.
- [Visual language](visual-language.md): the desktop visual language and its shared implementation.
- [UI catalog](ui-catalog.md): `putgd-ui-catalog` and the complete render CI publishes.
