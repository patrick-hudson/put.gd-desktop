# Structured failures and recovery

Every service/CLI failure has `code`, a source-free `message` and bounded `details`.
Ordinary diagnostics contain versions/stages/capability status, never source contents,
filenames, history URLs, authorization values or credential references.

| Code/state | User action |
|---|---|
| `output_required` | Choose an upload destination or enable Keep a local copy in General; neither output is currently enabled |
| `source_unsupported` | Inspect native support and choose an available source |
| `window_unavailable` | This compositor offers no window capture; the message names the missing protocol (on Budgie, labwc 0.20 or later and Mir 2.29 or later have it). Capture a region or the desktop instead |
| `recording_unavailable` | `recording.prepare` can't start here; `details.reason` says why (a later request for a recording that has ended returns the same code with no reason). When it is `wlr_capture_unavailable` (Budgie and Hyprland), the compositor does not offer the Wayland protocol the message names, or the connection to it failed: update the compositor, or restart the session if the connection failed |
| `busy_selecting` | Finish or cancel that interactive selection, or wait for the running capture countdown |
| `repeat_unavailable` | Repeat last region has nothing to repeat: no region was captured since put.gd started. Capture a region first |
| `repeat_layout_changed` | The monitors changed (added, removed, moved, resized, rescaled or rotated) since the last region capture. Capture a region again |
| `session_locked` | The screen was locked or blanked when a capture delay ended (the countdown was no longer on screen), so nothing was captured. Unlock and capture again |
| `delay_unavailable` | This desktop can't show the capture countdown (the message says why), so a delayed capture is refused. Capture without a delay |
| `queue_full`, `input_excessive` | Finish queued work or adjust documented resource limits |
| `disk_space_insufficient` | Free up space in put.gd's data folder and try again (the details give `freeBytes`, `reserveBytes` and what was needed; a recording also gives `shortfallBytes`): an import needs its bytes, and a recording three 64 MiB budgets, above the system reserve. Nothing was accepted |
| `source_unavailable` | Fix the reported item index; invoke again; nothing was published |
| `archive_failed` | Fix the original local folder, then retry local save; upload is independent |
| `artifact_missing`, `artifact_changed` | Restore access or invoke a new action; mutable local copies are not retry sources |
| `operation_settling` | "This operation is still finishing; try Retry again in a moment." The operation has finished but is still recording its outcome (its link copy, for example), and nothing was queued. Retry again once `operation.inspect` for the same operation ID no longer shows `settling: true` (`operation.recent` is a bounded list, and a newer operation can push this one out of it while it still settles); History hides Retry and Finish share link until then. The new Retry faces the usual checks |
| `retry_would_upload` | Finish share link (a retry with `scope` `sharing`) only makes links, and a file of this upload still needs its upload (`details.itemId`). Use Retry upload, which finishes the links too |
| `provider_unavailable` | Restore the adapter/credential access for the recorded destination |
| `unsupported_capability` | Choose a provider/policy that supports the required safeguards |
| `existence_unknown`, `collision_exhausted`, `precondition_changed` | Inspect the saved candidate and retry deliberately |
| `history_io` | put.gd could not read or write its History database (a full disk, an I/O error). Free disk space and try again. If History couldn't save an upload's result, put.gd says once "put.gd couldn't save some History results. Restart put.gd to finish saving them." (when notifications are on). Until the restart, that upload's row may be out of date (a later successful save can still update it), and its Retry answers `history_io` either way. After it, the row shows what put.gd could establish, which may be "outcome unknown", and Retry checks with the destination as usual. If a local save made its file but couldn't record it, the copy may already be in your folder: check there before saving again |
| `unknown` upload / `publication_unknown` | Reconcile the saved publication identity before another PUT. When S3 attempts ran out after a lost reply, the message names the last attempt's error |
| `s3_upload_interrupted` | Every S3 attempt ended (a stall, a dropped connection, or a server error) before S3 had the whole file, so nothing was published. Check the network and Retry; the message names the last attempt's error |
| copy `suppressed` / `failed` | Explicitly copy the existing result; no new PUT is needed |
| `delete_confirmation_changed`, `delete_target_changed` | Review the exact original targets/effects again. When an earlier deletion of the file saved its progress, a new deletion isn't started: choose **Retry deletion** in History |
| `delete_taken_over` | The file was deleted again on its own after this deletion stopped before sending anything for it, and that newer deletion took it over. Retrying this deletion leaves the file alone; its entry in History shows the newer deletion's outcome |
| remote `unsupported_safe_removal` / `version_retained` | The file, or an earlier version of it, may still be at the destination. **Retry deletion**, or choose **Remove from History only…** to stop tracking it; check the file at the destination yourself |
| `destination_removed` | The destination the file was uploaded to was removed from Settings, so put.gd can't delete it there; the file stays where it was uploaded. Choose **Remove from History only**, or add a destination for the same place again (an S3 destination with the same endpoint, bucket and effective addressing (Auto counts as Path, or as Virtual on an amazonaws.com endpoint for a bucket name without dots), a Cloudflare R2 destination with the same account, jurisdiction and bucket, a Backblaze B2 destination with the same API address and bucket, or an SFTP destination with the same Server, User and Folder on server) and delete it through that. Proton Drive and custom uploader files can't move to another destination |
| `destination_readded` | A deletion saved its progress through one destination, which was then removed, and a destination for the same place was added back; or it saved its progress through a destination added back whose credentials were changed afterwards. A request may have reached the file, so put.gd won't finish that deletion through the new destination or credentials. **Retry deletion** checks the recorded file there without changing anything. If the check reports the recorded file missing, the deletion ends as already missing; if what it finds is no longer that recorded file, it ends as changed. Either way History lets the entry go. If the check found the file (`details.found` is `own`), the deletion stays as it was: open **Advanced file details** for its exact name and version, delete that file at the destination yourself and choose **Retry deletion** again. If the check failed (`details.found` is `unknown`, with the reason in `details.checkError`), the deletion stays as it was: choose **Retry deletion** to check again rather than deleting anything by hand. If only the credentials of the destination added back changed, restore the original credentials in Destinations and choose **Retry deletion** to resume the saved deletion. put.gd doesn't check when a version can remain while the file looks gone (the deletion ended `hidden_version_retained` or `version_retained`, or its saved progress names an S3 delete marker), or when no destination points at the recorded place any more: check the file at the destination. Remove from History only is offered only when that deletion had ended `unsupported_safe_removal` or `version_retained`, after a warning that put.gd stops tracking the file |
| `history_record_retained` | Remove from History only kept the record: an earlier deletion may have reached the file, or a share link change hasn't finished (`details.retainedBy` says which). Check the file at the destination, or its link in History, first |
| `destination_not_removed` | The error log entry's destination is in Settings again, so the file can be deleted there: delete it from History instead of removing the record |
| remote `changed` | Replacement is preserved; no destructive request was sent |
| remote `unknown` / `hidden_version_retained` | Explicitly retry the recorded deletion/progress |
| `history_migration`, `history_version` | Preserve the database and restore a supported version; no partial migration is committed |
| `config_invalid`, `config_missing`, `config_too_large` | Fix the original file; accepted settings remain available |
| `config_busy`, `config_conflict`, `config_unwritable` | Reload/rebase or fix filesystem access; prior bytes remain intact |
| `invalid_json`, `invalid_request`, `unsupported_version`, `wrong_session`, `request_too_large` | Correct the versioned session request; no operation was accepted |
| `session_unavailable` | Start the resident app in the same user session |
| `storage_busy` | Quit the other or earlier put.gd process that holds the data folder, then start put.gd again; nothing was moved |
| `storage_migration_failed` | Fix access to the reported folders and start put.gd again; the move resumes from its staging folder and nothing was removed |
| `storage_unavailable` | Make the user configuration and data folders resolvable and writable |
| `sxcu_invalid`, `sxcu_unreadable` | The file is not a ShareX custom uploader, or could not be read; export it again from the service |
| `import_busy` | Another `.sxcu` import is still being saved; import again when it has finished. Nothing of this file was stored |
| `credential_reference_in_use` | The keychain entry this import's ID would use (`putgd/http/<ID>`) already belongs to another destination, or is still being written; choose another ID. Nothing was stored |
| `credential_store_timeout` | The system keychain did not answer an import within two minutes, usually an unanswered prompt; nothing was imported. Unlock the keychain, or import with credentials for this session only |
| `sxcu_unsupported`, `sxcu_unsupported_type` | The message says which part put.gd does not take (a URL shortener, an interactive placeholder, a credential repeated in the link, another method); set the destination up by hand without it ([custom uploader](custom-uploader.md)) |
| `http_settings_invalid`, `http_insecure_url` | Fix the named field of the custom uploader; for a plain `http://` address beyond loopback, turn on **Allow plain HTTP for this destination** or use `https://` |
| `http_text_only`, `http_type_unsupported` | This uploader does not take that kind of file; choose another destination, or change its body type or **Uploads** |
| `http_connect_failed`, `http_upload_interrupted` | Nothing was stored; check the address and the network, then retry |
| `http_authentication_failed` | The server refused the stored values (401/403); enter them again and store them |
| `http_too_large`, `http_rate_limited`, `http_request_refused` | The server refused the upload with a 4xx status, and put.gd records a failure. Its own message follows when the destination has an Error message value |
| `http_server_error` | The server answered 5xx before the request was complete; nothing of it was accepted. put.gd's HTTP client reads no reply while it is still sending, so such a server usually shows as `http_upload_interrupted`. A 5xx after the whole request is `http_outcome_unknown` |
| `http_redirect_refused` | The server redirected to another host, to plain HTTP, or too often; put.gd did not follow. Set the address it named as the request URL if that is right |
| `http_outcome_unknown`, `http_response_unreadable` | The upload was sent and may be on the server; put.gd cannot check. This includes a 5xx that arrived after the whole request was sent and a reply that ended before it was complete (shorter than its `Content-Length`, or a chunked reply without its last chunk). Look on the server, or **Retry upload**, which sends the file again and may leave a second copy. For an unreadable reply, fix the URL value under Response first |
| `http_link_holds_secret` | The server accepted the upload but put a credential of the destination into the link it returned; put.gd neither records nor copies that link, and **Retry upload** would send the file again with the same result. If the named value is not a credential, clear **Secret** on its row (for a value under **Other secret values**, write it back into the address); otherwise the server's reply is at fault ([what counts as a credential](custom-uploader.md#what-counts-as-a-credential)) |
| `http_delete_unsupported` | The upload has no deletion URL put.gd may use: none came back, or the one that did is on another host and would need a stored value, uses plain HTTP the destination does not allow, or points at this computer. History says which; delete it on the server |
| `public_url_unreachable` (custom uploader) | Test connection uploaded the file but could not open the returned link. When the link is plain HTTP and **Allow plain HTTP** is off, put.gd does not open it and says so |
| `http_delete_refused`, `http_delete_unconfirmed` | The server refused the deletion, or answered while the link still serves the file (also when a check of the link was cut short and its bytes match as far as they came); nothing is recorded as deleted. Try again later or delete it on the server |

