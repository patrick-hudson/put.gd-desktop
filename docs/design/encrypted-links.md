# Encrypted links

An encrypted link is an ordinary put.gd upload that only someone holding the link can open
(#2079). put.gd encrypts the file on the user's computer and wraps it in a small
self-decrypting web page, and that page goes to the destination the user already uses. The
key travels only in the link's fragment, the part after `#`, which browsers never send to a
server. The bucket, the CDN, its access logs and anyone who can list the bucket see only
ciphertext under a random name. There is no put.gd server and no account.

```
https://files.example.com/k3Xq9vTa2mPw.html#v1.Rk2c8w…43 chars…
└── your bucket serves this page ──────────┘└─ key: never leaves the browser
```

Encryption is off by default. A destination turns it on in its **Encrypted links** setting, and one
upload can turn it on or off for itself. How it reads to users is in the
[user guide](../user/user-guide.md#encrypted-links). Its failure codes are in
[failure codes](../user/errors.md#encrypted-links), and its session methods, `putgd-cli` commands
and MCP tools are in [the CLI](../automation/cli.md#encrypted-links) and
[MCP](../automation/mcp.md#tools-and-effects) documents.

## What it protects against, and what it doesn't

The user guide gives the same table in plain words:

| Threat | Protected? |
| --- | --- |
| Someone who can read the bucket: a public bucket, a leaked read key, the cloud provider, a backup | **Yes.** They see ciphertext and a random object name. |
| CDN and server access logs, proxies, link scanners that fetch the URL without the `#` part | **Yes** |
| Preview bots that only fetch the link | **Yes.** They don't send the `#` part to the destination, and usually show no preview. |
| The chat or email service the link is sent through | **No.** It receives the full link, key included. When that service is outside your trust boundary, set a passphrase and send it another way. |
| Anyone the full link is forwarded to | **No**, unless a passphrase is set (see below) |
| The recipient's browser history, synced history, and extensions that can read tabs or history | **No.** The key stays in the address bar like any bearer link. |
| Someone on the network path of a plain `http` link | **No.** An active attacker can serve another page in its place, one that reads the fragment. The protection is that the Test check refuses a link that isn't https (loopback excepted), so encrypted uploads never get a plain `http` link. |
| Someone who can *change* files in the bucket and swaps the page for one that steals the key | **No** for the page on its own. Stated as a known limit. `putgd-cli open` verifies the page before decrypting. |
| Recipient's device or browser compromised | No |

Other processes running as the same user are trusted: file handling for encryption guards against
files put.gd didn't create, not against such a process racing it in put.gd's private folders.

A chat service is not protected in general. A link-preview bot that only fetches the URL never
sends the fragment, so the destination's logs and the bot never see the key, and the chat
usually shows no preview. But the messaging service stores and delivers the message itself,
so it holds the full bearer link. When the chat provider is outside the user's trust boundary,
the link alone is not enough protection: use **On with a passphrase** and send the passphrase
another way.

The fragment also stays in the recipient's address bar, so it is in the browser's history and
any history sync, and an extension with access to tabs or history can read it. That is the
same exposure as any bearer link, such as a presigned URL.

There is no "burn after reading" and no view count, because a static bucket can't enforce
either. Expiry is what the destination already offers: its lifecycle rules, an expiring
link's own lifetime, and put.gd's **Delete from destination**.

The bucket-writer limit is deliberate. A browser runs whatever page the bucket serves, so
someone who can write to the bucket can replace the page with one that reads the fragment and
sends it away. A viewer hosted on app.put.gd and pinned by hash would close that gap, but it
would be a put.gd-hosted page, so it stays out (decision 3 on the issue). `putgd-cli open`
closes it for anyone who opens links with it: it refuses any page that isn't put.gd's pinned
viewer byte for byte before it decrypts anything.

## The link and its key

- The link key `K` is 32 random bytes from OpenSSL's generator, new for every file.
- The link is the destination's link for the page, then `#v1.` and `K` in unpadded base64url
  (43 characters). A fragment that isn't exactly that form opens nothing: the viewer and
  `putgd-cli open` accept only the canonical encoding.
- The optional passphrase is a second factor, never a replacement for the key. Someone who has
  only the link, or only the passphrase, can't open the file.

## Format version 1

The source of truth is the header comment in `src/encrypt/EncryptCrypto.h`. An encrypted container is
a 96-byte header followed by AES-256-GCM chunks in the STREAM construction. OpenSSL does every
primitive on put.gd's side and WebCrypto in the browser. Nothing is hand-written crypto.

The feature was first called sealed links, and five version 1 format identifiers keep that
name: the magic `PGDSEAL1`, the HKDF labels "put.gd sealed v1 check" and "put.gd sealed v1
payload", the page marker `<!--putgd-sealed-->` and the data element's id `putgd-sealed`. They
are part of the format, so changing them would break the test vectors and every link already
shared.

### Header

| Bytes | Field |
| --- | --- |
| 0–7 | Magic `PGDSEAL1` (ASCII) |
| 8 | Version, `1` |
| 9 | Flags: bit 0 set when a passphrase is needed; every other bit 0 |
| 10–11 | Zero |
| 12–15 | Chunk size, u32 big-endian: 1,048,576 (1 MiB) |
| 16–19 | PBKDF2 iterations, u32 big-endian: 600,000 with a passphrase, 0 without |
| 20–51 | Salt, 32 random bytes |
| 52–58 | Nonce prefix, 7 random bytes |
| 59–63 | Zero |
| 64–95 | Key check, 32 bytes |

The viewer and `putgd-cli open` refuse a header with any other magic, version, flag, chunk
size or nonzero padding, and any iteration count but 600,000 when a passphrase is set (0
otherwise). Version 1 fixes the cost, so put.gd always encrypts with 600,000 and both sides open
nothing else.

### Keys

```
key check  = HMAC-SHA-256(HKDF-SHA-256(K, salt, "put.gd sealed v1 check", 32), header[0..64))
ikm        = K                                         without a passphrase
           = K || PBKDF2-HMAC-SHA-256(NFC(passphrase) as UTF-8, salt, iterations, 32)
file key   = HKDF-SHA-256(ikm, salt, "put.gd sealed v1 payload", 32)
```

The key check proves the link key matches this header before any passphrase is asked for, so
a damaged or wrong link says so at once. It says nothing about the passphrase. A wrong
passphrase shows only when the first chunk fails to open, and the viewer then says "That
passphrase doesn't open this link." PBKDF2-SHA-256 at 600,000 iterations is native WebCrypto
and needs no WASM (decision 2 on the issue). It slows guessing, and the random 256-bit link
key stays the main protection. A passphrase is one line of at most 1 KB (1,024 bytes) of
UTF-8 after NFC normalization, on both sides. Encrypting, opening, Settings, the session methods
and `putgd-cli` all refuse a passphrase with a line break or a longer one.

### Chunks

```
chunk i  = AES-256-GCM(file key,
                       nonce = nonce prefix || u32 BE i || u8 last,
                       aad   = header (all 96 bytes) || u32 BE i || u8 last) || tag (16 bytes)
```

`last` is 1 for the final chunk and 0 otherwise. Every chunk but the last holds exactly
1 MiB of plaintext; the last holds 1 byte to 1 MiB. Because each chunk's nonce and
additional data carry its index and the last flag, a container that is reordered, cut short,
extended or changed anywhere fails to open. It never gives part of a file.

### What the plaintext holds

The plaintext stream is a u32 big-endian length, a metadata JSON of at most 4,096 bytes, then
the file's bytes. The metadata is compact UTF-8 with exactly the keys `name`, `size` and
`type`, in that order. The original file name, size and type are therefore encrypted too.
The object at the destination has a random name and says nothing about the file.

## The viewer page

`src/encrypt/viewer.html` is the one viewer. It is about 12 KB, with one inline script and one
inline style and nothing loaded from anywhere else. Its exact bytes are compiled into put.gd
(`cmake/Encrypt.cmake`) and pinned by SHA-256 in `viewerSha256` (`src/encrypt/EncryptPage.cpp`):

```
3f271e733ddbb3ab7921d52a7dc8256a8eb6fdbb7a61409371a4961ef6f3d042
```

`tools/encrypt/pin_viewer.py` rewrites the two CSP hashes and `viewerSha256` after an edit and
prints the new hash for this document; `--check` changes nothing and fails when a hash is out
of date. The unit test `encrypt_format_contract` checks the compiled-in viewer against the pin,
and the encryption helper refuses to encrypt with a viewer that doesn't match
(`encrypt_viewer_changed`). A change to the viewer is therefore always a deliberate, reviewed
change to this hash. The CSP allows no fetch except, in the two-file form, the page's own
`./NAME.bin` on its origin; `pin_viewer.py` also refuses a viewer that names
an outside address a later edit could navigate to, written plainly (`https://`), protocol-relative
(`//host`) or hidden in a character reference or escape.

An encrypted page is the viewer with its marker `<!--putgd-sealed-->` replaced by one data
element, and nothing else changed:

| Form | Data element |
| --- | --- |
| One page | `<script id="putgd-sealed" type="application/octet-stream" data-form="inline">` + base64 of the container + `</script>` |
| Page and data file | `<script id="putgd-sealed" type="application/octet-stream" data-form="split" data-bin="NAME.bin"></script>`, and the CSP's `connect-src 'none'` becomes `connect-src 'self'` |

`NAME` is 1 to 128 characters of `A–Z`, `a–z`, `0–9`, `_` and `-`.

### Content Security Policy

The page carries its policy in a `<meta>` tag, so it holds on any server:

```
default-src 'none'; script-src 'sha256-…'; style-src 'sha256-…'; img-src blob:;
media-src blob:; connect-src 'none'; base-uri 'none'; form-action 'none'
```

Scripts and styles run by hash only. Images and media come only from `blob:` URLs the page
made itself. Only the two-file form gets `connect-src 'self'`, for its one fetch of
`./NAME.bin`, which omits credentials, refuses redirects and skips the cache. The page also
sets `referrer` to `no-referrer` and `robots` to `noindex, nofollow`.

### After decrypting

- **Images** (PNG, JPEG, GIF, WebP, AVIF, BMP and SVG) show in an `<img>`; a click switches
  between fit and actual size. SVG shows only as an image, so its scripts never run.
- **Video** (MP4, WebM, Ogg, QuickTime) and **audio** (MP3, Ogg, WAV, WebM, MP4, FLAC) play in
  the browser's player.
- **Plain text** up to 4 MB shows as text.
- **Everything else**, HTML included, offers only **Download**. A file that isn't shown is
  handed to the browser as `application/octet-stream`, so HTML never renders as a page.
  **Download** is offered for every type.
- **Too large for the browser.** Before showing anything, the viewer reads back the first, middle
  and last byte of the file it put together, before the player or **Download** gets it. A
  browser can refuse to read back a large file a page builds: Chrome in a private window does
  for a 1 GB file, and headless Chrome from about 512 MB. And a desktop Chrome player handed a 1 GB file without that read stayed blank, but
  played it once the viewer had read it. When the read fails, the viewer
  says "This file is too large for this browser to show. Select Save to file to save it." and
  offers **Save to file**, which writes the decrypted file straight into the file the person
  picks in the browser's save dialog (`showSaveFilePicker`). A browser without that dialog says
  "This file is too large for this browser. Open the link in Firefox, or use putgd-cli open."
  The viewer never writes the decrypted file into the page's own storage (the origin private
  file system or IndexedDB): many links share one origin, such as path-style S3 and B2's
  `/file/` links, so any other page there could read it.

A one-page link works offline once loaded; a two-file link needs its `./NAME.bin` fetch
first. The viewer works in phone browsers through the QR code: WebCrypto works in current iOS
and Android browsers. Browsers offer WebCrypto only on secure pages
(https, or this computer), so on a plain `http` page the viewer decrypts nothing and says
"This browser decrypts only on secure pages. Ask for an https link." That refusal is not a
protection against a network attacker, who can serve another page in its place. It sends no
request with the key and keeps the key only in the address bar, which also puts it in the browser's
history.

The viewer also refuses a key that isn't the canonical base64url form, a container larger
than a 64 MB file needs in the one-page form or a 2 GB file needs as a data file, and metadata
whose `size` isn't a whole number from 0 to 2 GB.

## Forms and limits

| File size | What uploads |
| --- | --- |
| Up to 64 MB (67,108,864 bytes) | One self-contained `.html` page. Works on every destination that serves HTML as a page. |
| Over 64 MB, up to 2 GB (2,147,483,648 bytes) | The page plus `NAME.bin` beside it. The page fetches `./NAME.bin` from its own origin, so no CORS setup is needed. This needs a destination with a public Link address; per-object presigned or authorized links can't serve it. |
| Over 2 GB, or over 64 MB where the destination can't serve two files | Nothing uploads. The item fails before anything is sent, with `encrypt_too_large` or `encrypt_split_unavailable`. Turn off **Encrypt** to upload it as an ordinary link. |

The one-page form holds base64, so its page is about a third larger than the file. The page
gets a random name of 12 letters and digits (`EncryptJob::randomName`), never the capture's own
name, and its data file shares that stem: `k3Xq9vTa2mPw.html` and `k3Xq9vTa2mPw.bin`.

## Process boundaries

The resident never maps libcrypto:

- `putgd_encrypt_page` (Qt Core only) builds and checks pages. The resident links it.
- `putgd_encrypt` (OpenSSL) holds the crypto. Only `putgd-cli` and tests link it, and
  `tools/ci/check_layers.py` keeps it that way.

Each item is encrypted by a short-lived `putgd-cli --encrypt-job` child (`src/core/EncryptJob.cpp`,
`src/encrypt/EncryptJobMain.cpp`). The resident writes one JSON request on its standard input: the
file's private path, size, name and type, the form, the output paths and, when there is one,
the passphrase. Before it starts the child, the resident creates each output in the
operation's private folder itself: an empty file with mode 0600, opened with
`O_CREAT|O_EXCL|O_NOFOLLOW`, whose descriptor it keeps. When it can't create one, the item
fails with `encrypt_failed` ("Couldn't create the encrypted file's working copy.") and no child
starts. The child runs with core dumps off and a lower priority, reads exactly the stated
size, and only writes: it opens each output path without creating it and refuses anything
that isn't an empty, owner-only regular file of this user, never through a link ("Couldn't
open the file for the encrypted page.", "Couldn't open the file for the encrypted data file."). It
syncs what it wrote and never creates or deletes a file. It answers on standard output with
the key and the size and SHA-256 of each file. The resident hashes the files itself through
the descriptors it kept, so it checks the files it created, and refuses them when they
differ, then records them as the item's artifacts. After a refusal, a crash, a timeout, a
failed start, a cancel or a mismatch, the resident deletes an output path only while its
device and inode still match the file it created, so it never deletes a file that took the
path. A cancel that arrives as the child finishes ends `cancelled` before the output is
checked. Encrypting a file has a 30-minute deadline and stops on
cancel. The passphrase reaches the child only on standard input, and the key comes back only
on standard output; neither is ever on a command line.

`putgd-cli open` and `putgd-cli --encrypt-probe` (the Test check below) run without the resident
and also live in the CLI.

## The pipeline

Encryption sits in the shared pipeline between the immutable artifact and naming, in
`OperationService::encryptItems`.

1. **Whether to encrypt.** An upload is encrypted when it asked to be (`encrypt` on the request, kept as
   the operation's `encrypt.requested`), else when its destination's `encrypt.mode` is `on` or
   `passphrase` in the settings the upload was accepted with. The **Encrypt** toggle beside
   Upload in the editor (also the editor the sensitive text prompt opens) and in recording
   review sets the operation's own choice. `operation.encrypt` works that window's own toggle
   (`editor.encrypt` and the review's `encrypt` action are the same toggle), and it is accepted
   only while that window is open: the editor while it is editing, or the recording in
   review. Before a sensitive text check's editor opens it is refused with
   `encrypt_unavailable`, while the recording runs or exports with `recording_state`, once the
   edit was submitted or the upload started (`enqueue` closed the choice) with
   `encrypt_too_late`, and once the operation finished, or for an ID no operation has, with
   `operation_not_found`. The toggle's rules hold in both windows. On is offered only where
   the destination can encrypt (`available`); Off always is. Asking for the state the toggle
   already has succeeds and changes nothing, even while the toggle is disabled. Turning it on
   where encryption is unavailable answers `encrypt_unavailable` with the reason before any other
   refusal, an export in review included. After a failed upload preparation the editor gives
   back its controls with the same rule, so Encrypt stays disabled where it can't be turned on.
   Recording review shows the toggle whenever the capture's route is an upload with a
   destination (`uploadReviewOptions`), whatever that destination is called, and disables it
   while an export runs. `recording.upload`'s own `encrypt` decides that upload alone and
   never moves the toggle; without it the upload takes the toggle's choice as the window shows
   it. `RecordingService::exportTo` fixes that choice when the export starts and keeps it in
   the export request, so Try again (`retryExport`) uses the same choice, and a toggle change
   during the export applies to the next upload. An upload whose fixed choice is to encrypt,
   whether from `encrypt: true`, the toggle or a Try again, is refused before anything is
   exported: with `encrypt_unavailable` (the toggle's reason, then "Upload it without
   encryption."; "Encrypted links aren't available for this upload." when there is no reason),
   or with `encrypt_passphrase_locked` ("The Encrypted links passphrase isn't loaded. Unlock it
   in Destinations, then upload again.") when the destination asks for a saved passphrase that
   isn't loaded. Since nothing was exported, the review window offers Upload again for any
   `encrypt_*` refusal, not Try again.
   To encrypt a capture from the start, pass `encrypt` with `action.invoke` (region, window,
   monitor, desktop, repeat, clipboard and edit; `scroll`, `record` and `text` refuse it with
   `invalid_request`) or `upload.files`. The choice is written into the operation's `encrypt.requested` before its first artifact is stored, so a
   restart keeps it. Diagnostics package uploads are never encrypted.
2. **Refusals.** When encryption was asked for and can't happen, the file is never uploaded
   unencrypted. The item fails before anything is sent, with one of:
   - `encrypt_unavailable`: the destination type can't serve an encrypted page (its provider's
     `encryptUnavailable`, such as Nextcloud or Proton Drive).
   - `encrypt_needs_link`: the capture's route publishes privately (`publicationMode`
     `private`, or `default` where the provider's default is private), so the upload gets no
     link and nobody could open the page. It is checked before the Test check and before
     anything is encrypted. It is a guard for a destination that offers both private uploads
     and encrypted links, and no current destination does. Only Proton Drive offers private
     uploads (`privateUploads`), and it refuses Encrypt first with `encrypt_unavailable`. Every
     other destination refuses Private upload earlier, with `unsupported_capability`.
   - `encrypt_check_required`: the destination hasn't passed the encrypted-link Test check for the
     settings this upload was accepted with, or failed it (the message gives the check's
     reason).
   - `encrypt_passphrase_locked`: the destination asks for a passphrase and it isn't loaded and
     the request didn't carry one. Uploads never unlock the keychain.
   - `encrypt_passphrase_missing`: a Retry that must encrypt an item not encrypted yet, in an upload
     whose passphrase came only with its request (`upload.files`'s `encrypt.passphrase`). That
     passphrase is held in memory for the request and never kept, so the file must be
     uploaded again with it. An item that was already encrypted is sent again as it is and
     needs no passphrase.
   - `encrypt_too_large`: the file is over 2 GB, the largest file the format encrypts.
   - `encrypt_split_unavailable`: the file is over 64 MB, so it needs the page and a data file
     beside it, and the destination's links can't serve the two side by side (S3 and R2 with
     expiring links, B2 with authorized links, custom uploaders). An SFTP destination without
     a Link address can't encrypt any file, so it answers `encrypt_unavailable`.
   - `encrypt_failed` (or `encrypt_viewer_changed`): the encryption helper is missing, failed, timed out
     or wrote files that didn't match its answer, or the resident couldn't create the job's
     output files ([process boundaries](#process-boundaries)). Only files the resident created
     are ever removed again.
   - `cancelled`: **Cancel** while the helper runs or while its output is checked. `EncryptJob`
     answers `cancelled`, not a mismatch, and the item ends `cancelled` with "Upload cancelled
     before it started. Select Retry upload to try again."

   The order is `encrypt_unavailable`, `encrypt_needs_link`, `encrypt_check_required`,
   `encrypt_passphrase_missing`, `encrypt_passphrase_locked`, then the size refusals. The editor's
   and review's **Encrypt** toggle (`OperationService::encryptOptions`) is offered as On only when
   none of the first three applies, and its tooltip gives the first that does. `operation.encrypt`
   failures are written to the error log under the `upload` kind.

   An encrypted request reaches its destination only once its file is encrypted. It skips the
   warm-up connection that other uploads open when the action starts
   (`StorageProvider::anticipate`), so a file put.gd refuses to encrypt contacts no destination at all.
   With the sensitive text check on, **Encrypt** can still be turned on in the editor the prompt
   opens, so the warm-up of every upload waits until the upload starts (`enqueue`) and its
   Encrypt choice is final.
3. **No exceptions.** `OperationService::sendable` sends an item of an upload that is to be
   encrypted only once it is encrypted. A Retry keeps the upload encrypted, so a file put.gd can't
   encrypt fails the same way again. To send it as an ordinary link, the user turns off **Encrypt**
   and uploads it again. Batch naming skips a refused item, so it keeps its failure and the
   upload never ends `completed` with it.
4. **Encryption.** The item's phase is `encrypting` while the helper runs. The item keeps its own
   file as its artifact, for previews and local copies; what uploads under the item's
   candidate name is the encrypted page (`OperationItem::uploadArtifact`).
5. **The data file first.** In the two-file form the data file uploads before the page, under
   its own random name with a name check and a conditional create, journaled in
   `encrypt.data`. Its name is written inside the page, so it is never renamed: a taken name fails
   the item with `encrypt_name_taken`. An unknown data-file outcome is reconciled before it is
   ever sent again, as an item's own upload is. A data file still `uploading` when put.gd
   stopped becomes `unknown` at the next start, and its multipart upload is cleaned up with
   the item's own. When the destination refuses to abort the data file's multipart upload,
   it is kept and reported like any kept upload: once, in the `incompleteUploadNotice` with
   its size, and in `cache.clear`'s `incompleteUploads` with `reported`. The data file keeps
   its own once-only mark (`incompleteUploadReported` in `encrypt.data`), apart from the page's.
6. **Retry** sends the same page and data file again, so a link already shared keeps working.
   An item a Retry sends that was never encrypted (it failed or was cancelled first) is encrypted
   then. While Forget key writes the operation's record, `operation.retry`,
   `operation.retryLocalSave` and `operation.copy` answer `operation_busy`, so none of them
   can write the key back.
7. **The link** is the destination's link for the page, then `#` and the key
   (`OperationItem::link`). The clipboard, the link-ready notification, History's **Copy
   link**, **Open in browser** and **Show QR code** all use it.

When every file of an upload is encrypted and its link is copied, the notification's title is
`Encrypted link copied` (`Encrypted links copied` for several) and its body is `Encrypted link copied
to clipboard.` (`Encrypted links copied to clipboard.` for several), followed by `Send the passphrase separately.` when a passphrase is set.

An item's `encrypt` record:

| Field | Meaning |
| --- | --- |
| `status` | `encrypted`. An item that couldn't be encrypted fails with its error and has no `encrypt` record |
| `form` | `inline` (one page) or `split` (page and data file) |
| `key` | The fragment `v1.…`; empty with `keyForgotten: true` after Forget key |
| `passphrase` | `true` when the link also needs a passphrase. The passphrase itself is never kept here. |
| `page` | The encrypted page's private artifact |
| `data` | Two-file form only: the data file's artifact, candidate, upload status, receipt, name-check reference, condition, publication ID, journal and error |

## History

- History records carry `link` (the link to copy, with the key after `#` while it is
  available) and `encrypted`. Private paths and the data file's upload journal are left out of
  `history.detail` and `operation.inspect`.
- Encrypted items show a lock glyph in both the list and the gallery. **Copy link**, **Copy
  links**, **Open in browser** and **Show QR code** include the key.
- **Forget key** (`history.forgetKey`) removes the key from the History record and from its
  operation's journal in one transaction (`HistoryStore::forgetEncryptKey`), so put.gd can't open
  the file or show its link again. Copy link, Open in browser and Show QR code stop being
  offered. The file stays at the destination, and anyone who already has the full link can
  still open it until the file is deleted there. `history_not_encrypted` answers for an item that
  isn't an encrypted link, and `history_busy` while its upload runs, a Retry included, or another
  History change of it is being written (`OperationService::holdOperation`): "This upload is
  running, or History is already changing it. Wait a moment, then select Forget key again."
- History's row menu offers **Forget key…** for a single-file row. For a row with several
  files it offers **Forget key for NAME…**, for the file chosen in the details pane (the
  first until another is chosen), once that file is encrypted and its key isn't forgotten. An
  encrypted row says **Encrypted** or "Encrypted, key forgotten" in its tooltip, its
  accessible text and a failed row's description.
- A forgotten key stays forgotten whatever copy of the operation a writer saves later, such as
  a Retry, a copy or a local-save retry that read the operation before Forget key. Saving an
  operation with encrypted keys takes the write lock first, reads the forgotten mark from the
  operation's journal in one query and clears those keys before it writes
  (`HistoryStore::persistOperation`). The journal outlives the History record, so the mark
  holds after the record is removed.
- **Delete from destination** deletes the page, then the data file beside it. The data file
  is checked and deleted like the page, with its own durable checkpoint
  (`encryptDataProgress`), and its outcome is the target's `encryptDataStatus`. When the page went
  and the data file didn't, the target stays `failed` or `unknown` with `encrypt_data_left`, and
  **Retry deletion** finds the page gone and tries the data file again. `failed` keeps "The
  encrypted page was deleted, but its data file beside it wasn't. It holds only encrypted data.
  Select Retry deletion." For an `unknown` data file put.gd still reports `encrypt_data_left`, with `dataStatus`
  `unknown`, but its message doesn't claim the file is still there, because put.gd only
  doesn't know yet: "The encrypted page was deleted, but put.gd couldn't confirm whether its
  data file was deleted. Select Retry deletion to check again." A quit after the page went
  and before the data file's turn leaves the target `unknown`, with `dataStatus` the data
  file's recorded status. When that status is already `unknown`, an earlier attempt may have
  deleted it without hearing back, so the message is "The encrypted page was deleted, but put.gd
  quit before confirming whether its data file was deleted. Select Retry deletion to check
  again." Otherwise it is "The encrypted page was deleted, but put.gd quit before it could delete
  its data file. Select Retry deletion." Once Quit
  has begun, the data file's delete intent isn't acknowledged (the journal goes back to
  before it), so no further delete is sent during shutdown. A failed or unknown result read
  after Quit began proves nothing about the server. When a delete checkpoint of the data file
  was already acknowledged, the request may have gone out, so `encryptDataStatus` is `unknown`
  with `delete_unknown` ("put.gd quit before confirming whether the data file was deleted.
  Select Retry deletion to check again.") and the target's message is "The encrypted page was
  deleted, but put.gd quit before confirming whether its data file was deleted. Select Retry
  deletion to check again." Without an acknowledged checkpoint nothing that deletes was sent
  this time, so the data file keeps `unknown` if it was `unknown` and is otherwise `failed`. History keeps the record while the data file's checkpoint holds a request phase,
  as it does for the page's own (`HistoryStore::releasable`). A file beside the page that
  changed at the destination may be a replacement, not put.gd's data file, so it isn't
  put.gd's to delete: the deletion reports `encrypt_data_left` with `dataStatus` `changed` and
  leaves it.
- A data file the destination can't delete safely stays, and the deletion says so before it
  starts. `HistoryService::prepare` asks the provider about the data file's own receipt. When
  its `safeRemoval()` is false (a WebDAV data file over 256 MB on a server that returns no
  strong ETag, such as Apache's mod_dav), the target gets `encryptDataKept` and its confirmation
  adds "The encrypted link's data file beside it stays at the destination, because put.gd can't
  delete it safely there." with the provider's `removeUnavailable` reason, then "It holds only
  encrypted data. Someone who saved the page and has the full link (and its passphrase, if it
  has one) can still open it until you delete the file there yourself." The deletion removes the page and ends `removed`, with
  `encryptDataStatus` `kept`, not with `encrypt_data_left`. `encrypt_data_left` is only for a data file
  put.gd could have deleted and didn't.
- Keys are kept in History's database and the operation journal, the same places and the same
  protection as other bearer links such as presigned URLs. They are never in `config.json`,
  the keychain or the logs; log redaction already drops link fragments.

## The Test check

**Test upload and link** gains one step after a passed Test (`src/app/EncryptCheck.cpp`). A
failed check never fails the Test; it only decides whether encrypted uploads may go there.

1. A destination type that can't encrypt answers `unsupported` with its reason and uploads
   nothing.
2. Otherwise put.gd uploads a small encrypted test page under a random `.html` name, with a name
   check and a conditional create. The page is the `probe` vector of
   `tests/fixtures/encrypt/vectors.json`, whose key is public, so it proves only how the
   destination serves a page.
3. The destination's link for the uploaded page must be https, except a loopback address
   (`localhost`, `127.0.0.1`, `::1`). On plain http anyone on the network path could replace
   the page and read the key, and browsers don't offer WebCrypto there. Any other link fails
   the check, and the test page is cleaned up as in step 6 without being fetched.
4. `putgd-cli --encrypt-probe -` fetches the link as a visitor would: the link on standard input
   (a presigned signature never reaches a command line), no cookies, no cache, at most three
   redirects that stay as safe, a 60-second transfer limit and 1 MB at most.
5. The check passes only for HTTP 200, a `Content-Type` of `text/html` (parameters ignored),
   no `Content-Disposition: attachment`, a final address after redirects that is still https
   (or loopback), and bytes identical to the page that was uploaded: the same size and
   SHA-256, and not cut off at the 1 MB limit. A server that rewrites the
   page would break the byte-for-byte check `putgd-cli open` relies on.
6. The test page is deleted whatever the fetch found, when the destination can delete it
   safely; otherwise it is left and the result says so. The local copy of the test page is
   removed on every way out, a failed write included.

The check runs only on the connection the Test checked. When the destination's connection
fingerprint or credential revision changed since the Test started, no check runs and `encrypt`
is `{state: "untested", reason: "The destination changed during the test. Test again."}`.
A cancel during the check, or one that arrives while the check cleans up its test page,
ends the Test `cancelled`: the setup job fails with error `cancelled`, "The connection test
passed, but the encrypted-link check was cancelled. Test again to check encrypted links."
(`details` `connectionPassed: true`), its `result.encrypt.state` is `cancelled`, and the
destination's own Test record still counts as passed.

The result is `encrypt` in the Test's result: `{state, reason?, cleanup}` with `state` `passed`,
`failed`, `untested`, `unsupported` or `cancelled` and `cleanup` `removed`, `left`, `failed` or `unknown`. While it runs,
`setup.inspect` shows the event `{step: "encrypt", status: "running"}`. The resident keeps it in
`destination-status.json` under `encryptChecks`, keyed by destination, with `passed`, a `reason`
of at most 300 characters, the connection fingerprint and the credential revision it ran with
([destination checks](configuration.md#destination-checks)). It counts only while the
fingerprint matches the destination as saved, and it is forgotten with the destination. At
load a record whose revision isn't the destination's current one is dropped, so a check never
outlives the keys it ran with across a restart. A check made with session-only credentials is
never written, so like the destination's own Test it ends with the session. New
credentials stored for the destination, or for one that shares its keychain entry, clear it
(`DestinationStatus::credentialsChanged`). A check that ran while new keys were stored
records nothing and is reported `untested` with "New credentials were stored during the
check. Test again." A cancelled check records nothing. A cancel stops the page fetch at once and still deletes the
test page. Each
entry of `destinationStatus` carries it as `encrypt` (`{state: passed|failed|untested, reason?,
fingerprint?}`). `fingerprint` is the connection a passed or failed check ran on. Settings
compares it with the destination as edited, so an edited connection shows `untested` and
**On** isn't offered until a Test of that connection passes.

## Where it works

Each provider declares `encryptUnavailable` (why it can't, empty when it can) and `encryptSplit`
(whether the two-file form works) in its capabilities
([provider contract](storage-providers.md)). A provider that says nothing can't encrypt. The
capabilities JSON gives them as `encryptedLinks` (true when `encryptUnavailable` is empty),
`encryptSplit`, and `encryptUnavailable` only when it isn't empty. The `capabilities` method's
`providers` entries describe each type with no destination settings, so SFTP there says
`encryptedLinks: false` (no Link address) and S3 says `encryptSplit: true`. `history.detail`'s
`capabilities` for an item without a receipt are its own destination's.

| Destination | Encrypted links |
| --- | --- |
| S3 and Cloudflare R2 with public links (a Link domain, a public bucket, r2.dev) | Both forms |
| S3 and Cloudflare R2 with expiring (presigned) links | One page only, up to 64 MB |
| Backblaze B2 with public links | Both forms |
| Backblaze B2 with authorized links | One page only: the token is in the link's query, which the page's `./NAME.bin` drops |
| WebDAV with a Link address | Both forms, if the web server serves `.html` as `text/html` |
| SFTP with a Link address | Both forms, if the web server serves `.html` as `text/html`. Without a Link address: "Encrypted links need a Link address for this destination." |
| Custom uploaders (HTTP) | One page only, and only when the Test check passes. The server names what it stores, so the link has the server's name, not put.gd's random one. |
| Nextcloud | No: "Nextcloud share links download HTML files instead of opening them, so an encrypted page can't open there." |
| Proton Drive | Off: "Proton Drive already encrypts files end to end." |

Every destination that can encrypt still needs a passed Test check, with an https link, before
its first encrypted upload. Settings refuses **On** and **On with a passphrase** on Nextcloud and
Proton Drive ([configuration](configuration.md#encrypted-links-setting)).

## Settings and the passphrase

A destination's default is its profile's `encrypt` key, `{mode, passphraseCredentials}`, with
`mode` `off` (also when absent), `on` or `passphrase`
([configuration](configuration.md#encrypted-links-setting)). Settings → Destinations shows it as
**Encrypted links**: **Off**, **On** or **On with a passphrase**. Changing it affects new uploads
only. **On** stays offered only after a passed check of the connection as shown; **Off**
always is. Editing any connection setting marks the check untested in the window until the
edited destination passes a Test. A typed passphrase is cleared once Settings accepts the
store, stays typed when it is refused, and is cleared with the other typed secrets when
Settings closes. Saving a destination whose **Encrypted links** mode didn't change leaves its
`encrypt` object exactly as saved, `passphraseCredentials` included. Agents drive it with
`ui.encryptedLinks` (MCP `drive_encrypted_links`): `inspect`, `mode` and `unlock`. Its `available` says
whether encryption can be turned on there, `modeEnabled` whether the choice can change and
`saved` whether the file has the mode shown, with nothing for this
destination still waiting to save. `mode` answers only once the file has the new
mode and a fresh read from the resident confirms it, or with `settings_not_saved` when it isn't
saved or can't be confirmed (#2103): until then an agent's next
`setup.storeEncryptPassphrase` could read the old mode. A passphrase is typed in the window only.

The passphrase is kept like any other secret, in its own keychain entry or for this session
only, never in `config.json` ([credentials](credentials.md)). `setup.storeEncryptPassphrase`
saves it from Settings, `setup.unlockEncryptPassphrase` reads it, and the startup credential read
loads a saved one. The startup read starts one job for every saved entry, even past the cap
of 32 setup jobs at once. The two run one at a time per destination and take the passphrase
entry's keychain lock. A passphrase reference shared with any other keychain entry, a
destination's keys or another passphrase, is refused, because saving one would replace the
other. `putgd-cli upload --passphrase-file` can give one upload its own passphrase,
which stays in memory and reaches only the encryption helper.

## Diagnostics and logs

A diagnostics package carries each item's encryption `status`, `form`, `passphrase`,
`keyForgotten` and the data file's upload status. It never carries the
key, the link or the passphrase. The log records that a file was encrypted, its form, whether it
has a passphrase and the page size, and the Test check's HTTP status, content type and cleanup.

## Known limits

- **The Test check isn't journaled.** If put.gd stops in the middle of a check, its test page,
  about 10 KB under a random `.html` name, may stay at the destination.
- **A large data file on some WebDAV servers can't be deleted.** WebDAV confirms a file's
  identity only up to 256 MB when the server returns no strong ETag, and Apache's mod_dav
  returns none. put.gd can't delete such a file safely, which is the same limit as for an ordinary
  WebDAV upload of that size. For an encrypted link in the two-file form, **Delete from
  destination** deletes the page and leaves the data file, and its confirmation says so
  ([History](#history)). The user deletes the data file at the destination if it matters.
- **The two-file form holds the whole file in the browser.** The viewer keeps every decrypted
  chunk in memory until the last one opens, so a 2 GB file needs that much memory in the
  recipient's browser. A browser that can't read back a file that large offers
  **Save to file** instead ([after decrypting](#after-decrypting)).
- **Memory wiping is best effort.** put.gd wipes the key, the passphrase bytes, the derived keys
  and the decrypted buffers that OpenSSL works on. Qt strings that held the passphrase or the
  link make their own copies, so put.gd can't promise that memory is wiped.

## Testing

- `tests/unit/test_encrypt.cpp` (`encrypt_format_contract`): every vector in
  `tests/fixtures/encrypt/vectors.json` encrypts to the same bytes and opens again; one full chunk at
  the boundary; changed, cut, extended and reordered bytes never open; a wrong key never opens;
  the passphrase is a second factor; only canonical fragments are keys; the viewer matches its
  pin; pages parse only when unchanged; the Test page is the `probe` vector; and the real
  `putgd-cli --encrypt-job` writes pages that open.
- `tests/integration/test_encrypted_uploads.cpp` (`encrypted_uploads_integration`), through the
  upload pipeline with the real helper: only the page leaves; the per-upload choice beats the
  destination's; encryption that can't happen uploads nothing (`encrypt_check_required`,
  `encrypt_unavailable`, `encrypt_passphrase_locked`); the passphrase is needed beside the link;
  Retry uploads the same ciphertext; large files encrypt as a page and data file; a file too large
  to encrypt, or too large for one page where the destination can't serve two files, is never
  sent, and its Retry fails the same way (`fileTooLargeToEncryptIsNeverSent`,
  `twoFilesWhereLinksCantServeThemIsNeverSent`); a private upload is refused with
  `encrypt_needs_link` and goes out privately with Encrypt off (`privateUploadIsNeverEncrypted`); a
  file already at a job's output path, or one that took a path while the helper ran, survives
  a refusal, a crash or a Cancel (`foreignFileAtAnOutputPathSurvives`,
  `cancelLeavesAFileThatTookAnOutputPath`); a Cancel during the Test's check cancels the Test
  and keeps the connection's pass (`cancelDuringTheEncryptCheckCancelsTheTest`);
  Forget key leaves no link, and a writer that
  read the operation earlier can't put the key back.
- `tests/encrypt/browser_vectors.py` serves the pinned viewer on 127.0.0.1 and opens
  `tests/encrypt/harness.html` in headless Firefox and Chromium. WebCrypto encrypts every vector to
  the same bytes as C++, and each page opens as a recipient would: one page and two files, a
  passphrase, a wrong key, a missing key, changed data and a missing data file. The server
  records every request, and a page that asks for anything but itself and its one `./NAME.bin`
  fails the run. It needs only the Python standard library and the browsers.
- Live uploads to every destination type, opened in Firefox, Chrome and a phone browser
  through the QR code, and acceptance on all five desktops, are recorded on the code pull
  request.
