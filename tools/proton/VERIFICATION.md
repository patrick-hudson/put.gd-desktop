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

## Faster publishing: a chosen API connection, link work beside the transfer (2026-11-11)

A started-ahead 64 KiB publish took about 2.3 s on a good run after the change
above, and 4 to 5 s on the lab desktops. Tracing about 150 live publishes on
d69 showed three things left.

**Proton answers each connection from one of several backends, and stays with
it.** On 24 new connections, the second read of `/core/v4/addresses` took
161 to 183 ms on six, 327 to 389 ms on six and 545 to 704 ms on twelve. The
network round trip (120 to 136 ms by HTTP/2 ping) and the unauthenticated
`/tests/ping` (about 140 ms) were the same on all of them, so the difference
is the service's own time. Ten name checks in one helper stayed in one band
for all ten: 178 to 197 ms in one process, 350 to 479 ms in another. Returning
the service's `Session-Id` and `Tag` cookies did not pin new connections to a
backend. The compiled `fetch` pools connections and gives no say in which one
carries a request, so a publish's requests in sequence were as slow as the
connections they happened to get. Bun 1.4.4's `fetch.preconnect` throws
`Invalid port` for every address form tried.

**The link's local work ran with nothing in transit.** After the share's
creation the SDK spent about 0.48 s before it could send the link's request:
two bcrypt hashes (about 0.1 s each), the SRP modulus read, and the share's
session key wrapped under the password by an iterated hash of 65 MB (0.25 s).

**A file over 128 KiB opens a new connection to a storage host when its bytes
are ready.** The single-request limit is the service's: a 360 KB file sent to
`/files/small` with the SDK's limit raised was refused with 422.

Helper changes:

- The first GET to the API is sent on six new HTTP/2 connections; the one that
  answers first carries every later API request, and the others are closed. A
  first answer more than 0.1 s beyond the connection's ping starts a second
  round of six in the background. The ordinary client takes over for the rest
  of the process when no candidate connects or answers in four seconds, when
  the first answer is not a 200 or a 401, when a proxy is configured, or when
  any request on the kept connection fails. A failed GET and a request the
  service did not act on (a refused stream, or one opened after the last
  stream named in a shutdown) are sent again on the ordinary client. Bun 1.4.4
  reports a reset stream's code as 0, so the refused-stream case is covered by
  a scripted connection in the tests, not by a live reset.
- An upload told that a link follows hashes the link's password and reads the
  modulus while its bytes are in transit; the session key is wrapped while the
  share's request is in transit. The password is the SDK's own generator's,
  called ahead through `sharing.management.cryptoService`; an SDK without it
  there makes links as before, and a test fails provisioning.
- An upload's answer names its storage host, and an upload request may name
  one, which the helper connects to while the draft is made (for a file of one
  block only, see below). Of 36 block
  uploads 26 went to `fra-storage` and 10 to `zrh-storage`; a hint that named
  the other host cost nothing. With the right host and the connection ready
  60 ms or more before the block, the block's request took 1.10 to 1.22 s in
  eight of nine runs (1.85 s in the ninth) instead of 1.42 to 1.56 s. A HEAD
  does not leave a reusable connection in the pool; a GET read to its end does.
- A token refresh no longer makes the account code read the user and the
  addresses a second time (two requests in sequence, 1.2 s in the trace that
  showed it). This is covered by a test against the pinned `Addresses` class;
  no later run began with an expired token.

Application changes: an upload request says whether the operation's link
follows; the provider keeps each account's last storage host in memory and
names it in the next upload; and an operation invoked while another's session
has the account takes over that session's helper when it ends, instead of
starting one.

Tried and not kept: skipping the ownership read before the link when this
helper had just uploaded the file. The SDK then reads the file itself, so no
request was saved, and the read stayed.

Timed on d69 against the live test account, in alternating runs of the nightly
helper (commit 4f80bcfff, `3a5167cbe44c`) and this one through one driver, each
started 1.5 s ahead. Seconds from the name check's request to the link's
answer; every one of the 32 runs ended `absent`, `uploaded`, `shared` and
`revoked`.

| Payload | Nightly, mean (lowest to highest), n=8 | This change, mean (lowest to highest), n=8 |
| --- | --- | --- |
| 64 KiB | 2.66 s (2.04 to 4.19) | 1.73 s (1.65 to 2.01) |
| 360 KB | 5.30 s (3.96 to 6.77) | 3.09 s (3.00 to 3.26) |

