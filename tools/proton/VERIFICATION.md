# Bun 1.4.4 update (#1054)

The current official Bun pin is 1.4.4. On d69 the helper reproduced SHA-256
4683e9fb28673df66a590660f2932f5b202fd61b1c229f49c61b8e8754113be4.
All 45 Bun tests, compiled protocol/large-stdout checks and a guarded live
read-only account/folder smoke passed, with existing-runtime checks before
and after under the Proton lock. These are helper checks. Desktop/resource
and current native Bun modification/relink acceptance remain separate gates.

The records below describe earlier builds and keep their historical scope.

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
offline provisioning reused them. The initial committed build and repeated identical offline
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

## Optional folder display name follow-up

The protocol now accepts an optional `folderName` display hint, bounded to a
nonempty string of at most 255 UTF-8 bytes without control characters. It does
not participate in folder identity. The new parser regression verifies legacy
absence, Unicode names, invalid values, receipt preservation and unchanged folder
UIDs. Offline provisioning passed 17 tests / 88 assertions and all existing
compiled stdio/stdout checks. Updated runtime SHA-256:
`9b147e7849474fef4fc99a4e38bc4a6758f10187e08ac56e8fe2936a91e86d68`.
The C++ compiled parser rejection also includes the optional hint and reaches
its intended no-account input validation error. Full type checking retains the
same four upstream errors; no helper diagnostics were reported.


## Request cache and name preflight follow-up

The helper now uses a 64-entry in-memory LRU crypto-key cache for one request;
entity metadata still uses NullCache. Key invalidation, request isolation, the
fixed bound, replacement/tags/enumeration and clearing are covered by tests.
The original NullCache failed the retention regression. The name preflight now
uses the supported SDK `getAvailableName` query. Original-name, occupied-name
and failed-query regressions failed against the previous folder-listing path,
then passed; failures remain inconclusive and upload creation remains atomic.

Offline provisioning passed 23 tests / 105 assertions, seven compiled stdio
cases and the complete stdout drain. Runtime SHA-256:
`146f1aa4c0e474dc76361385bd2125be1f2bf4857f1441742cccbc56bcb0e783`.
The source manifest and CMake guard now include `crypto-cache.ts`. Full type
checking retains the same four upstream stream declaration errors and no helper
source diagnostics. No configured lint command exists for this helper.

The integrating caller exercised live synthetic upload/create-link/revoke using
the actual C++ adapter in an isolated authenticated local folder. Two samples
per variant reduced mean preflight/upload/share readiness from 27.810 to 16.344
seconds. This later live evidence supplements the initial helper-only unit above;
it does not prove recipient download bytes, password/expiry recovery, total-process
memory or supported compositor acceptance. See the integration verification for
the per-stage timings, review and remaining gates.


## Upload progress and deadline follow-up

The new helper SHA-256 is
`0f8ef3ec922f7da98bcaae82d35a8b00e5c90775bab25a0d215ef9a8e65970fa`.
Its manifest now fingerprints `progress.ts` alongside the other helper sources.
Offline provisioning passed 27 tests / 117 assertions, seven compiled stdio
cases and the complete 1,048,337-byte stdout regression. Full TypeScript checking
retains the same four official Crypto/Bun stream declaration errors; no helper
or test source diagnostics appeared. No helper lint command is configured.

The regression tests observed missing SDK callback wiring in the old upload
body before the change. They now verify callback forwarding while waiting for
SDK completion to establish the receipt, total-free transfer counters, retry
rollback, 100,000 synchronous reports, slow stdout and final drain. The reporter
retains at most one in-flight write and one latest report; late/invalid counters
are ignored and write errors are surfaced. Deadline errors identify expiry and
its budget without exposing provider exception text or changing uncertainty.
These SDK-shaped tests are offline; authenticated progress evidence is recorded
separately in the integration verification document.

Review finding #2 added two controlled-clock tests for delivery before completion
and open slow-write rescheduling. Isolated disabled-scheduling and disabled-
rescheduling mutations failed the new assertions; restored source passed all
29 helper tests / 130 assertions. TypeScript still reports only the same four
upstream declaration errors. Only the test file changed; the verified production
helper SHA-256 and compiled stdio/full-stdout evidence above remain applicable.


## Extended deadline and retry errors

The maximum request budget is now 86,400,000 ms (default remains 120,000;
connect stays 600,000). An attempt override lives outside destination/receipt
identity. Publication-specific client IDs survive upload/reconciliation attempts
without storing authentication. Explicit unfinished-upload collision evidence
and deadline errors survive the status factory.

Pinned helper build SHA-256:
`68228b887f68b06d4aca742b36ba127863b30bd87bd6e9761bcb841df637f4b3`.
All 32 tests / 149 assertions passed; eleven compiled stdio cases exercise long
budget acceptance and out-of-range rejection without an account; complete
1,048,337-byte stdout drain passed. Four upstream Crypto/Bun type errors remain;
no helper/test type diagnostics remain. Live large-file recovery and release
acceptance are recorded separately in `docs/proton-drive-verification.md`.
