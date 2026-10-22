# Proton Drive storage

Proton Drive is a provider in the shared upload, retry and history pipeline. The
Qt app in a Proton-enabled build uses a privately packaged executable built
from the official Drive SDK and pinned official account/CLI sources. Users of
that build do not install a CLI, Bun or Node.js. This is a third-party integration, not officially supported by Proton.
put.gd intends to ship and maintain it as an experimental production integration.
The 2026-10-15 product decision accepts upstream's readiness position; ongoing
compatibility, packaging and user recovery are put.gd responsibilities. See
[the evidence and release gates](proton-drive-release-gates.md) for current status
and the SDK update/migration procedure.

The unmodified official CLI cannot supply the upload receipt this application
needs: its pinned upload command discards completion node/revision IDs and
prints aggregate transfer counters. Its sharing password is an argv option.
The private helper therefore calls the same official SDK directly and retains
the official CLI/account foundation for authentication. It does not reimplement
Proton cryptography. This replaces the initial CLI-only candidate.

In Destinations, add a Proton Drive destination. Non-secret edits save automatically.
Choose Connect account and finish sign-in in the browser. Authentication stays in the desktop secret service
under put.gd's own namespace; it does not use the official Drive CLI's session.
Choose a top-level folder in My files or explicitly create a new put.gd folder,
and the selection saves automatically. Settings retain its display name alongside
the folder ID so the picker still shows the selected folder after reopening.
The Refresh folders button beside Upload folder updates a renamed folder’s display
name; uploads use the ID. Queries show Loading folders while working and preserve
the known account email and selection, including when switching destinations.
Existing configurations without a name keep working. Account setup never uploads a file.

Connect stores the session in the secret service's default collection (the
Secret Service `default` alias). A secret service without one refuses the write,
for example gnome-keyring with no login keyring and no prompt to create a default
one. Connect then fails with "The private keychain is unavailable."
(`keychain_unavailable`), and token refreshes report the same error. Create or
unlock a default keyring, then connect again. KWallet and KeePassXC secret
services were not tested; see [the round-11 evidence](evidence/2026-10-20-round11-proton/README.md).

Use **Test connection** to check that the saved account and selected upload folder
are still accessible. A successful check satisfies the current session's
untested-destination warning when choosing **Make default**. This check is
read-only: it does not verify upload permission, file transfer, or share-link
creation. Its result states those limits explicitly.

Proton enforces each plan's per-file size limit on the server; the pinned SDK sets
none of its own, so import does not reject large files. A file over the limit fails
with `destination_file_too_large_for_plan` (service error 200003), naming the file's
size and the plan limit, rather than `quota_exceeded`, which means the drive or a
sharing limit is full. After either refusal the helper asks Proton whether the file's
name is still free in the folder. Unfinished drafts hold their names too, so a free
name means nothing was created and the item fails; Retry then uploads again without
reconciling. A taken name or a failed check leaves the item unknown until Retry
reconciles it. On a free 5 GiB plan, tested live on 2026-10-20, Proton refused every
oversized file at draft creation with the storage quota (`quota_exceeded`, service
error 200002) before any per-file limit, from 5.5 GiB to 100 TiB, so 200003 was not
observed; see [the round-10 evidence](evidence/2026-10-20-round10-proton/README.md).

Sharing is the destination default. Clear that choice for private uploads, or
use Settings → Capture → Per-action exceptions to override publication per action. Private upload
creates no public link and leaves the clipboard unchanged. Existing folder and
member access still applies. Shortcuts remain global app/action settings.

Advanced settings set new-link expiration (0–365 days; 0 means no expiration)
and an optional password. Store the password explicitly in the secret service;
configuration and receipts contain only its reference. Use no link password
clears that reference for future links. Password entry is excluded from retained
UI state and CLI arguments. Old receipts keep their original account, folder,
credential and sharing settings when destinations are edited.

