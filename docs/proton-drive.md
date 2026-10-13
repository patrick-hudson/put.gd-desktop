# Proton Drive storage

Proton Drive is a provider in the shared upload, retry and history pipeline. The
Qt app uses a privately packaged executable built from the official Drive SDK
and the pinned official account/CLI sources. Users do not install a CLI, Bun or
Node.js. This is a third-party integration, not officially supported by Proton.

The unmodified official CLI cannot supply the upload receipt this application
needs: its pinned upload command discards completion node/revision IDs and
prints aggregate transfer counters. Its sharing password is an argv option.
The private helper therefore calls the same official SDK directly and retains
the official CLI/account foundation for authentication. It does not reimplement
Proton cryptography. This replaces the initial CLI-only candidate.

In Settings, add a destination, choose Proton Drive and save it. Connect the
account through the browser. Authentication stays in the desktop secret service
under put.gd's own namespace; it does not use the official Drive CLI's session.
Choose a top-level folder in My files or explicitly create a new put.gd folder,
then save the updated destination. Account setup never uploads a file.

Sharing is the destination default. Clear that choice for private uploads, or
use Advanced settings to override publication per app action. Private upload
creates no public link and leaves the clipboard unchanged. Existing folder and
member access still applies. Shortcuts remain global app/action settings.

Advanced settings set new-link expiration (0–365 days; 0 means no expiration)
and an optional password. Store the password explicitly in the secret service;
configuration and receipts contain only its reference. Use no link password
clears that reference for future links. Password entry is excluded from retained
UI state and CLI arguments. Old receipts keep their original account, folder,
credential and sharing settings when destinations are edited.

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

Uploads are create-only. The SDK's atomic name collision result supports rename
or skip; overwrite is rejected. Exact completion node/revision IDs and an
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
processes, including token refresh. Helper requests use bounded stdin/stdout;
passwords and sessions never use argv. Fresh per-request SDK caches are not
persisted. Timeouts/cancellation can leave an uncertain mutation, which the
journal reconciles instead of hiding with another upload. SDK transport retries
are separate from application-level operation replay.

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
folder. Never move a user's session or credentials into VM images or evidence.
Local authentication testing is the agreed first step; release acceptance still
requires Cinnamon functional/memory evidence before expanding to GNOME/KDE.
Complete redistribution notices and service readiness remain release gates.
See [integration verification](proton-drive-verification.md) for the actual
build, test, review and remaining acceptance record.
