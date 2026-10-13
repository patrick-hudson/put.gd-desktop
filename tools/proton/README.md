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
line during `connect`, then one `{"ok":true,"result":...}` or
`{"ok":false,"error":{"code":...,"message":...,"details":{}}}` line.
Auth URLs are transient UI events. Errors are generic, never provider exception
text. Secrets and account names never appear on the command line. The required
`PUTGD_PROTON_DATA_DIR` is an owned, private, real directory. Timeout defaults
to 120 seconds and is bounded to 1–600 seconds; login can use 600 seconds.

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

Stat takes the receipt; reconciliation takes the original candidate,
publication ID and artifact checksum. Both return `own`, `foreign`, `missing`
or `unknown`. Missing requires a successful complete destination listing.
Ownership includes folder, node, name, revision, encrypted publication marker
and checksum. Key, name and revision content authors must have successful SDK
signature verification; decrypted marker data alone does not prove ownership.
Denial never proves absence. Name checks return `absent`,
`present` or `inconclusive`.

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
