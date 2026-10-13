# Bundled helper verification — 2026-10-13

Implemented the production one-request SDK helper and its explicit provisioning
under `tools/proton/`. No CMake, core, GUI or index changes were made by this unit.
The prior standalone CLI provisioning draft was replaced. Account authentication
uses Proton's official pinned account foundation; uploads and sharing call the
official SDK directly. Credentials are accepted through owned keychain references
only. The app identity is `external-drive-put_gd@0.1.1-alpha`.

Built with:

```sh
python3 tools/proton/provision_runtime.py --source /tmp/putgd-proton-sdk
python3 tools/proton/provision_runtime.py --source /tmp/putgd-proton-sdk --offline
```

The first provisioning installed integrity-locked registry packages. Subsequent
offline provisioning reused them. The final build and repeated identical offline
build produced SHA-256
`81d8600d7af30973b9b7019c0ec823f1bc814364348e9b18679d8fdd4d913735`.
Artifact: `build/dependencies/proton-runtime/putgd-proton-runtime`.
Manifest: adjacent `manifest.json`. Exported notices: adjacent
`THIRD-PARTY-NOTICES.txt`, also retained in `tools/proton/notices/`.

The actual compiled binary reports Bun 1.4.2, SDK `js@0.22.2`, protocol 1 and
the accurate alpha app version. Provisioning runs 16 protocol and SDK-boundary
tests with 79 assertions, all passing, and the compiled version plus 7 stdio cases,
all passing. These cover exact completion identities, root/folder/name/revision
and encrypted marker ownership, changed identities, decryption uncertainty,
raw-secret rejection, receipt canonicalization, bounded strict UTF-8 input,
generic errors, and unsupported removal before keychain/network initialization.
No smoke case accesses a keychain, account, or Unix socket.
The separately compiled `stdout-fixture-test` uses the production async writer
and successfully captures all 1,048,337 synthetic output bytes before exit;
the fixture never ships as the runtime. This guards Bun's observed truncation
when exiting immediately after an unawaited stdout write.

Verification used the new helper's real parser/identity functions and real
compiled executable. There were no prior helper tests. Initial test additions
were validated after implementation; no initial red-before implementation result
is claimed.
During the safety follow-up, a realistic SDK-shaped signature regression failed
against the prior helper: decrypted metadata with `contentAuthor.ok:false` was
incorrectly classified as own. It now passes, including missing/failed key, name
and revision authorship. Ownership requires literal successful signature results
from all three authors. Further pure tests verify post-share ownership changes
suppress URLs without rollback, reconnect preserves old credentials, and SDK
Date-based expiration only marks unexpired link access usable.
Review finding #1 (P1), from the completed review at
`/tmp/compound-engineering-1000/ce-code-review/20261013-154708-d8d0f8ac`, exposed
that account-root comparison also required a selected folder. Three regression
tests failed before the fix: matching-root inspection, first-folder creation,
and the expected wrong-root rejection with an empty folder UID. The account
comparison now validates only the saved account-root UID. File operations still
require the full configured root/folder pair. Setup/stat functions are exported
for instrumented SDK-boundary tests; CLI execution is guarded by
`import.meta.main`. The tests verify top-level scope, matching/wrong roots, and
first-folder creation, plus stat lookup failures with a present UID, confirmed
absent UID, or a partial failing listing. These tests use SDK stubs to prove
branching and scope, not live account or graphical acceptance. The completed
code review was not rerun by this unit.
A separate Bun file-descriptor characterization confirmed that closing a
pinned file handle with an unconsumed Node-to-Web stream completes immediately.
It used a temporary synthetic file and no SDK/account operation.

Full `node node_modules/typescript/bin/tsc --noEmit --pretty false` in the
generated build-source tree reports four errors solely in the official Crypto
package: `pmcrypto/message/utils.ts`, `pmcrypto/utils.ts` (two), and
`proxy/endpoint/api.ts`. These are Bun/OpenPGP web-stream declaration
incompatibilities. No helper, protocol, account-adapter or upstream account
source diagnostics remain. Full TypeScript checking is not reported as passing.

Unverified acceptance: live browser login, account identity/folder operations,
upload/collision/reconciliation, link create/inspect/revoke and expiration or
password behavior. No live account authentication or mutation was attempted.
Real Cinnamon, GNOME, KDE, app memory and full application gates belong to the
integrating caller and were not run by this helper unit. Packaging and release
acceptance require a complete license review (including Bun embedded notices)
and Proton third-party/service readiness review.

Intentional capability limits: new files only; no replacement/revision upload;
safe removal unsupported because the SDK provides no atomic exact-revision
delete guard. File/link ownership checks use fresh server reads with NullCache,
but SDK sharing itself has no atomic revision compare guard: another client may
change a file after either check. Fresh checks run before and after sharing,
revocation or link inspection; changed or unknown ownership suppresses any URL.
Existing configured account roots cannot silently change during Connect or
inspection. Existing valid sessions are inspected, and an occupied invalid
reference requires a new reference rather than replacing prior credentials.
The helper never replays mutations or reuploads at the application level; the
official SDK retains its internal HTTP retries.
Uncertain outcomes require the caller's saved mutation checkpoint and explicit
reconciliation. Destination scope is MyFiles or its immediate child folder.