History shows a link's expiry as Proton confirmed it, "Link expires on …".
Once it passes, the row says "Link expired on …" and Copy link and Open in
browser are off. `history.detail` carries the expiry as `shareExpiresAt` and
`share.inspect` as `expiresAt`. Inspecting an expired link never recreates it.

The Proton destination's Advanced link settings include **Request deadline** in
seconds (default 120; range 1–86,400, or 24 hours). It is saved as `requestTimeoutMs` in that
profile's settings. This bounds each helper request, including verification,
encryption and transfer; browser sign-in has a separate ten-minute deadline.
Changes apply to new operations. Explicit Retry may adopt a longer upload/reconciliation deadline
from the current profile only when the provider, account reference, account root
and folder still match. Its attempt budget is saved separately as
`operation.requestTimeoutMs`; the accepted destination, settings and receipt
identity remain unchanged. Lowering a profile deadline does not shorten an
already accepted retry budget. A deadline interruption
reports its reason and budget and preserves an unknown remote outcome.

Retries inspect the original publication before uploading again. Proton hides
unfinished drafts from completed-file listings. If the SDK explicitly reports
an unfinished upload collision and a complete listing proves no completed file
for that candidate, the configured rename policy can choose a new name without
removing the old draft. Unknown or incomplete listings remain unknown. Upload
and reconciliation requests use a stable client UID derived from the account
reference and publication ID, so subsequent attempts can recognize their own
new drafts. Drafts left by older builds are preserved.

History includes current activity through acquisition, preparation, upload, sharing and
clipboard results. Proton reports real SDK transferred bytes as encrypted
blocks finish, with no matching total; the current-operation row therefore shows bytes rather
than an estimated percentage. Counters can include overhead and decrease during
SDK retries. Other providers may report a matching total for percentage display.
The Lift tray uses the same byte observations and acquired source size for an
explicitly labelled estimate when there is no transfer total. This estimate
stops at 99% pending finalization/receipt; it does not change the SDK counters,
the activity row's accounting, or publication evidence.
A full byte count does not establish a completed file or a usable link: those
still require the confirmed receipt and sharing result. Notifications follow
the saved final result, independently of progress updates.

Shared results are Proton share-page URLs, including their complete access
fragment. They are not direct file/embedding URLs. URLs are sensitive: history
and explicitly requested clipboard/API results retain them; diagnostics and
helper errors do not include them.

History can create a link for a private upload, inspect uncertain sharing, or
revoke URL access without another upload. Revocation leaves the file, member
access and downloaded copies in place. Each sharing mutation has a durable
operation ID and intent before the helper starts. Interrupted results suppress
copied links and require inspection; startup does not repeat mutations. A
stale upload completion or retry cannot restore a revoked URL. If inspection
finds no link after an uncertain create, the original create intent and its
expiry/password references remain pending; a subsequent explicit retry creates
the link without reuploading. Inspection of a previously shared expired link or
an explicitly revoked link does not restore it. A failed journal commit stops
polling, clears any unconfirmed URL and permits explicit inspection after the
journal becomes writable.

Uploads are create-only. Name preflight uses the SDK's encrypted name-availability
query rather than decrypting every file in the destination. The SDK's atomic
name collision result supports rename or skip; overwrite is rejected. Exact completion node/revision IDs and an
authenticated encrypted publication/checksum marker establish ownership during
reconciliation. Missing requires a complete successful scoped listing; denial
is uncertain. Sharing checks the recorded account, folder, name, node, revision
and marker. SDK link mutations have no atomic revision precondition, so an
external revision change can race the mutation; a fresh post-check suppresses
an unconfirmed URL. Do not treat a link as an immutable revision URL.

Safe remote deletion is unavailable because the SDK does not expose atomic
revision-guarded deletion. The app rejects deletion of a recorded Proton file;
use link revocation or manage deletion in Proton. The helper never empties Trash.

