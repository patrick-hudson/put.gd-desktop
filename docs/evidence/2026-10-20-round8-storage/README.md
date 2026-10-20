# Round 8 storage follow-ups: pictures and measurements (2026-10-20)

PR #195 (#146 #148 #174) and PR #186 (#147). The source is the #195 branch with
nightly merged (#186, #187); builds are the `dev` preset in a scratch tree on d69
(Debian 13, 4 cores).

## Screenshots

These are offscreen grabs (`QT_QPA_PLATFORM=offscreen`, private XDG dirs) of put.gd's
own main window. They are produced by a scratch variant of
`tests/gui/redesign_preview.cpp` with synthetic history: a Proton item that failed
under the plan's file limit, and a retired S3 upload whose abort a synthetic provider
refuses with the production `upload_abort` guidance. No desktop content appears in
them. Crop and highlight boxes were added afterwards.

- `pr-147-proton-plan-limit.png`: the item and its detail now name the plan's
  per-file limit and the file's size (`destination_file_too_large_for_plan`) instead
  of "a storage or sharing limit".
- `pr-146-cleanup-incomplete-upload.png`: **Clean retry artifacts and previews…**
  reports the multipart upload S3 refused to abort.

## #174 measurement (`pr-174-borrowed-lanes.png`)

The run took a quiet d69 slot at 06:49–06:56Z under the desktop lock, with 1-min
load 0.05 at the start, using synthetic AES-CTR files. Tools:
`tests/performance/s3_upload_probe` (`EXTRA_LANES` 0 vs 3) and
`operations_upload_probe` (production OperationService with 3 uploads at once,
`PUTGD_PROBE_GROUP_GAP_MS`). Servers: `python3 -m moto.server` 5.1.1 on loopback,
and `tests/fixtures/s3_server.py` with a 250 ms delay on every PUT.

| Case | 1 lane | Up to 4 lanes |
| --- | ---: | ---: |
| 1 GiB to moto, round 1 | 18,962 ms, 54.0 MiB/s | 13,199 ms, 77.6 MiB/s |
| 1 GiB to moto, round 2 | 18,757 ms, 54.6 MiB/s | 13,287 ms, 77.1 MiB/s |
| 256 MiB, 250 ms per PUT | 12,177 ms, 21.0 MiB/s | 6,692 ms, 38.3 MiB/s |
| Worker peak RSS | 39.6–39.8 MB | 43.6–43.7 MB |

The small action was a 4 MiB file through the production pipeline at 3 uploads at once:

| Run | Alone | Started 12 s into a borrowing 1 GiB upload |
| --- | ---: | ---: |
| 1 | 399 ms | 549 ms |
| 2 | 489 ms | 330 ms |

Afterwards moto listed 0 incomplete multipart uploads.

## Live S3 (#146)

Run on d69 with key set 2 against `put.gd/acceptance/round8/` (us-east-2):

- **Test connection:** ok, including the create/list/abort multipart probe.
- **`s3_live_probe --abandon-only`:** a 70 MiB upload was interrupted at a third,
  and cleanup aborted its retained upload (`abortedUploads: 1`, journal cleared,
  artifact removed).
- **Read-only check of the prefix afterwards:** 0 objects, 0 versions, 0 delete
  markers, 0 incomplete uploads.

Details are in #145.