## SFTP

These come from the [SFTP destination](sftp.md) (#1871). The `ssh_` codes are what put.gd
reads from the startup errors of your own `ssh`; anything it doesn't recognize is
`ssh_failed` with ssh's exit status. No error carries ssh's own output or the server's status
text, which can name hosts, users and paths. Each refusal before a file was created leaves
nothing on the server.

| Code | `details` | User action |
|---|---|---|
| `ssh_not_installed` | none | "put.gd uploads over SFTP with your OpenSSH client, and ssh isn't installed". Install `openssh-client` (Debian, Ubuntu, Mint), `openssh-clients` (Fedora) or `openssh` (Arch) |
| `ssh_config_invalid` | none | `ssh -G` couldn't read your SSH settings for this server. Run `ssh -G <server>` in a terminal to see what it says, and fix `~/.ssh/config` |
| `ssh_host_unknown_key` | `fingerprint`, `host` after a test | put.gd doesn't know the server's host key. Uploads refuse; **Test** shows the key with **Trust key**. Trust it only if it matches the key your server's admin gave you |
| `ssh_trusted_key_changed` | `fingerprint`, `previousFingerprint`, `host` after a test | The key you trusted in put.gd changed; the message gives both fingerprints. Trust the new key only if your server's admin confirms it was replaced |
| `ssh_host_key_changed` | `knownHostsFile` | The key in your own `known_hosts` (the file put.gd found the server's key in) no longer matches. It can mean someone is intercepting the connection. Only after your server's admin confirms the key was rotated, run `ssh-keygen -R '<lookup name>' -f '<known_hosts file>'` with the name and file the error gives, then test again. It names the first file with an entry for the server, which may not be the one ssh objected to, so check the others too; for a system-managed file, ask its admin |
| `ssh_host_key_mismatch` | `fingerprint` | **Trust key** fetched the key again and it isn't the one you were shown, an agent passed another fingerprint, or put.gd's `known_hosts` holds another key for the server than the one this destination trusted. Nothing was trusted; test again and compare |
| `ssh_host_key_known` | none | **Trust key** refused: your own `known_hosts` already has a key for this server, and that one decides. If the server's key changed, and only after its admin confirms that, run `ssh-keygen -R '<lookup name>'` with the name the message gives, then test again. That edits `~/.ssh/known_hosts`; when the entry is in another file (a `UserKnownHostsFile` from your ssh config), add `-f '<that file>'`, and for a system-wide file ask the admin |
| `ssh_known_hosts_off` | none | **Trust key** refused: your SSH settings turn known_hosts files off for this server (`UserKnownHostsFile none`), so put.gd can't keep a key for it |
| `ssh_known_hosts_unreadable` | none | put.gd couldn't check your known_hosts files: ssh-keygen is missing or failed on a file, a file can't be read, or your settings name a file with a `%` token, `${VARIABLE}` or `~user` put.gd can't expand. Nothing connects until it can; fix the file or setting the message names |
| `ssh_host_alias_pattern` | none | **Trust key** refused: the server's `HostKeyAlias` isn't one literal host name (a pattern or a list), or ssh recorded the key under another name. put.gd trusts a key only for one exact host |
| `ssh_auth_failed` | `touched` when a security key was asked for a touch | The server refused the sign-in. The message says why when put.gd knows: a key that needs its passphrase in **Key passphrase**, a server that asks for a password, a chosen **SSH key** the server didn't accept (check User and that the key's public half is in that user's `~/.ssh/authorized_keys`; the message also points to Key passphrase when ssh asked for one), or no agent, key file or password at all |
| `ssh_key_missing`, `ssh_key_unreadable` | `field` (`keyFile`) | The file chosen in **SSH key** isn't there, or isn't a file put.gd can read. Choose the private key again, or leave SSH key empty |
| `ssh_key_public` | `field` (`keyFile`) | **SSH key** names a public key. Choose the private key, usually the same name without `.pub`; picking a `.pub` in Settings takes the private key next to it when there is one |
| `ssh_key_putty` | `field` (`keyFile`) | **SSH key** names a PuTTY key, which OpenSSH can't read. Convert it with `puttygen key.ppk -O private-openssh -o key` and choose the converted file |
| `ssh_key_permissions` | `field` (`keyFile`) for the chosen key | The key is yours and other users have some access to it (any group or other permission bit), so ssh refuses it. Run `chmod 600` on the key file's full path; the message names only the file. Also given when ssh skipped a key from `~/.ssh/config` for the same reason |
| `ssh_needs_one_time_code` | none | The server asks for keyboard-interactive sign-in (a one-time code, or a password typed into its own prompt), or something else put.gd can't answer. Sign in with a key or your SSH agent, or ask the admin to allow password authentication |
| `ssh_host_not_found` | none | ssh couldn't find the server; check its name or the Host alias |
| `ssh_connection_refused` | none | The server refused the connection on its port; check the port and that its SSH service runs |
| `ssh_connection_timeout` | none | The server didn't answer on its port; check the name, port and network |
| `ssh_no_sftp` | none | The server has no SFTP subsystem for this account, or text a shell prints at startup broke the SFTP stream. Ask its admin to enable `sftp`, or print that text only in interactive shells. SCP-only servers aren't supported |
| `ssh_failed` | `exitStatus` | ssh stopped before SFTP started for another reason. The message gives its exit status; run `ssh -v <server>` in a terminal to see why |
| `sftp_settings_invalid` | `field` | Fix the named field: `server`, `user`, `folder`, `publicUrlBase`, `hostKeyFingerprint` or `keyFile` |
| `sftp_folder_missing` | `path` | Folder on server doesn't exist; the path is the one put.gd looked for. Use **Create folder**, which the test offers, then run **Test** again. Uploads never create folders |
| `sftp_not_folder` | `path` | That path is a file; choose another Folder on server |
| `sftp_permission_denied` | `path` | The server refused to create, write or delete there; the user needs write access |
| `sftp_disk_full` | `path` | The server's free-space figure confirms there's no room (it says how much is free). Free space on the server. The message says the partial file was removed only when the server confirmed it, and otherwise names it to check |
| `sftp_failed` | `path`, `status` | The server refused a request; the message names the SFTP status (permission denied, failure, …), never the server's own text |
| `sftp_name_taken` | none | The name was taken when the file was created; your naming rules rename or skip it |
| `sftp_connection_lost` | `path` when an upload had created its file | The connection ended. An upload this cut short is unknown when its partial file couldn't be removed; check the path |
| `sftp_timeout` | `path` when an upload had created its file or its exclusive create got no answer | The server stopped answering: the exclusive create got no answer within 20 seconds, the new file's attributes none within 5, or a request or the upload's writes none for two minutes. An upload this cut short is unknown, so check the path, unless its cleanup confirmed the partial file is gone: then it simply failed. A timeout before the exclusive create was sent fails with no path and nothing to clean up |
| `sftp_protocol` | none | The server sent something that isn't valid SFTP |
| `sftp_reconcile_unknown` | `path` | After an upload whose outcome was lost, a file is at its name but put.gd has no evidence of its own that it is this upload (the size, time and owner its handle reported and its SHA-256, journaled before the file was closed), or it is over 256 MiB, which put.gd doesn't read back. Check it on the server |
| `sftp_server_changed` | none | Checked with only the `known_hosts` lines recorded at the upload, the server no longer holds that key, or the destination now reaches another host, port or user, so put.gd won't check or delete the file there |
| `sftp_path_unsupported` | `path` | The folder's path on the server isn't valid UTF-8, so put.gd can't record it. Nothing was written; choose a folder with a plain name |
| `sftp_delete_unavailable` | none | This upload's record lacks the server identity and file details put.gd checks before deleting it |
| `sftp_known_hosts_failed` | none | put.gd won't use or couldn't update its own `known_hosts` (`ssh/known_hosts` in its configuration folder): the folder or file is a symbolic link, not yours or not private (the file must be a regular 0600 file), it is over 16 MiB, or it couldn't be locked, read or written |
| `sftp_link_mismatch` | `httpStatus` | **Test** uploaded the file, but its link returned other content. Check that Link address serves Folder on server |
| `public_url_unreachable` (SFTP) | `httpStatus` | **Test** uploaded the file, but its link didn't open. Check the Link address and the web server in front of it |
| `destination_test_incomplete` (SFTP) | `path` | The server didn't confirm the test file was deleted, or the name held another file by then (which put.gd left alone); check the path on the server and delete the test file if it is still there |
| `sftp_worker_start` | none | put.gd's SFTP helper didn't start or didn't take its request. Try again; if it keeps failing, reinstall put.gd |
| `sftp_transport_unknown` | `reason` (`quit`, `cancelled` or `deadline`) when put.gd ended the helper, and `path` when it had journaled one | The helper stopped answering, a cancel didn't finish within 45 seconds, put.gd quit (an active request gets up to 30 seconds), or the helper ended without a result. Check the path on the server before retrying |
| `sftp_checkpoint_timeout` | none | The resident didn't save the helper's journal in time, so the request stopped before going on. Try again |

## Scrolling capture

These come from [scrolling capture](../design/scroll-capture.md). A capture that ends with
an image is not a failure: its `endReason` says how it ended and the image is kept.
`end_of_content` (put.gd scrolled to the end of the page) and `done` (you finished it) are
whole as far as the page was scrolled, unless the note in History says otherwise: after put.gd
lost the page and found it again (`foundAgain`), rows may be missing there, so check the image. `height_cap`, `lost_track` (finished while put.gd
had lost the page: the image ends where it last found it) and `top_not_reached` (the way
to the top could not reach the top, so the image starts partway down) are marked
`truncated`, with a warning and a note in History. Losing the page no longer ends a
capture: scroll back a little and it finds its place again.

| Code | `details` | User action |
|---|---|---|
| `scroll_unavailable` | `reason` | Scrolling capture can't start here, or the desktop stopped giving it what it needs mid-capture. `recording_helper_missing`: this installation has no `putgd-record`, reinstall the package. `injection_unavailable`: the desktop gave no way to turn the wheel (X11 without XTEST; GNOME without Mutter's RemoteDesktop service; Plasma 6.7 or earlier without the `putgd-record` desktop entry that lists `org_kde_kwin_fake_input`, restart the resident after installing it; a wlroots compositor without `zwlr_virtual_pointer_v1`); the message says which. `frame_source_unavailable`: the desktop's native frame stream or a GStreamer element is missing, or the stream ended during the capture. `region_unavailable`: this build has no region selector for the desktop (the Ubuntu 24.04 DEB on Plasma Wayland has none; use the AppImage). Scroll capture is unavailable for every caller there: it is refused with it before anything starts, with a rectangle named through `putgd-cli` or MCP as well as without one, and the window's button reports it. On GNOME the same reason, or `helper_outdated`, means the loaded Shell extension is older than version 22: log out and back in once. The reasons `scroll_not_probed`, `scroll_probe_pending` and `scroll_probe_failed` are transient: try again |
| `scroll_active` | `operationId` | A scrolling capture is already running, or a recording, the editor or another capture wants to start during one. Stop or cancel it first |
| `scroll_region` | `rect`, `minimum` | The region spans monitors, lies off the output, or is under 64 px in a direction. Select again inside one monitor |
| `scroll_helper_failed` | `check` | The recording helper died or broke protocol (`exited`, `crashed`, `protocol`, `start_timeout`, `command_timeout`, `build`). Nothing was captured or kept. For `build`, reinstall the package and restart put.gd; otherwise try again and send a [diagnostics package](../design/diagnostics.md) if it repeats |
| `scroll_output_invalid` | none | The resident refused the helper's PNG (wrong size, hash, header or location), or a result whose end reason does not match how it was made. Nothing was admitted. Try again |
| `scroll_state`, `scroll_busy`, `scroll_not_found` | none | A `scroll.*` session call that does not fit the session: the command is not valid in its state (`autoScroll` only while capturing; an auto-scroll toggle, pause or resume while stopping answers `scroll_state` and leaves the capture as it is), another `start`, `select` or `stop` is pending, or a `cancel` is on its way (`cancel` itself is always accepted, but a `stop` whose image was admitted before it arrived has ended the session, so that `cancel` is `scroll_not_found`), or no session has that ID. Inspect the session and send the right command |
| `notification_close_failed` | none | A put.gd notification was on screen and the notification server did not close it after three tries, so the capture was not started (a notice on screen would end up in the image). Dismiss the notification and capture again |
| `capture_timeout` | `check` | `first_frame`: the frame stream sent nothing within 3 seconds. `modifiers_released`: a modifier key stayed down for 10 seconds on X11 when auto-scroll was to begin, so no wheel step was sent. Release the keys and try again |
| `display_changed` | `stage` | The monitors or the size of the captured area changed during the capture. Nothing was kept. Try again |
| `encoding_failed`, `input_excessive` | `stage`, `check` | The spool or the PNG could not be written, or the PNG would pass its size bound. Free disk space in put.gd's data folder, or lower the height cap |

`output_required`, `recording_active`, `editor_active` and `busy_selecting` also mean what
they do elsewhere here. `invalid_request` covers a `scroll.prepare` whose rectangle is
under 64 px or whose `maxHeight` is outside 2,000 to 60,000.

## Text recognition and QR codes

These come from [text recognition](../design/text-recognition.md), which runs in the temporary
`putgd-ocr` helper. Each message is fixed and holds no path or recognized text. `putgd-cli
capabilities` has a `text` block with `available`, the `error` that stops it, `landlockAbi`, the
installed `languages` and the state of each source. It starts no helper. A completed operation that
found nothing is not a failure: it ends as "No text found".

| Code | `details` | Message and user action |
|---|---|---|
| `engine_unavailable` | none | "Text recognition needs the Tesseract English data. Install tesseract-ocr-eng (Debian, Ubuntu, Mint), tesseract-langpack-eng (Fedora) or tesseract-data-eng (Arch)". Install that package. The DEB and RPM require it, so it is missing only after it was removed or when installed without dependencies. The AppImage carries English data and a development build reads the folders in [languages](../design/text-recognition.md#languages-and-where-data-is-read-from). Arch's English data is a larger model, and put.gd loads only the part it needs. A model with no LSTM data is refused as `language_unavailable`. The same code means Tesseract itself failed to start |
| `text_helper_missing` | none | "Text recognition needs the putgd-ocr helper. Reinstall put.gd". `putgd-ocr` is not next to the resident, or is not executable. Reinstall the package or the AppImage. A development build needs `PUTGD_OCR` on and the Tesseract and ZXing-cpp development packages |
| `helper_sandbox_unavailable` | `landlockAbi` in capabilities | "Text recognition is unavailable: this system cannot confine the recognition helper". put.gd refuses to read an image unless the helper can lock itself down first, and it needs the kernel's Landlock at version 3, which is Linux 6.2 or newer with Landlock enabled (`landlockAbi` of 0, 1 or 2 in capabilities means it is not). Update the kernel, or enable Landlock in the kernel's list of security modules (`lsm=` on the boot command line). A container or a security policy that stops a process from restricting itself with seccomp or resource limits gives the same error. Nothing is read and nothing is copied until it is fixed |
| `language_unavailable` | `language` | "Text recognition data for this language is not installed". A language you or a caller named has no data, or its file did not load, or the job named more than three languages or more than 64 MiB of data. Install the language package or copy its `.traineddata` file to `~/.local/share/putgd/tessdata/`, or choose other languages in Settings, Capture, Text recognition. A language chosen in Settings that cannot load is only skipped when another loads, and the notification says so |
| `input_excessive` | none | "The image is too large for text recognition". The image is over 24 megapixels, or a side is over 32,768 pixels, or it is so wide that no band could hold four lines of text. Select a smaller region |
| `text_busy` | none | "Text recognition is still running". Another text operation is running (a selection that is not finished counts). Wait for it, or end it with Escape or `text.cancel`. Nothing queues |
| `text_job_timeout` | none | "Text recognition timed out". The helper did not answer within its 20 second deadline. Select a smaller region. A page with a lot of text normally returns what it read after about 16 seconds instead, marked as cut |
| `text_job_failed` | none | "Text recognition failed". The helper crashed, was stopped, hit a memory or time limit or exited with an error. Try a smaller region, and send a [diagnostics package](../design/diagnostics.md) if it repeats. Nothing was copied |
| `text_job_io` | none | "Text recognition could not read the image or write its result". Try again, with a smaller region if it repeats |
| `text_incompatible` | none | "The text recognition helper does not match this build of put.gd". The installed `putgd-ocr` is from another build or answered out of turn. Let uploads finish, then restart put.gd, and reinstall the package if it stays |
| `text_interrupted` | none | "Text recognition was interrupted; nothing was copied". put.gd quit or crashed while it was reading. It is listed in History as "Text recognition failed" and is never run again. Start Copy text again |
| `edit_stale` | none | "A Redact mark or the crop changed before recognition finished", "A Redact mark or the crop changed while Quick redact was matching", or "That recognition result is no longer current; inspect the editor again". You or a script changed a Redact mark or the crop, so the old result no longer belongs to the image. Quick redact added nothing. Run it again, and read the editor's `text.result` again before a script copies |
| `edit_command_invalid` (`redactText`) | none | "redactText takes categories: 1 to 7 of email, phone, ip, card, key, name and list", "redactText terms are 1 to 200 words or phrases of up to 100 characters on one line", "redactText patterns are 1 to 50 regular expressions of up to 500 characters", "An entry needs a letter or digit", "redactText terms and patterns need the list category", "redactText must be the only command in its batch", or an entry error such as "redactText patterns entry 2: This pattern does not compile: … at offset 5." or "… This pattern matches empty text". Entries count from 0, and the message never repeats the word or pattern. Through MCP, an invalid field or value is refused first as `invalid_request`, and the editor's verdict is final: a pattern that does not compile or matches empty text reaches this error with its index, and so can a term whose character Python and Qt classify differently as a letter. Fix the command and send it again. Nothing was recognized or added |
| `editor_finalizing` | none | "The edit was already submitted or discarded". Quick redact or another editor method arrived after the edit ended, so it added nothing. Open the editor again |
| `sensitive_check_failed` | `cause` | Two cases. With `cause` `file_changed`: "<file>: the file changed after put.gd checked it for sensitive text, so nothing was uploaded". The whole upload stopped before anything was archived or sent. Upload the files again to have them checked again. Otherwise: "put.gd couldn't check this capture for sensitive text, so it wasn't uploaded: …". The check ended without being able to ask, because the image couldn't be staged (over the size limit, or a failed disk write) or the checking helper is missing, crashed or didn't answer within 40 seconds. With Keep a local copy on, the capture is saved locally instead, and History says "Not uploaded: put.gd couldn't check it for sensitive text."; this error isn't shown then. The same happens when put.gd couldn't prepare the covered image after covering text automatically, after **Upload anyway** with covers, or after **Redact and upload** (it couldn't encode it, it was over the output limit, or a disk write failed): the original is never sent instead. A check that ran but couldn't finish is not this error: it asks, with "put.gd couldn't finish checking this capture". If it repeats, send a [diagnostics package](../design/diagnostics.md) |
| `sensitive_check_running` | `operationId` | "The capture is still being checked for sensitive text". `operation.decide` came before the check finished. Ask again |
| `sensitive_decision_unavailable` | `operationId`, or none | "This operation is not waiting for a sensitive text decision" (with `operationId`): it was already decided, timed out, or was never checked. "This operation is not waiting for that sensitive text decision" (no details): the decision's `prompt` named another prompt of a file selection. Inspect the operation and answer its current `sensitive.prompt`. A second decision that reached the panel after the first answers `editor_finalizing` instead |
| `sensitive_choice_unavailable` | none | "Open in editor is not offered for this capture" (`edit`): the capture is a selection of several files, an image over 24 megapixels, or an image that couldn't be read. "Redact and upload is not offered for this capture: nothing was found to cover" (`redact_upload`): no kind set to Warn was found, so `sensitive.found` is empty (a check that couldn't finish, with only automatic covers or nothing). Choose another of the prompt's choices |
| `editor_busy` | none | For a sensitive text decision: "Another decision for this capture is still being answered". In report mode, an agent sent `operation.decide` while its earlier decision was still being answered. Depending on how far the earlier decision got, a repeat can instead get `editor_finalizing`, `sensitive_decision_unavailable` or `sensitive_check_running`. Wait for that reply, inspect the operation, then answer its current prompt |
| `qr_too_long` | `bytes`, `maximumBytes` | "This link is too long for a QR code". A QR code holds at most 2,331 bytes at the error-correction level put.gd uses. Use a shorter link |

`link_unavailable` with a `reason` of `private`, `revoked`, `removed`, `expired` or `none` is
Show QR code's refusal for a link that cannot be offered. An `expired` link that put.gd cannot refresh
is final. `image_unavailable` means History's Copy text has no retained local copy. `source_unsupported`
for a source of text recognition means it holds no PNG, JPEG or clipboard image, and
`source_unavailable` that a file could not be read.

## GNOME 50 setup states

If GNOME 50 setup reports `login_required`, log out and back in once. Any other
state below has its own step. These states report what's missing
(`result.source.helper.setup` in `putgd-cli capabilities`, `desktop.setup`, the MCP
helper). Capture errors use the code in the second column.

| State | Code | What to do |
|---|---|---|
| `login_required` | `gnome_login_required` (`gnome_extension_updated` after an update) | Log out and back in once |
| `user_extensions_disabled` | `gnome_user_extensions_off` | Turn on **Extensions** in the Extensions app; put.gd's extension starts with them |
| `disabled_by_user` | `gnome_extension_disabled` | Turn the put.gd extension on again in the Extensions app |
| `foreign_user_copy` | `gnome_foreign_extension` | A copy put.gd didn't install is in your user extensions folder (by default `~/.local/share/gnome-shell/extensions/putgd@put.gd`); remove it, start put.gd, then log out and back in once |
| `unsupported_shell` | `gnome_shell_unsupported` | This GNOME Shell isn't 50; use a supported session |
| `settings_locked` | `gnome_settings_locked` | Your administrator has locked GNOME's extension list |
| `settings_unavailable` | `gnome_settings_unavailable` | `gsettings` is missing or failed; the message gives the reason |
| `install_failed` | `gnome_extension_install_failed` | A directory under `~/.local/share/gnome-shell/` isn't writable; the message names it. Logging out won't help until that's fixed; then start put.gd again |
| `not_installed` | `gnome_extension_missing` | Reinstall the put.gd package |
| `extension_blocked` | `gnome_extension_blocked` | Per-user extensions are off on this system; use the DEB or RPM |
| `extension_error` | `gnome_extension_error` | GNOME Shell couldn't start the extension; the message gives its reason |
| `system_copy_outdated` | `gnome_system_extension_outdated` | Update the put.gd package to match this AppImage |

Unknown publication/deletion, failed local save and successful remote upload are
independent states. Cancelling a job cannot promise rollback after a network mutation
was sent. Origin deletion does not invalidate browser/CDN caches or remove local files.

The UI also reports `settings_not_saved` when an action cannot proceed with unsaved
settings, and `credentials_not_stored` when a screen capture would close unfinished
credential entry. The window stays open. Resolve the displayed save failure or
Store/clear the credentials, then start the action again. No action is admitted
by either rejection.

## Nextcloud and WebDAV

| Code | User action |
|---|---|
| `webdav_auth_failed` | Nextcloud refused the app password (it may have been revoked): Sign in again. WebDAV: check the user and password and store them again |
| `webdav_quota` | The account is out of space (507). For Nextcloud the message gives the space left or used and the quota. Free some space or ask the administrator for more, then Retry |
| `webdav_folder_missing` | The WebDAV folder doesn't exist; Test offers **Create folder** (`details.createFolder`). For Nextcloud, the folder disappeared during the upload: Retry creates it again |
| `webdav_too_large` | The server refused a file this large (413). Its upload size limit is lower than the file |
| `webdav_forbidden`, `webdav_locked` | The server refused access (403) or the file is locked by another app (423) |
| `webdav_rate_limited` | The server is limiting requests (429). Try again in a few minutes |
| `webdav_server_error`, `webdav_failed`, `webdav_unreachable` | The server had an error, gave another answer, or couldn't be reached; the message names the status or the connection problem |
| `webdav_tls_failed` | The server's certificate isn't trusted, has expired, or is for another host. When a certificate was received, the message gives its SHA-256 and, for an untrusted issuer, the command that adds your own certificate authority to this distribution's trust store |
| `webdav_plain_http_refused` | An `http://` address on another computer needs **Allow plain HTTP for this destination** |
| `public_url_unreachable` (Nextcloud, WebDAV) | Test uploaded the file, but Nextcloud made no share link, or the link didn't open without signing in, opened a web page, or gave other bytes than the test file. WebDAV: set **Link address** to where the folder is served publicly, or allow public reading. Nextcloud: check the server's sharing settings |
| `webdav_settings_invalid`, `webdav_name_invalid` | Fix the field the message names |
| `webdav_cleanup_failed` | put.gd couldn't remove the parts of an unfinished chunked upload; Nextcloud's own cleanup removes them later |
| `credential_setup_required` (Nextcloud, WebDAV) | Not signed in, or the saved credentials are locked: unlock them, or sign in. `details.reason` `binding`: they were given for another server address; sign in (WebDAV: enter them) again. `unbound`: an app password entered by hand hasn't been checked yet; run Test. Without a reason: the app password belongs to another account; sign in again. Or the item was prepared for a folder this destination no longer uses (a folder edit on the same server, for example); signing in doesn't change that, so upload the file again |
| `webdav_transport_unknown` | The helper stopped without a confirmed result; Retry reconciles before sending again |
| `nextcloud_not_found` | The address doesn't answer like a Nextcloud server; use the address you open Nextcloud at in a browser |
| `nextcloud_sharing_disabled` | Public links are turned off on the server. Found before the upload, nothing was uploaded; found by the share step, the file stays without a link and History offers Finish share link. Ask the administrator to allow sharing with public links |
| `nextcloud_share_password_required` | The server enforces a password on public links, which put.gd doesn't set. The upload stays in your files without a link |
| `nextcloud_signin_unavailable` | The server has no browser sign-in (Login Flow v2); use **Use an app password instead** |
| `nextcloud_signin_timeout`, `nextcloud_signin_cancelled` | Sign-in wasn't finished in the browser within 20 minutes, or was cancelled; choose Sign in again |
| `nextcloud_server_mismatch` | Nextcloud named another address for sign-in or finished it for another server (its `overwrite.cli.url` or trusted proxies may be wrong); enter the address it names, or ask the administrator |
