# Proton runtime provisioning and protocol

Before configuring the application, run:

```sh
python3 tools/proton/provision_runtime.py
```

To reuse a clean official checkout, add `--source /absolute/path/to/sdk`.
`--crypto-source /absolute/path/to/WebPackages` selects an existing clean
checkout of the approved GPL crypto pin. `--offline` requires both checkouts
and cached locked npm packages. Provisioning
uses normal Git/npm build tools; CMake never fetches dependencies. The distro
GCC and Qt toolchain is unchanged.

The executable, manifest and `THIRD-PARTY-NOTICES.txt` are under
`build/dependencies/proton-runtime/`.
The manifest binds the artifact, complete runtime and npm locks, helper source,
approved crypto source/version, runtime versions, dependency notices and each
redistribution file. The manifest, notices and `redistribution/` ship next to
the runtime. See [the source catalog](notices/SOURCES.json) and
[rebuild instructions](notices/REBUILD.md) for required accompanying source assets.
Crypto source 2.2.0 and the npm 2.2.0 bootstrap now match; the public source
commit and full dependency graph are locked. Historical 2.0.0/2.0.1 provenance
remains in [the provenance record](notices/CRYPTO-PROVENANCE.md). Supported command
lines are `--version`, `--stdio` and `--session` only.

`--stdio` consumes exactly one UTF-8 JSON request (maximum 128 KiB) until EOF;
the caller closes stdin. It emits at most one `{"event":"auth","url":...}`
line during `connect`, or bounded/coalesced
`{"event":"progress","phase":"preparing"|"uploading"|"finalizing","bytesTransferred"?:integer,"bytesTotal"?:integer}`
lines during `upload`, then one `{"ok":true,"result":...}` or
`{"ok":false,"error":{"code":...,"message":...,"details":{}}}` line.
Auth URLs are transient UI events. Errors are generic, never provider exception
text. Secrets and account names never appear on the command line. The required
`PUTGD_PROTON_DATA_DIR` is an owned, private, real directory. Timeout defaults
to 120 seconds and is bounded to 1–86,400 seconds (24 hours); login can use 600 seconds.
Deadline errors carry a generic reason and the request budget. A deadline after
mutation dispatch preserves `unknown`; cancellation is distinguished from expiry.

`--session` answers several requests for one account from one process, a JSON
line each (the same 128 KiB bound), until the caller closes stdin. It accepts
`warm`, `checkName`, `upload`, `stat`, `reconcile` and `share` only, all with the
same keychain reference; anything else is `unsupported_request`. `warm` signs
in, opens the connections and reads the account and the configured folder; it
changes nothing and answers `{}`. Each request has its
own deadline, cancellation signal, progress lines and single completion line.
The process keeps the signed-in account, its connections and one Drive client
between requests. An upload for a publication other than the client's gets a
new client, so its drafts keep that publication's client identity. The first
good answers for My files and its root share serve the process's later
clients, and the root share is read as soon as My files names it; no other
response is kept. The clients share the two caches. Requests still read their
destination and ownership from the service, with one exception: the destination
read made for a name check also serves the one upload that follows it within
ten seconds. Once a session has read a destination, a name query for it runs
beside the destination read instead of after it. A passed
deadline answers and ends the process. A request past the size bound is refused
and nothing after it is read.

An upload request may say `linkFollows:true`: the operation will ask for a share
link as soon as the file is uploaded. A new link costs about half a second of
local work before its request can be sent: two bcrypt hashes of its generated
password, a read of the SRP modulus, and the share's session key wrapped under
that password by an iterated hash of 65 MB. The helper then does the hashing
and reads the modulus while the file's bytes are in transit, and wraps the
session key while the request that creates the share is in transit. The
password comes from the SDK's own generator, called ahead; the SDK's link code
is handed that password and gets the prepared answers for exactly those inputs.
A link with a share password of the user's own, a file over 4 MiB, a link asked
for more than ten seconds after its preparation, and a preparation that failed
are computed in line, as before. A preparation ends with the request after the
upload, whatever that request is, and nothing of it is stored. The link itself
is then four requests in sequence: the ownership read, the share, the link and
the second ownership read.

An upload's answer names the storage host its blocks went to (`transferHost`),
and an upload request may name one (`transferHostHint`). The application keeps
the last host per account in memory and names it in the next upload. For a file
over the single-request limit and of one 4 MiB block at most, the helper then
opens a connection to that host while the upload's draft is being made, with
one unauthenticated request that carries nothing of the account or the file. A
file of several blocks gets none: its blocks are sent side by side, and the
block on the opened connection started ahead of the others, which then crawled. Only a `*.proton.me` host other
than the API's is accepted in either direction. A 360 KB upload spent about
0.3 s less with the connection already open.

