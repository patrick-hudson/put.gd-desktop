# Troubleshooting

Start with the production CLI in the same user session as the resident:

```sh
putgd-cli capabilities
putgd-cli settings.get
putgd-cli diagnostics
```

For an uninstalled build, use `./build/dev/putgd-cli` instead. These inspect local
metadata without capture, clipboard-content reads or provider requests. Read
`schemaVersion`, `requestId`, `ok`, `result` and `error` for every response. Exit 0
does not mean an accepted upload finished. Preserve operation IDs and inspect them:

```sh
putgd-cli operation.inspect '{"id":"RETURNED-OPERATION-ID"}'
```

The optional MCP `inspect` result nests separate envelopes under
`result.capabilities`, `result.diagnostics` and `result.configuration`; check each
nested `ok`. See [cli.md](cli.md) and [mcp.md](mcp.md) for exact interfaces.

| Symptom | Production diagnosis | Recovery |
| --- | --- | --- |
| `session_unavailable` | CLI cannot connect to the resident's same-user Unix socket. | Start `putgd` in the actual graphical user session, then inspect again. Use the documented `--socket PATH` only for a known alternate endpoint. Do not delete a live resident's socket or launch another user's process. |
| Transport failure, MCP timeout or missing action reply | Inspect `operation.recent` and retained IDs; a sent action may already exist. | Recover the existing operation before considering another invocation. A client deadline does not cancel or roll back the resident operation. |
| Configuration appears unchanged or reload returns `config_invalid` | `settings.get` returns accepted `result.document`, plus file `result.metadata.path`, `acceptedRevision`, `observedRevision` and `validation`. Outer `ok: true` can coexist with `validation.ok: false`. | Correct the reported JSON path in the complete file, save atomically under the documented cooperative lock, call `settings.reload`, then inspect validation/revisions again. Invalid edits preserve accepted settings. Unsupported schema versions need explicit recovery and stay untouched. |
| `config_conflict` or GUI reports an external edit | Compare the current observed file revision with the patch's `expectedRevision`; GUI dirty drafts can also conflict with accepted external changes. | Reread, rebase the intended change and patch with the new `observedRevision`. Retain dirty GUI values until deliberately reloading/rebasing. Never overwrite merely to clear the error. See [configuration.md](configuration.md). |
| Capture/backend/helper unavailable | Capabilities report each target, shortcut and clipboard import/result type separately. A compiled adapter is not proof of a usable session. | Use the declared native session. On GNOME install/enable the matching helper and check its discovery after a fresh login. On Plasma use current LayerShellQt for region selection; Qt 6.4 builds report it unavailable. Restore missing distro runtime packages. Never substitute another capture target or claim Xwayland as native Wayland. |
| `capture_permission` on Plasma | Check backend diagnostics and the installed desktop entry against the actual resident executable. | Install the correct entry and restart the resident. Keep compositor permission checks enabled. GNOME helper absence requires helper setup, not a screenshot permission bypass. |
| Shortcut does nothing or conflicts | Inspect returned bindings in capabilities; saved settings alone do not prove a grab. | Choose an available shortcut in Settings and inspect the actual result. Leave clipboard sharing unbound until explicitly assigned. Restore helper/session services and verify rebinding; never steal a foreign binding. |
| Clipboard import unavailable/changed or result copy fails | Check import and result MIME capabilities independently, operation acquisition/copy status, and native helper/data-control availability. | Restore the helper/native clipboard service and required consent, then invoke again only if acquisition did not publish. `clipboard_changed` requires a new source invocation. For an uploaded receipt, use `operation.copy` after restoring result copying. Do not focus/paste through a shell as a replacement for native background acceptance. |
| `copy_not_attempted` (`copyStatus=failed`) after a successful upload | A link was available and nothing refused or failed its copy, yet no automatic copy was queued; the `putgd.operation` warning `Automatic copy not attempted` carries the operation, mode, link count and ordering cursor. | Copy the link from Activity. Report it with a diagnostics bundle: it is a defect in the per-link ordering, not a clipboard change (#478). |
| Notification says "Link not copied: the clipboard changed during the upload", Activity says "Link not copied (clipboard changed)" (`copyStatus=suppressed`) | Something took the clipboard, or a newer copy or action came first, between the shortcut and the link. The `putgd.clipboard` record `Automatic copy finished` names which in `suppression.guard`, with the clipboard revisions it compared (see [diagnostics.md](diagnostics.md#records)); a diagnostics package includes such uploads. | Use **Copy link** in Activity, **Copy result** or `operation.copy`. This is deliberate clipboard preservation: put.gd does not overwrite what you copied meanwhile, and it needs no second upload. |
| S3 `AccessDenied` / `s3_permission_denied` | Inspect the item error's redacted HTTP status/service code and effective non-secret profile. A 403 HEAD is inconclusive, not proof that an object is absent. | Check the explicit credential reference and intended destination with its owner. This adapter needs `s3:GetObject` for HEAD and `s3:PutObject` for publication. Without `s3:ListBucket`, a missing key can return 403 instead of 404; selected public-read also needs `s3:PutObjectAcl`. Retain the artifact and retry only after access is corrected. Never disable existence/conditional-write checks or change bucket policy to make a test pass. |
| `s3_acl_incompatible` | `AccessControlListNotSupported` rejects the selected ACL. | Deliberately choose `omit` only if appropriate for the intended destination's existing access model; validate the profile. No automatic ACL change occurs. Successful PUT still does not prove public URL reachability. |
| Uploaded object but public link fails | Inspect the confirmed receipt and public URL configuration; the signing endpoint and custom public base are distinct. | Correct later profile settings deliberately and perform a new isolated synthetic reachability check. Existing receipts retain their original URL/destination. Do not infer missing publication from a failed unauthenticated GET or rewrite remote access controls. |
| `publication_unknown` / interrupted upload | Inspect the persisted item key, publication state and error; a lost reply may follow a successful PUT. | Use `operation.retry` on the same ID to reconcile retained identity. Never rename, create a second upload or clear retained state to evade uncertainty. Completed uploads use copy retry; missing/expired retained artifacts cannot be reconstructed safely from the clipboard. |
| Local archive fails while upload succeeds | Inspect independent local-save and upload states and the effective absolute archive folder. | Restore folder access/free space and use `operation.retryLocalSave`; do not reupload. Existing local copies remain user-owned through failures and cancellation. |
| History deletion rejected, changed, failed or unknown | Inspect `history.detail`; prepare the exact selected IDs with `delete.prepare` and review original destinations/effects. | Confirm only identical ordered IDs and the exact returned acknowledgement for authorized objects. Retain the separate deletion-operation ID and inspect/retry it. Changed objects are not retargeted; lost marker replies stay unknown; `hidden_version_retained` does not mean all bytes were removed. Permission/retention failures may prevent cleanup. Local files/history survive. See [s3.md](s3.md). |
| Clean artifacts reports incomplete uploads it could not remove | The cleanup result's `incompleteUploads` and the item error's `abortError` give S3's refusal of `AbortMultipartUpload` (stage `upload_abort`, request IDs). The parts of that failed multipart upload keep using storage. | Grant `s3:AbortMultipartUpload` (or unlock the destination's credentials) and run **Clean artifacts** again; each cleanup retries until the abort succeeds. A bucket lifecycle rule that aborts incomplete multipart uploads also removes them. |
| Proton `destination_file_too_large_for_plan` | Proton refused the file under the account plan's per-file size limit (service error 200003); the message names the file's size. The drive is not full. | Upload a smaller file, or upgrade the Proton plan, then Retry. When the file's name was still free afterwards, nothing was created and the item is failed, so Retry uploads again; otherwise it is unknown and Retry reconciles first. |
| History/cache clear did not remove a public object | History/cache clearing is local; deletion has a separate prepared effect and operation. | Inspect the retained receipt and use explicit selected-object deletion if authorized. Do not derive a deletion target from a share URL or current profile. Origin deletion also does not invalidate browser/CDN caches. |
| Qt cannot load a platform plugin or history has no SQLite driver | The app may fail before its CLI endpoint exists. Inspect installed distro dependencies and Qt/backend versions rather than assuming a service failure. | Restore matching distro `qt6-qpa-plugins`, `qt6-wayland` and `libqt6sql6-sqlite` packages with normal dependency resolution. Build packages on the target distro. Remove accidental plugin-path overrides; do not bundle a substitute Qt or force offscreen/Xwayland for acceptance. |
| Login startup error | GUI startup diagnostics distinguish `autostart_conflict` from `autostart_io`; the managed entry lives in the user's XDG autostart directory. | Preserve an unmanaged existing entry and resolve its ownership deliberately. Correct directory access for I/O errors, then toggle the preference and prove a real logout/login. Package installation alone does not enable startup. |

These S3 permission distinctions follow the official [HeadObject](https://docs.aws.amazon.com/AmazonS3/latest/API/API_HeadObject.html) and [PutObject](https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html) references.

## VM display failures

A blank guest/session and fragmented VNC updates need different evidence. Inspect
the same display head through the existing runner while keeping VNC visible:

```sh
python3 tools/lab/lab.py screenshot --run work/lab-runs/EXISTING-RUN --head 0
python3 tools/lab/lab.py cli --run work/lab-runs/EXISTING-RUN --method capabilities
```

If the direct QMP framebuffer is also blank or locked, restore the logged-in
native session through its visible console before testing. SSH readiness does
not prove desktop readiness. Do not modify or boot the golden disk directly.

If direct framebuffer witnesses are clean while the VNC wall shows fragmented
incremental updates, retain simultaneous same-head witnesses and viewer/session
identity. This difference was observed on KDE; its exact viewer/codec cause remains
unconfirmed and the investigation is parked at the user's request. It does not
prove a Qt paint defect or authorize a display workaround. Keep the guest's
localhost VNC output active and distinguish display observation from capture/paste
acceptance. See [vm-lab.md](vm-lab.md) and [verification.md](verification.md).

## Redacted support records

Collect request/operation/deletion IDs, structured error codes and safe details,
app/package hashes, distro/Qt/compositor/helper versions, actual backend and
bindings, display geometry/scales, stage timings and configuration validation
status/revisions. Use `diagnostics` for its redacted backend metadata; separately
extract only necessary validation metadata from `settings.get`. Full settings,
history detail and operation results can include destination names, paths, URLs
or filenames, so review and minimize them before sharing.

For an existing lab run, `python3 tools/lab/lab.py collect --run
work/lab-runs/EXISTING-RUN` collects the whitelisted versions and production
sanitized diagnostics. Export curated per-attempt evidence, not the whole VM run
directory. Retain synthetic witness hashes/results and label fixture versus real
service evidence. Do not collect clipboard contents, source pixels/files, secrets,
authorization headers, keyrings, VM passwords/private keys or shell input traces.
Never read, copy, log or commit `credentials.txt` into a support record.

Use the documented [sanitizer setup](thread-sanitizer.md) for native-code failures
and preserve its instrumentation limits; broad suppressions do not resolve a race.
[release.md](release.md) lists performance failures and remaining acceptance gates.
