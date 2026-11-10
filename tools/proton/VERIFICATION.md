# SDK 0.23.0 / Crypto 2.2.0 / Bun 1.4.4 (#1071)

On d69, repeated builds of the current helper reproduce SHA-256
`9fd4d72f86352cfcf2c8b6ba1db4102ed6f7b22660077698c700989e9fda8586`.
All 46 Bun tests (319 assertions), eleven compiled protocol cases and the full
1,048,337-byte stdout regression pass. The compiler-backed source collector
independently reproduces that hash and selects 7,395 public source files.
The SDK capability adapter contributes 2,697 bytes; its type-only search input
contributes zero. No search engine, vendor WASM or BoltFFI runtime is embedded.
The helper source packet retains the installed vendor WASM and its SDK provenance
as an unbundled input; no preferred Rust-source or engine-rebuild claim is made.

The helper now uses the official SDK server-time response hook, with an isolated
real-Crypto regression test for header precedence, fallback and invalid values.
The SDK credential parser/store interfaces used by secret_bridge are unchanged;
no bridge API adaptation was needed. SDK 0.23.0 requires an explicit NodeType
argument to getAvailableName; both file preflight and quota-refusal reconciliation
now pass NodeType.File. The two argument-shape regressions fail against the old
call sites and pass with this adaptation. Full TypeScript checking now reports
only three upstream Crypto/WebStream declaration errors, with no helper or test
diagnostics; full type checking is not claimed as passing.

Under d69's Proton lock, one guarded live smoke passed existing-runtime and
private-broker checks before and after, list, new isolated test folder, synthetic
upload with exact receipt ownership, share-link creation, revoke and confirmed
private status. The synthetic file/folder remain; no existing object was changed.
The private receipt retains no session snapshot or capability URL. This is
headless helper evidence, not desktop or resource acceptance.

The native replay passed on d69: Bun Rust, JavaScriptCore C++ and TinyCC C
source markers execute from both the rebuilt native Bun and its compiled helper.
Both execute TinyCC C code; the 46 helper tests, eleven compiled protocol checks,
full stdout drain and modified Argon/OpenPGP interoperability fixture pass.
The production helper is unchanged. Native binary byte identity is not claimed.
The exact subject and receipts are in native_relink/ and notices/SOURCES.json. Earlier desktop attempts used the pre-upgrade helper;
no acceptance transfers to this SDK/Crypto build. Final desktop/resource gates
remain independently recorded in the catalog.

The records below are historical and do not establish current acceptance.

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
acceptance are recorded separately in `docs/history/proton-drive-verification.md`.


## Faster publishing: fewer requests, one helper per operation, started ahead (2026-11-10)

A 64 KiB upload with a share link took 15 to 20 seconds. Tracing every request
of the three helper calls showed why: 38 sequential requests to Proton, most of
them repeats. Each helper call re-read the account and folder one request at a
time, the SDK fetched each parent again because entity metadata used NullCache,
and the application hashed the 110 MB runtime (about 0.8 s with Qt's SHA-512)
before every helper start.

Helper changes:

- Node metadata has a second bounded cache. The helper's destination and
  ownership checks read past it in one batch (root, folder and file); the SDK
  reuses that batch for its own lookups.
- The user and address reads start together beside the first Drive read; the
  root share is read as soon as My files names it; a new link's SRP modulus is
  read beside the share's creation.
- Files under 128 KiB use the SDK's single-request upload.
- `--session` answers an operation's requests from one signed-in process. Its
  Drive clients share their caches and their My files and root share answers.
  An upload reuses the destination read of the name check just before it, and
  a name query runs beside the destination read once the folder's keys are in
  the process.
- `warm` signs a session in before its first request.

Application changes: the provider hears of an upload when its action is
invoked and starts a helper with `warm`; the operation's session takes that
helper; the adapter keeps a helper only while every answer is clean; the
runtime's hash verdict is kept per file and first computed in the background.
The helper process is no longer a QProcess, which belongs to one thread, but a
small pipe-and-spawn wrapper that the invoking thread can hand to the
operation's thread.

Requests for one 64 KiB publish: 38 before; 20 with a helper per request; 8
after the warm-up's 5.

Timed on d69 against the live test account with `proton_latency_probe`, the
production adapter and compiled helper. Each row is name check + upload + share
link, in alternating runs. "Nightly" is commit 8662a5fd1 with only the probe's
folder-name allowance applied. "Cold" starts the helper at the first request;
"started ahead" starts it 1.5 s before, as an action's invocation does while a
region is chosen.

| Payload | Nightly | New, cold | New, started ahead |
| --- | --- | --- | --- |
| 64 KiB | 14.5, 18.1, 18.4, 15.8 s | 5.8, 5.3, 3.6, 2.9 s | 2.0, 2.4, 2.4, 2.4 s |
| 1 MiB | 13.8, 19.1, 15.5 s | 9.0, 6.8, 6.4 s | 9.1, 4.4, 8.8 s; then 4.3, 4.4, 7.5, 4.9, 4.3 s |

Means: 64 KiB 16.7 s to 2.3 s (4.4 s cold); 1 MiB 16.1 s to 5.9 s over all
eight runs started ahead, 4.4 s in its five typical ones. Service and uplink
latency varied between runs, so single runs are not comparable; the
alternation is what makes the columns comparable. Above 128 KiB the SDK's block
upload is five sequential requests, and the 1 MiB block alone took 1.7 to 2.0 s
on d69's uplink.

What is left in a started-ahead 64 KiB publish: the name query (about 0.2 s),
one upload request (0.5 to 0.7 s, of which 0.15 s is key generation), and the
link (1.3 to 1.5 s): an ownership read, the share's creation, about 0.55 s of
Proton's own password hashing on d69 (two bcrypt hashes and a 65 MB iterated
key derivation), the link's creation and a second ownership read.

Every run ended `absent`, `uploaded`, `shared` and `revoked`. Sharing requires
the ownership check to read back the publication marker, so the single-request
upload stores it. 51 helper tests, 12 compiled stdio cases, 3 compiled session
cases and the stdout drain pass in provisioning; `proton_transport` (45 cases,
11 new for kept helpers and helpers started ahead), `proton_contract` (31) and
`test_operations` (122) pass on d69. Type checking reports only the three
upstream Crypto stream declaration errors.

The rebuilt helper is
`3a5167cbe44c854b53d10a40e1e91f5094dc2d1cf4f950213dcb970b21324d48`, reproduced
by two provisions on d69. `SOURCES.json` binds it and its overlay sources. The
native relink proof and the desktop/resource acceptance describe helper
`9fd4d72f8635`: both are open gates again, and their flags are off, until they
are repeated for this helper.

Not done, with reasons: Bun's experimental HTTP/2 client and pre-connect API;
running the link's password hashing on another thread; raising the SDK's
128 KiB single-request limit; and keeping a helper alive between operations,
which the idle memory gate rules out.