| Stage, mean | 64 KiB nightly | 64 KiB now | 360 KB nightly | 360 KB now |
| --- | --- | --- | --- | --- |
| Name check | 0.27 s | 0.19 s | 0.31 s | 0.19 s |
| Upload | 0.74 s | 0.64 s | 3.09 s | 1.98 s |
| Link | 1.64 s | 0.91 s | 1.89 s | 0.92 s |

The 360 KB runs named `fra-storage` as the last storage host, as an
application's second upload would. Through the production adapter and the
compiled helper (`proton_latency_probe`, no storage host known), three 64 KiB
publishes took 1.71, 1.75 and 1.72 s and two 360 KB ones 3.41 and 3.41 s.

Before the connection was chosen, the link work and the storage connection
alone gave 2.77 s to 2.62 s (64 KiB, n=15) and 4.82 s to 4.52 s (360 KB, n=12)
on the same account: the requests' own time on slow connections swamped them.
Adding a 0.48 s idle wait before the link's request did not make that request
faster (n=8 each), so the service does not need the pause the local work used
to make.

What is left in a started-ahead publish: one round trip for the name check,
one request or five for the upload, and four in sequence for the link (the
ownership read, the share, the link and the second ownership read), each about
0.18 to 0.26 s on a fast connection from this lab.

