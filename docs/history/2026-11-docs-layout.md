# Documentation layout change, November 2026

> **Historical record.** This page records one change to where documents live (#1222, 2026-11-05). Use it to resolve an old path found in an evidence record or an earlier commit. The current map is [docs/README.md](../README.md).

Until this change, 53 documents sat flat in `docs/` and acceptance evidence sat in
`docs/evidence/`. Documents moved into folders by subject, and evidence moved to
its own repository. Dated records keep their wording, so they still name the old
paths; the tables below resolve them.

## Moved documents

`docs/build.md` and `docs/proton-drive.md` did not move in this change. `docs/desktops/`,
`docs/readme/` and `docs/history/` kept their paths.

| Old path | New path | Note |
| --- | --- | --- |
| `docs/acceptance-matrix.md` | [`docs/testing/acceptance-matrix.md`](../testing/acceptance-matrix.md) | |
| `docs/agents.md` | [`docs/automation/agents.md`](../automation/agents.md) | |
| `docs/appearance.md` | [`docs/design/appearance.md`](../design/appearance.md) | |
| `docs/architecture.md` | [`docs/design/architecture.md`](../design/architecture.md) | |
| `docs/aurora-vm.md` | [`docs/lab/aurora-vm.md`](../lab/aurora-vm.md) | |
| `docs/b2.md` | [`docs/user/b2.md`](../user/b2.md) | |
| `docs/boat-candidates.md` | [`docs/lab/boat-candidates.md`](../lab/boat-candidates.md) | |
| `docs/boat-snapshots.md` | [`docs/lab/boat-snapshots.md`](../lab/boat-snapshots.md) | |
| `docs/ci.md` | [`docs/release/ci.md`](../release/ci.md) | |
| `docs/cli.md` | [`docs/automation/cli.md`](../automation/cli.md) | |
| `docs/configuration.md` | [`docs/design/configuration.md`](../design/configuration.md) | |
| `docs/credentials.md` | [`docs/design/credentials.md`](../design/credentials.md) | |
| `docs/diagnostics.md` | [`docs/design/diagnostics.md`](../design/diagnostics.md) | |
| `docs/errors.md` | [`docs/user/errors.md`](../user/errors.md) | |
| `docs/evidence-retention.md` | [`docs/testing/evidence-retention.md`](../testing/evidence-retention.md) | |
| `docs/file-dialogs.md` | [`docs/design/file-dialogs.md`](../design/file-dialogs.md) | |
| `docs/file-manager-upload.md` | [`docs/design/file-manager-upload.md`](../design/file-manager-upload.md) | |
| `docs/flatpak.md` | [`docs/history/flatpak.md`](flatpak.md) | |
| `docs/image-editor.md` | [`docs/design/image-editor.md`](../design/image-editor.md) | |
| `docs/image-worker.md` | [`docs/design/image-worker.md`](../design/image-worker.md) | |
| `docs/lab-storage.md` | [`docs/lab/lab-storage.md`](../lab/lab-storage.md) | |
| `docs/mcp.md` | [`docs/automation/mcp.md`](../automation/mcp.md) | |
| `docs/memory-strategy.md` | [`docs/design/memory-strategy.md`](../design/memory-strategy.md) | |
| `docs/neon-vm.md` | [`docs/lab/neon-vm.md`](../lab/neon-vm.md) | |
| `docs/overview.md` | [`docs/design/overview.md`](../design/overview.md) | |
| `docs/package-repositories.md` | [`docs/release/package-repositories.md`](../release/package-repositories.md) | Stub remains at the old path |
| `docs/recording.md` | [`docs/design/recording.md`](../design/recording.md) | |
| `docs/release-runbook.md` | [`docs/release/release-runbook.md`](../release/release-runbook.md) | |
| `docs/release.md` | [`docs/release/release.md`](../release/release.md) | Stub remains at the old path |
| `docs/remote-builders.md` | [`docs/lab/remote-builders.md`](../lab/remote-builders.md) | |
| `docs/remote-golden-images.md` | [`docs/lab/remote-golden-images.md`](../lab/remote-golden-images.md) | |
| `docs/remote-vm-displays.md` | [`docs/lab/remote-vm-displays.md`](../lab/remote-vm-displays.md) | |
| `docs/s3.md` | [`docs/user/s3.md`](../user/s3.md) | |
| `docs/storage-providers.md` | [`docs/design/storage-providers.md`](../design/storage-providers.md) | |
| `docs/surface-driver-prerequisites.md` | [`docs/testing/surface-driver-prerequisites.md`](../testing/surface-driver-prerequisites.md) | |
| `docs/testing-live-s3.md` | [`docs/testing/live-s3.md`](../testing/live-s3.md) | |
| `docs/testing-shortcut-dispatch.md` | [`docs/testing/shortcut-dispatch.md`](../testing/shortcut-dispatch.md) | |
| `docs/testing-tray-progress.md` | [`docs/testing/tray-progress.md`](../testing/tray-progress.md) | |
| `docs/testing-x11-shortcut-refresh.md` | [`docs/testing/x11-shortcut-refresh.md`](../testing/x11-shortcut-refresh.md) | |
| `docs/testing.md` | [`docs/testing/testing.md`](../testing/testing.md) | |
| `docs/thread-sanitizer.md` | [`docs/testing/thread-sanitizer.md`](../testing/thread-sanitizer.md) | |
| `docs/troubleshooting.md` | [`docs/user/troubleshooting.md`](../user/troubleshooting.md) | |
| `docs/ui-catalog.md` | [`docs/design/ui-catalog.md`](../design/ui-catalog.md) | |
| `docs/ui-process-protocol.md` | [`docs/design/ui-process-protocol.md`](../design/ui-process-protocol.md) | |
| `docs/ui-process-state.md` | [`docs/design/ui-process-state.md`](../design/ui-process-state.md) | |
| `docs/updates.md` | [`docs/design/updates.md`](../design/updates.md) | |
| `docs/user-guide.md` | [`docs/user/user-guide.md`](../user/user-guide.md) | |
| `docs/verification.md` | [`docs/testing/verification.md`](../testing/verification.md) | Stub remains at the old path |
| `docs/versioning.md` | [`docs/release/versioning.md`](../release/versioning.md) | |
| `docs/visual-language.md` | [`docs/design/visual-language.md`](../design/visual-language.md) | |
| `docs/vm-lab.md` | [`docs/lab/vm-lab.md`](../lab/vm-lab.md) | |

## Evidence

`docs/evidence/<name>` became `records/<name>` in
[put.gd-evidence](https://github.com/patrick-hudson/put.gd-evidence). Its commit
[`874703b35562c732268c6fe0bc9b42614ffda928`](https://github.com/patrick-hudson/put.gd-evidence/tree/874703b35562c732268c6fe0bc9b42614ffda928/records)
is a byte-for-byte import of put.gd-desktop commit
`6677938767e732247f9097e0ae5f063cb63140ef`: every file has the same Git blob ID at the
same relative path. The earlier commits of this repository still hold the same bytes
at the old paths.

Two directories stayed because CI and tests read them, and
[`docs/evidence/README.md`](../evidence/README.md) stayed as a pointer:

| Old and current path | Read by |
| --- | --- |
| [`docs/evidence/live-s3-memory/source114/`](../evidence/live-s3-memory/source114/) | `tools/ci/contracts.py` `run_evidence` |
| [`docs/evidence/cold-family-metadata-race/`](../evidence/cold-family-metadata-race/) | `tests/performance/test_observe_cold_family.py` |
