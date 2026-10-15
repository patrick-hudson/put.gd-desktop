# Proton runtime provisioning and protocol

Before configuring the application, run:

```sh
python3 tools/proton/provision_runtime.py
```

To reuse a clean official checkout, add `--source /absolute/path/to/sdk`.
`--offline` requires that checkout and cached locked npm packages. Provisioning
uses normal Git/npm build tools; CMake never fetches dependencies. The distro
GCC and Qt toolchain is unchanged.

The executable, manifest and `THIRD-PARTY-NOTICES.txt` are under
`build/dependencies/proton-runtime/`.
The manifest binds the artifact, upstream commit, npm lock, helper source and
runtime versions, and dependency notices. Both files ship next to the runtime. Supported command lines are
`--version` and `--stdio` only.

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

Upload accepts only `condition:"absent"`, plus `candidate`, `publicationId`,
and `artifact:{path,name,mime,bytes,sha256}`. It hashes and streams the same open
regular file, rejects a final symlink, and provides expected size/SHA-1 to the
SDK. Encrypted additional metadata stores publication ID and SHA-256. Receipts
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

Each request uses a bounded 64-entry SDK crypto-key cache. It has no disk or
cross-request lifetime. Entity metadata uses NullCache, preserving fresh
server identity/revision reads before and after sharing.

Share takes the receipt and `options:{action:"create"|"revoke"|"inspect",
expirationDays,passwordReference}`. The caller persists its mutation checkpoint
before launching. Create uses viewer access and preserves the SDK URL intact.
Revoke removes URL access only. Changed identity/revision rejects mutation.
After SDK sharing/inspection, ownership is checked again through fresh server
reads. Changed or unknown ownership suppresses the returned URL; there is no
rollback touching other member shares. Expired URL access returns private with
`linkAvailable:false`. Only active, unexpired URL access is returned as usable.
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
contain exactly one Proton profile with an isolated `put.gd-` test folder. Resolve
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
is provided. The [P1 receipt](../../docs/evidence/2026-10-15-proton-p1-recovery.md)
records the service checks and their original limits. The
[application follow-up](../../docs/evidence/2026-10-15-proton-p1-app-recovery.md)
completes the bounded P1 normal-use recovery campaign. The
[P2 resource receipt](../../docs/evidence/2026-10-15-proton-p2-resources.md)
records active uploads/cancel/retry/repeats and idle, including a rejected
smaller-heap experiment. No runtime optimization was retained; P3–P6 stay open.
