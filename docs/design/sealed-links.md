# Sealed links

A sealed link is an ordinary put.gd upload that only someone holding the link can open
(#2079). put.gd encrypts the file on the user's computer and wraps it in a small
self-decrypting web page, and that page goes to the destination the user already uses. The
key travels only in the link's fragment, the part after `#`, which browsers never send to a
server. The bucket, the CDN, its access logs and anyone who can list the bucket see only
ciphertext under a random name. There is no put.gd server and no account.

```
https://files.example.com/k3Xq9vTa2mPw.html#v1.Rk2c8w…43 chars…
└── your bucket serves this page ──────────┘└─ key: never leaves the browser
```

Sealing is off by default. A destination turns it on in its **Seal links** setting, and one
upload can turn it on or off for itself. How it reads to users is in the
[user guide](../user/user-guide.md#sealed-links). Its failure codes are in
[failure codes](../user/errors.md#sealed-links), and its session methods, `putgd-cli` commands
and MCP tools are in [the CLI](../automation/cli.md#sealed-links) and
[MCP](../automation/mcp.md#tools-and-effects) documents.

## What it protects against, and what it doesn't

The user guide gives the same table in plain words:

| Threat | Protected? |
| --- | --- |
| Someone who can read the bucket: a public bucket, a leaked read key, the cloud provider, a backup | **Yes.** They see ciphertext and a random object name. |
| CDN and server access logs, proxies, link scanners that fetch the URL without the `#` part | **Yes** |
| Chat apps unfurling a preview | **Yes.** There's no preview. Documented as intended. |
| Anyone the full link is forwarded to | **No**, unless a passphrase is set (see below) |
| Someone who can *change* files in the bucket and swaps the page for one that steals the key | **No** for the page on its own. Stated as a known limit. `putgd-cli open` verifies the page before decrypting. |
| Recipient's device or browser compromised | No |

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

The source of truth is the header comment in `src/seal/SealCrypto.h`. A sealed container is
a 96-byte header followed by AES-256-GCM chunks in the STREAM construction. OpenSSL does every
primitive on put.gd's side and WebCrypto in the browser. Nothing is hand-written crypto.

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

The viewer refuses a header with any other magic, version, flag, chunk size or nonzero
padding, and an iteration count outside 1,000 to 10,000,000 when a passphrase is set (0
otherwise). Tests seal with fewer iterations; uploads always use 600,000.

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
key stays the main protection. A passphrase is 1 to 1,024 bytes of UTF-8 on one line and is
normalized to NFC on both sides.

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

`src/seal/viewer.html` is the one viewer. It is about 12 KB, with one inline script and one
inline style and nothing loaded from anywhere else. Its exact bytes are compiled into put.gd
(`cmake/Seal.cmake`) and pinned by SHA-256 in `viewerSha256` (`src/seal/SealPage.cpp`):

```
d5d4a75407088bdc26cb764bdd48c283ba0673aaced941ef8d0bd0eb5bb908ae
```

`tools/seal/pin_viewer.py` rewrites the two CSP hashes and `viewerSha256` after an edit and
prints the new hash for this document; `--check` changes nothing and fails when a hash is out
of date. The unit test `seal_format_contract` checks the compiled-in viewer against the pin,
and the sealing helper refuses to seal with a viewer that doesn't match
(`seal_viewer_changed`). A change to the viewer is therefore always a deliberate, reviewed
change to this hash.

A sealed page is the viewer with its marker `<!--putgd-sealed-->` replaced by one data
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

The viewer works offline once loaded, and in phone browsers through the QR code: WebCrypto
works in current iOS and Android browsers. It sends no request with the key and keeps the
key only in the address bar.

## Forms and limits

| File size | What uploads |
| --- | --- |
| Up to 64 MB (67,108,864 bytes) | One self-contained `.html` page. Works on every destination that serves HTML as a page. |
| Over 64 MB, up to 2 GB (2,147,483,648 bytes) | The page plus `NAME.bin` beside it. The page fetches `./NAME.bin` from its own origin, so no CORS setup is needed. This needs a destination with a public Link address; per-object presigned or authorized links can't serve it. |
| Over 2 GB, or over 64 MB where the destination can't serve two files | The file uploads unsealed. The notification and History say why in one sentence. |

The one-page form holds base64, so its page is about a third larger than the file. The page
and the data file get separate random names of 12 letters and digits (`SealJob::randomName`),
never the capture's own name.

## Process boundaries

The resident never maps libcrypto:

- `putgd_seal_page` (Qt Core only) builds and checks pages. The resident links it.
- `putgd_seal` (OpenSSL) holds the crypto. Only `putgd-cli` and tests link it, and
  `tools/ci/check_layers.py` keeps it that way.

Each item is sealed by a short-lived `putgd-cli --seal-job` child (`src/core/SealJob.cpp`,
`src/seal/SealJobMain.cpp`). The resident writes one JSON request on its standard input: the
file's private path, size, name and type, the form, the output paths and, when there is one,
the passphrase. The child runs with core dumps off and a lower priority, reads exactly the
stated size, and writes the page (and data file) as new owner-only files that must not exist
yet, then syncs them. It answers on standard output with the key and the size and SHA-256 of
each file. The resident hashes the files itself and refuses them when they differ, then
records them as the item's artifacts. Sealing a file has a 30-minute deadline and stops on
cancel. The passphrase reaches the child only on standard input, and the key comes back only
on standard output; neither is ever on a command line.

`putgd-cli open` and `putgd-cli --seal-probe` (the Test check below) run without the resident
and also live in the CLI.

## The pipeline

Sealing sits in the shared pipeline between the immutable artifact and naming, in
`OperationService::sealItems`.

1. **Whether to seal.** An upload is sealed when it asked to be (`seal` on the request, kept as
   the operation's `seal.requested`), else when its destination's `seal.mode` is `on` or
   `passphrase` in the settings the upload was accepted with. The editor's and recording
   review's **Seal** toggle (`operation.seal`) sets the operation's own choice until its
   upload starts sealing. Diagnostics package uploads are never sealed.
2. **Refusals.** When sealing was asked for and can't happen, the file is never uploaded
   unsealed. The item fails before anything is sent, with one of:
   - `seal_unavailable`: the destination type can't serve a sealed page (its provider's
     `sealUnavailable`, such as Nextcloud or Proton Drive).
   - `seal_check_required`: the destination hasn't passed the sealed-link Test check for the
     settings this upload was accepted with, or failed it (the message gives the check's
     reason).
   - `seal_passphrase_locked`: the destination asks for a passphrase and it isn't loaded and
     the request didn't carry one. Uploads never unlock the keychain.
   - `seal_failed` (or `seal_viewer_changed`): the sealing helper is missing, failed, timed out
     or wrote files that didn't match its answer.
3. **Size exceptions.** A file over 2 GB, or over 64 MB on a destination without the two-file
   form, uploads unsealed with `seal.status: off` and a one-sentence `reason`, which the
   notification and History show. This is the one case where a file meant to be sealed goes
   out unsealed, as the design set it.
4. **Sealing.** The item's phase is `sealing` while the helper runs. The item keeps its own
   file as its artifact, for previews and local copies; what uploads under the item's
   candidate name is the sealed page (`OperationItem::uploadArtifact`).
5. **The data file first.** In the two-file form the data file uploads before the page, under
   its own random name with a name check and a conditional create, journaled in
   `seal.data`. Its name is written inside the page, so it is never renamed: a taken name fails
   the item with `seal_name_taken`. An unknown data-file outcome is reconciled before it is
   ever sent again, as an item's own upload is.
6. **Retry** sends the same page and data file again, so a link already shared keeps working.
   An item a Retry sends that was never sealed (it failed or was cancelled first) is sealed
   then.
7. **The link** is the destination's link for the page, then `#` and the key
   (`OperationItem::link`). The clipboard, the link-ready notification, History's **Copy
   link**, **Open in browser** and **Show QR code** all use it.

When every file of an upload is sealed and its link is copied, the notification's title is
`Sealed link copied` (`Sealed links copied` for several) and its body is `Sealed link copied
to clipboard.`, followed by `Send the passphrase separately.` when a passphrase is set. A
reason a file went out unsealed is added to the body.

An item's `seal` record:

| Field | Meaning |
| --- | --- |
| `status` | `sealed`, or `off` with `reason` when sealing was turned off for it by size |
| `form` | `inline` (one page) or `split` (page and data file) |
| `key` | The fragment `v1.…`; empty with `keyForgotten: true` after Forget key |
| `passphrase` | `true` when the link also needs a passphrase. The passphrase itself is never kept here. |
| `page` | The sealed page's private artifact |
| `data` | Two-file form only: the data file's artifact, candidate, upload status, receipt, name-check reference, condition, publication ID, journal and error |

## History

- History records carry `link` (the link to copy, with the key after `#` while it is
  available) and `sealed`. Private paths and the data file's upload journal are left out of
  `history.detail` and `operation.inspect`.
- Sealed items show a lock glyph. **Copy link**, **Copy links**, **Open in browser** and
  **Show QR code** include the key.
- **Forget key** (`history.forgetKey`) removes the key from the History record and from its
  operation's journal in one transaction (`HistoryStore::forgetSealKey`). The file stays at the
  destination and nobody, put.gd included, can open it again. Copy link, Open in browser and
  Show QR code stop being offered. `history_not_sealed` answers for an item that isn't a sealed
  link, and `history_busy` while its upload is still running. The key already copied or sent
  elsewhere is out of put.gd's reach.
- **Delete from destination** deletes the page, then the data file beside it. The data file
  is checked and deleted like the page, with its own durable checkpoint
  (`sealDataProgress`), and its outcome is the target's `sealDataStatus`. When the page went
  and the data file didn't, the deletion reports `seal_data_left`: what stays holds only
  ciphertext that no page points to.
- Keys are kept in History's database and the operation journal, the same places and the same
  protection as other bearer links such as presigned URLs. They are never in `config.json`,
  the keychain or the logs; log redaction already drops link fragments.

## The Test check

**Test upload and link** gains one step after a passed Test (`src/app/SealCheck.cpp`). A
failed check never fails the Test; it only decides whether sealed uploads may go there.

1. A destination type that can't seal answers `unsupported` with its reason and uploads
   nothing.
2. Otherwise put.gd uploads a small sealed test page under a random `.html` name, with a name
   check and a conditional create. The page is the `probe` vector of
   `tests/fixtures/seal/vectors.json`, whose key is public, so it proves only how the
   destination serves a page.
3. `putgd-cli --seal-probe -` fetches the link as a visitor would: the link on standard input
   (a presigned signature never reaches a command line), no cookies, no cache, at most three
   redirects that stay as safe, a 60-second transfer limit and 1 MB at most.
4. The check passes only for HTTP 200, a `Content-Type` of `text/html` (parameters ignored),
   no `Content-Disposition: attachment`, and bytes identical to the page that was uploaded. A
   server that rewrites the page would break the byte-for-byte check `putgd-cli open` relies on.
5. The test page is deleted whatever the fetch found, when the destination can delete it
   safely; otherwise it is left and the result says so.

The result is `seal` in the Test's result: `{state, reason?, cleanup}` with `state` `passed`,
`failed` or `unsupported` and `cleanup` `removed`, `left`, `failed` or `unknown`. While it runs,
`setup.inspect` shows the event `{step: "seal", status: "running"}`. The resident keeps it in
`destination-status.json` under `sealChecks`, keyed by destination, with `passed`, a `reason`
of at most 300 characters and the connection fingerprint
([destination checks](configuration.md#destination-checks)). It counts only while the
fingerprint matches the destination as saved, and it is forgotten with the destination. Each
entry of `destinationStatus` carries it as `seal` (`{state: passed|failed|untested, reason?}`).

## Where it works

Each provider declares `sealUnavailable` (why it can't, empty when it can) and `sealSplit`
(whether the two-file form works) in its capabilities
([provider contract](storage-providers.md)). A provider that says nothing can't seal.

| Destination | Sealed links |
| --- | --- |
| S3 and Cloudflare R2 with public links (a Link domain, a public bucket, r2.dev) | Both forms |
| S3 and Cloudflare R2 with expiring (presigned) links | One page only, up to 64 MB |
| Backblaze B2 with public links | Both forms |
| Backblaze B2 with authorized links | One page only: the token is in the link's query, which the page's `./NAME.bin` drops |
| WebDAV with a Link address | Both forms, if the web server serves `.html` as `text/html` |
| SFTP with a Link address | Both forms, if the web server serves `.html` as `text/html`. Without a Link address: "Sealed links need a Link address for this destination." |
| Custom uploaders (HTTP) | One page only, and only when the Test check passes. The server names what it stores, so the link has the server's name, not put.gd's random one. |
| Nextcloud | No: "Nextcloud share links download HTML files instead of opening them, so a sealed page can't open there." |
| Proton Drive | Off: "Proton Drive already encrypts files end to end." |

Every destination that can seal still needs a passed Test check before its first sealed
upload.

## Settings and the passphrase

A destination's default is its profile's `seal` key, `{mode, passphraseCredentials}`, with
`mode` `off` (also when absent), `on` or `passphrase`
([configuration](configuration.md#sealed-links-setting)). Settings → Destinations shows it as
**Seal links**: **Off**, **On** or **On with a passphrase**. Changing it affects new uploads
only.

The passphrase is kept like any other secret, in its own keychain entry or for this session
only, never in `config.json` ([credentials](credentials.md)). `setup.storeSealPassphrase`
saves it from Settings, `setup.unlockSealPassphrase` reads it, and the startup credential read
loads a saved one. `putgd-cli upload --passphrase-file` can give one upload its own passphrase,
which stays in memory and reaches only the sealing helper.

## Diagnostics and logs

A diagnostics package carries each item's seal `status`, `form`, `passphrase`,
`keyForgotten`, the unsealed `reason` and the data file's upload status. It never carries the
key, the link or the passphrase. The log records that a file was sealed, its form, whether it
has a passphrase and the page size, and the Test check's HTTP status, content type and cleanup.

## Testing

- `tests/unit/test_seal.cpp` (`seal_format_contract`): every vector in
  `tests/fixtures/seal/vectors.json` seals to the same bytes and opens again; one full chunk at
  the boundary; changed, cut, extended and reordered bytes never open; a wrong key never opens;
  the passphrase is a second factor; only canonical fragments are keys; the viewer matches its
  pin; pages parse only when unchanged; the Test page is the `probe` vector; and the real
  `putgd-cli --seal-job` writes pages that open.
- `tests/integration/test_sealed_uploads.cpp` (`sealed_uploads_integration`), through the
  upload pipeline with the real helper: only the page leaves; the per-upload choice beats the
  destination's; sealing that can't happen uploads nothing (`seal_check_required`,
  `seal_unavailable`, `seal_passphrase_locked`); the passphrase is needed beside the link;
  Retry uploads the same ciphertext; large files seal as a page and data file; a file too large
  for the destination uploads unsealed with its reason; and Forget key leaves no link.
- `tests/seal/browser_vectors.py` serves the pinned viewer on 127.0.0.1 and opens
  `tests/seal/harness.html` in headless Firefox and Chromium. WebCrypto seals every vector to
  the same bytes as C++, and each page opens as a recipient would: one page and two files, a
  passphrase, a wrong key, a missing key, changed data and a missing data file. The server
  records every request, and a page that asks for anything but itself and its one `./NAME.bin`
  fails the run. It needs only the Python standard library and the browsers.
- Live uploads to every destination type, opened in Firefox, Chrome and a phone browser
  through the QR code, and acceptance on all five desktops, are recorded on the code pull
  request.