Proton's API answers each connection from one of several backends and stays
with it for the connection's life. From the lab, the same read took 0.17 s on
about a quarter of connections and 0.35 to 0.6 s on the rest, with the same
network round trip on all of them, and a compiled `fetch` gives no say in which
pooled connection carries a request. The helper therefore sends its first GET
to the API on six new HTTP/2 connections at once, keeps the one that answers
first, closes the others, and sends every later API request as a stream on the
kept one. If that first answer took more than 0.1 s beyond the connection's
ping, a second round of six looks for a faster connection in the background and
replaces the first if it finds one, until a request is about to change
something: from then on the kept connection stays, so a change and the reads
that follow it reach one backend. Only a GET is ever sent more than once.
This is a shortcut, never a requirement: the ordinary client carries the
request, and every later one in that process, when no candidate connects or
answers within four seconds, when the first answer is not a 200 or a 401, when
a proxy is configured in the environment, or when a request on the kept
connection fails. A failed GET, and a request the service says it never acted
on, is sent again on the ordinary client; any other failure is reported as a
lost connection is. File blocks go to Proton's storage hosts through the
ordinary client as before. An idle kept connection is pinged every 20 seconds.

Sharing reads the file before and after the link is made. For a file this
process uploaded within the last thirty seconds, a read that may predate the
upload is made again before it is believed: one the service answered without
an error and that either did not return the file (the pinned SDK also leaves
out a file returned without its active revision, as a draft), could not tell
whose it is, or returned the file's own revision with none of the metadata it
was uploaded with. Revisions never change, so that last read is not another
file, although the ownership check alone calls it foreign. The reads are made
again up to five times, 0.25, 0.5, 1, 2 and 4 s apart. A read that shows
another revision, name, folder or trash is believed at once and the link
step reports `changed`. A file that still reads that way gets no link and is
reported `unknown`, which the application inspects again on Retry. Before
#1399 such a file was reported `changed` after three reads 0.3 s apart, or at
once for a revision without metadata, which told the user the file had been
replaced and ended the link step.

A token refresh makes the account code forget the user and address keys it
loaded. The helper puts a load that was under way back for the same session,
so the first request after an expired token no longer reads the user and the
addresses twice.

The application holds one session around an operation's name check, upload and
link for a destination, on that operation's thread, and holds the account lock
for as long as the helper lives. It starts that helper when the action is
invoked (`StorageProvider::anticipate`), with a `warm` request, so the sign-in
overlaps choosing and capturing the content. Starting ahead never waits for the
account lock or the runtime's hash; an Edit & upload starts nothing ahead. The
next request for the account takes the waiting helper after reading the
warm-up's answer; a warm-up that was refused, unclear or late ends that helper
and the request starts its own. A helper nobody used ends when its action
does: it is left to finish what it is doing, since a helper killed during a
session refresh loses the account. It asks a kept helper again only after a clean
answer (`ok:true` with an empty `result.error`) and only while the helper is
running and silent; otherwise it ends that helper and starts another for the
next request. History and setup actions use `--stdio`. After the warm-up, one
64 KiB publish makes 8 requests: two for the name check, one for the upload and
five for the link.

An operation invoked while another operation's session has the account, or
while the helper started ahead for another operation has not been taken yet,
starts no helper of its own. Every operation registers that it will ask, the
one that started the helper included, because either may get the account
first. A session's helper, signed in and connected, passes to a registered
operation when that session ends, in place of a new process and its sign-in. Only a helper that waits for its next request
and has said nothing since its last clean answer is passed on; it keeps the
account lock meanwhile, and ends when no registered operation is left to take it.

The application checks the bundled runtime's SHA-512 before it starts a helper.
Hashing the 110 MB file takes most of a second, so one verdict stands while the
file's device, inode, size, modification and change times are unchanged, and a
resident with a Proton destination runs the first check in the background
shortly after it starts.

Every request carries `schemaVersion:1`, `method`, and `destination` containing
`providerId:"proton-drive"`, `profileId`, `profileRevision`, `settings` and
`credentials:{mode:"keychain",reference}`. Proton endpoints and the keychain
service are fixed. No plaintext credential store is supported.