All helper operations for the same keychain reference serialize across put.gd
processes, including token refresh. Each helper loads its own copy of the session,
and Proton rotates the refresh token when it is used. If two helpers refreshed
the same session, the one holding the stale token would be refused and the
official account code would sign the account out and remove it from the
keychain. Live, three parallel helpers finished three uploads 1.7–2.7 times
faster than one at a time, so concurrent Proton uploads remain a tracked design
item (#176) that needs refresh coordinated across helpers first. Helper requests use bounded stdin/stdout;
passwords and sessions never use argv. Each helper request keeps at most 64
SDK crypto-key entries in memory to avoid repeated parent-key decryption. Entity
metadata stays uncached, so identity/revision checks still read from the service.
The key cache is discarded with the helper process and is never persisted.
Timeouts/cancellation can leave an uncertain mutation, which the journal reconciles instead of hiding with another upload. SDK transport retries
are separate from application-level operation replay.

## Windows

The native Windows app ([windows.md](windows.md)) runs the same helper, cross-compiled
with `provision_runtime.py --target windows-x64`, next to its executable under
`proton\`. The session and link passwords live in Credential Manager (Bun.secrets);
the resident must run in the interactive session because Credential Manager refuses
writes from a non-interactive logon. Account coordination uses a lock file under
`%LOCALAPPDATA%\put.gd\proton-accounts`.

The Windows packages ship the helper (#408). CI's `windows-packages` job provisions it,
binds `putgd.exe` to its digest and packages it with its manifest, dependency notices and
a sixteen-file redistribution set in `proton\redistribution\`. That set holds the Bun
notice aggregate, the Windows scope and attribution (`BUN-WINDOWS-SCOPE.json`,
`BUN-WINDOWS-ATTRIBUTION.md`), ICU 78.3's licence, the JavaScript notices, and the
Windows corresponding-source catalog `SOURCES-WINDOWS.json` with `REBUILD-WINDOWS.md`.
The catalog pins the Windows helper and the `@oven/bun-windows-x64` package against the
same six published source packets as `SOURCES.json`. The Windows release gates are in
[the gates document](proton-drive-release-gates.md#windows-x64).

## Building and verification

See [the helper provisioning instructions](../tools/proton/README.md). The current
runtime lock supports Linux x86-64. Provision before configuring with
`cmake --preset dev -DPUTGD_BUNDLE_PROTON=ON`; use the ordinary build and test
presets. CMake checks the artifact, current helper sources, dependency lock, pinned
versions and notices against the manifest, then compiles its hash into the
adapter. Changing any of those inputs requires explicit reprovisioning. Installation uses a private libexec path, manifest and license inventory.
An app built without this bundle reports runtime unavailability on use.

Local contract tests use synthetic fixtures and prove shared pipeline/journal
behavior, not Proton service or desktop acceptance. The bundled helper's own
verification is in [VERIFICATION.md](../tools/proton/VERIFICATION.md). Live testing
requires an explicitly connected account and a new isolated synthetic test
folder. Never put a user's session or credentials into golden images, source
exports or evidence. For the tray-progress campaign, the user explicitly
authorized copying only put.gd's Proton session into disposable VM keychains.
Use the [private credential bridge](../tools/lab/secret_bridge/README.md), keep
rotating snapshots in host memory, serialize use of the copied session, and
carry refreshed state back under the production account lock. Remove only the
local lab keychain entry; signing out would invalidate the shared remote session.
See the [tray-progress replay](testing-tray-progress.md) for the isolated live
test path. Release acceptance requires Cinnamon functional/memory evidence
before expanding to GNOME/KDE.
Recovery, authenticated helper memory, redistribution notices, Proton-enabled
shipping assets and final-candidate acceptance remain open in
[the release-gate register](proton-drive-release-gates.md#open-release-gates).
Upstream approval is not required for the intended production shipment.
See [integration verification](proton-drive-verification.md) for the historical
build, test and review checkpoints.