75 helper tests, 12 compiled stdio cases, 3 compiled session cases and the
stdout drain pass in provisioning; `proton_transport` (54 cases: upload hints
and nine for a session's helper passing to the next operation), `proton_contract`
(31) and `test_operations` (122) pass on d69. Type checking reports only the
three upstream Crypto stream declaration errors.

The timings above were taken with helper `c99f3c9fb468`. Three changes
followed them.

From review: a link preparation belongs to its account and is ended by
cleanup, so one that has not started, or is still drawing its password, is
never hashed or kept. A request for another origin is handed to the ordinary
client as it came, without a copy being made of it.

From the first desktop acceptance of this change
([record](https://github.com/patrick-hudson/put.gd-evidence/blob/c0a3ff1cb04bc6dd96a7242dcd1094775fcc58e5/records/2026-11-12-proton-chosen-connection-acceptance/README.md),
helper `ed95dc4a6d20`), which passed its Proton cases and found two faults:

- Two uploads invoked one right after the other still started two helpers on
  all four desktops. The second asked while the first's helper, started ahead,
  had not been taken yet, and that case registered nothing. It registers now
  (a test for it was removed with the helper reuse cases in #1422).
- 8 MB uploads took 7.6 to 12.9 s in their upload stage, against 5.0 to 6.2 s
  in the previous record. The storage connection opened early was the cause
  for a file of several blocks: its blocks are sent side by side, the block on
  the opened connection started ahead, and the late one crawled. With a storage
  host named, three 8 MiB uploads took 8.6, 9.9 and 10.7 s; without, five took
  4.5 to 6.6 s (the nightly helper in the same minutes: 6.4 to 12.2 s). The
  connection is now opened only for a file of one block. With the final helper
  and a host named, three 8 MiB uploads took 5.2, 8.4 and 5.2 s and opened
  nothing early.

That record's three screenshots in a row showed the early connection helping
on d69 (upload stage 2.10, 1.93, 1.87 s) and not on k72 or m74 (about 2.0 s
each time): it needs the same storage host twice and about half a second
before the block is ready, and neither is certain.

The second desktop acceptance
([record](https://github.com/patrick-hudson/put.gd-evidence/tree/4ac749f86dc322bb91023ad18b31d5c423d1c293/records/2026-11-12-proton-chosen-connection-acceptance-round2),
helper `e1237d260f4c`) showed both faults gone: one helper for two uploads
invoked together on all four desktops, and 8 MB totals of 8.2 to 8.9 s. None of
24 screenshots was black; what made one black in the first run was not
established, and this change does not touch capture. It left two questions.

- **One link ended `unknown`.** On k72, one screenshot of 63 publishes was
  uploaded and then got no link: the ownership read 0.4 s after its upload came
  back as not known to be its own, with no error, and the same read minutes
  later said it was. On d69 the read was `own` every time in 80 uploads read
  back at once on the same connection, and in 72 uploads read back on another
  connection (two fast, three medium, one slow). It was not reproduced and its
  cause is not established. Two changes follow from it, neither shown to be its
  cure. A connection is no longer replaced by a faster one once a request is
  about to change something, so a change and the reads after it reach one
  backend. And a file this process uploaded within thirty seconds, read back
  incomplete with no error, is read again up to three times, 0.3 s apart, before
  that is believed.
- **8 MiB against nightly in the same hour.** The acceptance run's single
  samples, this change first and nightly straight after, had this change's
  upload stage 0.2 to 1.1 s longer on three desktops and its totals 0.3 s
  shorter to 0.9 s longer. Order-balanced on d69 (new, nightly, nightly, new,
  twice): this change 4.5, 4.7, 6.5 and 11.6 s, nightly 6.2, 8.3, 9.9 and
  11.2 s. With the five alternating pairs above, nothing shows this change
  slower for large files; one upload's time here varies by more than the
  difference.

The third desktop acceptance
([record](https://github.com/patrick-hudson/put.gd-evidence/tree/0e94369f0480c65c990387496e85666809a30685/records/2026-11-12-proton-chosen-connection-acceptance-round3),
helper `42e1decb6d71`) had every link end `shared` (64 of 64), no black
capture in 24, and 15 of 15 cases on d69, k72 and m74 (9 of 15 on f44, whose
six capture cases need the GNOME extension an unpacked AppImage lacks). Its
order-balanced large-file samples had this change's 8 MiB totals at 8.2 to
10.5 s and nightly's at 9.6 to 17.7 s. It found one more fault: on k72 two
uploads invoked 31 ms apart used two helpers. The second invoked got the
account first and took the helper the first had started; when it ended, only
its own registration was left, because the first had withdrawn its
registration on starting a helper, so the helper was retired and the first
operation started another. Every operation now stays registered for as long
as it lives. Two tests, one with a row for each order and one running two
operations on threads ten times, failed on the code before this fix and passed
with it. #1422 removed both with the other helper reuse cases;
`test_proton_transport.cpp` now keeps `sessionKeepsOneHelperUntilItIsReleased`
and `accountBusyNeverDispatches`, which hold one helper to one session and
refuse a second request on the same account. This change is in the
application only: the helper is the same.

Through the production adapter and the final compiled helper, two 64 KiB
publishes took 1.76 and 1.76 s, two 360 KB publishes 3.44 and 3.52 s and one
8 MiB publish 5.71 s.

The rebuilt helper is
`42e1decb6d718e2640f0a419b3d135a3755f65a27e66682126ed16a2f6581065`, reproduced
by two provisions on d69. `SOURCES.json` binds it and its overlay sources. The
native relink proof and the desktop/resource acceptance still describe helper
`9fd4d72f8635` and remain open gates (#1399).

Not done: a publish that begins with an expired token.

## A keychain exchange that goes wrong for one process (#1418, 2026-11-12)

On k72 (Kubuntu 26.04, secret service `ksecretd`), with Bun 1.4.4 and nothing of
put.gd involved, 700 new processes each read the lab session's entry once: 692
got its value, 5 were refused with `Client public key size is invalid (code:
16)`, and 3 were answered with nothing. From one long-lived process the same
entry was read 400 times without a miss. In the application these were the
helper's `keychain_unavailable` (seen once with nightly) and
`authentication_required` for a connected account: 2 of 93 link revocations with
#1414's build and 1 of 93 with nightly failed that way, each within 0.32 s
([record](https://github.com/patrick-hudson/put.gd-evidence/tree/dbcfca8d16ac7ebf61191dc6314e8138f2eaf07f/records/2026-11-12-proton-k72-revoke-reliability)).

Reading again in the same process, up to four times, turned every refusal into
a value on the second attempt (5 of 5) and no empty answer into one (3 of 3
stayed empty). So:

- A refused read or write is made again in the helper up to three times, 30,
  90 and 240 ms apart. That covers the write after a token refresh, where a
  session not saved is a session lost.
- A helper that finds no session answers `session_unread` and changes nothing.
  The application asks once more through a new helper, with
  `sessionRetry:true`, and that helper believes what it finds. The compiled
  helper, asked about a reference that was never connected, answers
  `session_unread` and then, told so, `authentication_required` as before.

77 helper tests pass in provisioning with the 12 stdio and 3 session cases;
`proton_transport` (58 cases, four for the replaced helper), `proton_contract`
(31) and `test_operations` (122) pass on d69. Through the production adapter
and the compiled helper, two 64 KiB publishes took 1.72 and 1.75 s.

The rebuilt helper is
`d4bea9c8a1cdaffa6ae4efc7489bb6eb5c70720277950fe6999e6ba466b4555e`, reproduced
by two provisions on d69. `SOURCES.json` binds it and its overlay sources; the
native relink proof and the desktop/resource acceptance remain open gates
(#1399).

Not shown here: the failure rate in the application with this helper. Two
fresh helpers both answered with nothing would be about one start in 65,000 at
the rate measured.