Methods: `connect`, `inspectAccount`, `createFolder`, `storeSharePassword`,
`checkName`, `upload`, `stat`, `reconcile`, `share`, `remove`.
Account inspection lists only top-level MyFiles folders. Folder creation takes
`options.name` and creates directly under MyFiles. File operations require
configured root/folder UIDs and a folder equal to MyFiles or its direct child.
No method recursively scans the account.
An existing configured root is checked during account inspection and Connect.
Connect inspects an existing valid session; an occupied invalid session reference
requires a new reference. Browser authentication never replaces a prior account
snapshot in place.
The session is stored through the Secret Service `default` alias. A refused
write, during Connect or a token refresh, reports `keychain_unavailable`.
A new process's exchange with the secret service can go wrong for an entry that
is there (#1418): on KDE's `ksecretd`, of 700 new processes reading one entry,
5 were refused (`Client public key size is invalid`) and 3 were answered with
nothing. A refused read or write made again in the same process succeeds, so the
helper makes it again up to three times, 30, 90 and 240 ms apart, before it
reports `keychain_unavailable`. An empty answer stays empty for the life of the
process. A helper that finds no session therefore answers `session_unread`,
having changed nothing, unless its request says `sessionRetry:true`. The
application then asks once more through a new helper with `sessionRetry:true`,
which believes what it finds: an account that really is not connected answers
`authentication_required` as before, one helper start later.

Upload accepts only `condition:"absent"`, plus `candidate`, `publicationId`,
and `artifact:{path,name,mime,bytes,sha256}`. It hashes and streams the same open
regular file, rejects a final symlink, and provides expected size/SHA-1 to the
SDK. A file under 128 KiB goes in the SDK's single small-file request, enabled
as in Proton's own CLI at this pin; larger files stream 4 MiB blocks. Encrypted additional metadata stores publication ID and SHA-256. Receipts
contain exact SDK completion node/revision IDs, the revision concurrency token,
and SHA-256 inside `objectReference`. Upload never overwrites an existing file
or uploads a revision. Uncertain mutations return `unknown` for reconciliation.
Upload progress uses the SDK's callback and a bounded reporter with one pending
stdout write and one replaceable latest event. It drains before final completion.
The pinned SDK reports encrypted transfer traffic, can roll back during retries,
and supplies no matching total. The helper leaves `bytesTotal` absent rather
than dividing by plaintext artifact size. It exposes preparing/uploading phases;
there is no public SDK commit-start callback from which to infer finalizing.
Progress never supplies receipt identities or establishes publication.

Stat takes the receipt; reconciliation takes the original candidate,
publication ID and artifact checksum. Both return `own`, `foreign`, `missing`
or `unknown`. Missing requires a successful complete destination listing.
Ownership includes folder, node, name, revision, encrypted publication marker
and checksum. Key, name and revision content authors must have successful SDK
signature verification; decrypted marker data alone does not prove ownership.
Denial never proves absence. Name checks return `absent`,
`present` or `inconclusive`. They use the public SDK's hashed name-availability
query; lookup failures remain inconclusive. Atomic SDK creation still decides
collisions, and missing-file reconciliation still requires a complete listing.

Each Drive client has two bounded 64-entry caches in memory, one for SDK crypto keys
and one for node metadata. Neither is written to disk or outlives the process.
The helper's own destination and ownership checks never read the metadata
cache: each one is a batched service read of the account root, the configured
folder and, for stat and sharing, the file, during which the cache answers
nothing. The SDK reuses what that read returned for its own parent and key
lookups, which were a request each before. Sharing still reads the file from
the service before and after the mutation. The user and address keys load
beside the first Drive read, and a new link's SRP modulus beside the share's
creation, or beside the upload when the link was prepared there. One 64 KiB publish made 38 requests before these changes and makes
20 with them from three `--stdio` helpers.

Share takes the receipt and `options:{action:"create"|"revoke"|"inspect",
expirationDays,passwordReference}`. The caller persists its mutation checkpoint
before launching. Create uses viewer access and preserves the SDK URL intact.
Revoke removes URL access only. Changed identity/revision rejects mutation.
After SDK sharing/inspection, ownership is checked again through fresh server
reads. Changed or unknown ownership suppresses the returned URL; there is no
rollback touching other member shares. Expired URL access returns private with
`linkAvailable:false`. Only active, unexpired URL access is returned as usable.
Create and inspect results carry the service-confirmed `expiresAt` (ISO 8601 UTC)
when the link has an expiry, also after it has passed.
Results contain `progress:{schemaVersion:1}`. Password storage accepts
`options:{reference,password}` through stdin and never overwrites a different
existing value. Sharing loads its immutable password reference from keychain.

Remove returns `unsupported_safe_removal` before keychain/network initialization:
the SDK lacks atomic exact-revision guarded deletion. The helper does not replay
mutations or reupload files at the application level. The official SDK retains
its internal HTTP retry behavior.

Every stdout write is awaited before exit, including authentication events,
timeouts and final/error completions. Provisioning runs pure protocol/identity
tests, compiled no-account smoke checks, and a separately compiled synthetic
large-output fixture using the same production writer. These checks do not prove
live account or graphical desktop acceptance.


## Opt-in latency probe

`cmake --build --preset dev --target proton_latency_probe` builds a non-installed
probe using the production C++ adapter and bundled helper. Its required arguments are
the configuration file and a new, owned private output directory. An optional
third argument selects synthetic payload bytes (1–67,108,864; default 65,536). The config must
contain exactly one Proton profile with an isolated `put.gd-` test folder or a
builder's `putgd-lab-` session folder. It holds one session around its requests,
as an operation does; `PUTGD_PROBE_SEPARATE_HELPERS=1` gives each request a
helper of its own. Resolve
and preflight the actual keychain D-Bus Unix socket with
`tools/diagnostics/check_socket_paths.py --socket` before running; set `TMPDIR` to
the new short output directory. No put.gd session socket is created by this probe.

It creates one unique synthetic file, times collision preflight, upload,
sharing and revocation, and revokes only its own new test link. It leaves the
private file in the isolated folder. Stdout contains stage timings and generic
status/error codes only. `upload-progress.jsonl` and `progress-summary.json`
retain numeric SDK progress and whether counters arrived before completion.
Private journals retain exact object identities and
credential references for recovery; exclude those journals and the source config
from diagnostics or review packets. Capability URLs are omitted from the probe's
retained receipts. On failure inspect the retained intent before another mutation.

## Opt-in recovery probe

Build `proton_recovery_probe` explicitly with
`cmake --build --preset dev --target proton_recovery_probe`. This non-installed
service test tool calls the production adapter. Send one JSON request through
stdin and capture stdout in a private parent-process pipe (never a terminal or
redirected file). Requests supply `method`, `id`, an immutable `destination`, and
a new owned 0700 canonical `output` directory. Preflight the actual keychain
D-Bus Unix socket before launching. The probe creates no put.gd session socket.

Setup methods are `connect`, `inspectAccount`, `createFolder`, and
`storeSharePassword`; their `options` use the production setup protocol. File
methods are `checkName`, `upload`, `reconcile`, `stat`, and `share`. Upload and
reconciliation take `artifact`, `publicationId`, and `candidate`; stat/share use
an original `receipt`. Share `options` carry `action` (create/inspect/revoke),
`expirationDays`, and `passwordReference`. `cancelOnProgress` is an upload-only
fixture control that cancels after the first nonzero SDK byte frame.

Use only fresh `put.gd-p1-` folders and `putgd-p1-` candidate names. Private
intent/checkpoint/completion files retain exact references and receipt identities
but omit receipt capability URLs. Authentication events and successful URLs can
appear on the private output pipe: the parent must keep them out of logs and
review packets. Password values must remain in parent-process memory and the
keychain. Keep failed intents before retrying, reconcile uncertain uploads first,
and never rebind old receipts to another session/destination. No remote deletion
is provided. The [P1 receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-15-proton-p1-recovery.md)
records the service checks and their original limits. The
[application follow-up](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-15-proton-p1-app-recovery.md)
completes the bounded P1 normal-use recovery campaign. The
[P2 resource receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-15-proton-p2-resources.md)
records active uploads/cancel/retry/repeats and idle, including a rejected
smaller-heap experiment. No runtime optimization was retained; P3–P6 stay open.

Publish a versioned Proton source transport through the guarded publisher:

```sh
python3 tools/releases/publish_proton_sources.py \
  --catalog tools/proton/notices/SOURCES.json \
  --directory /absolute/path/to/verified/source-parts \
  --commit FULL_SOURCE_COMMIT_SHA \
  --notes /absolute/path/to/public-source-notes.md
```

Pin the selected source snapshot in `publication.sourceTagCommit` before
publishing; `--commit` must match it. The publisher validates the checksum index,
checks existing tags before mutation, verifies pinned local/uploaded bytes, resumes matching drafts and
passes `--prerelease --latest=false` at creation and publication. It rejects an
existing full release. Proton source releases must stay out of the stable
installer channel; installer publication also verifies their prerelease status.

On a slow upload link, `GODEBUG=http2client=0` gives the two large parts separate
HTTP connections in gh's concurrent uploader. The publisher allows up to an
hour for transport and verifies every remote digest before publication.
