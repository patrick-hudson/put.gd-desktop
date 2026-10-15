# Proton P1 recovery and recipient checks — 2026-10-15

At this service-only checkpoint P1 was **partially verified and remained open**.
The later [normal-use application follow-up](2026-10-15-proton-p1-app-recovery.md)
completes P1 within its recorded scope; the historical service limits below remain
unchanged. The earlier recipient checkbox
could not be backed by a recoverable receipt: the original integration chat's
last acceptance report explicitly left recipient download/password/expiry
pending, and its temporary artifacts were unavailable. This run supplies a
fresh bounded receipt; it does not backdate that evidence.

## Candidate and scope

Based on main `50fd37cafafcc4611925642b93dccb9d9f0949ab`, with the recovery
changes fingerprinted in [the numeric receipt](2026-10-15-proton-p1-recovery.json).
The live run used a dirty development candidate; source fingerprints distinguish
it from the base commit. SDK 0.22.2, Crypto 2.0.0, Bun 1.4.2 and upstream commit
`8ffae007b0fbb8bcef53514719d6f093bb32dcf9` remain pinned. Final helper SHA-256:
`93fc77e63913dd39075a7ede049289325fb7c16a3aaf20985bc62adbc25b0e99`.
Recipient/cancellation/refresh checks used the preceding helper
`ae39bf92135739e320711f69b7f7471acdbc368395966ea6ebbb263823573661`;
the final helper additionally fixes the revoked-refresh failure and was retested
against a separately revoked live session.

The opt-in, non-installed `proton_recovery_probe` uses the production C++ adapter,
bundled helper, official SDK and OS keychain. It records private intents before
mutations and sharing checkpoints before dispatch. It accepts private pipe I/O,
owned private output directories and isolated prefixed destinations/items; it
has no remote deletion operation. It is a service probe, not the application's
GUI, operation journal, clipboard or desktop acceptance campaign.

All payloads were synthetic, in one newly created isolated folder. Fresh
browser-authenticated SDK sessions were verified distinct from the everyday app
session before revocation. Revocation targeted only each test session through the
official account API's current-session endpoint. The everyday session still
passed read-only account inspection afterward. Initial recipient test links were revoked and their
owned local test keychain references removed. The additional viewer fixture uses
a three-minute automatic expiration; read-only inspection confirmed expiry
and that its private folder remains. Its independent login was revoked
(HTTP 200) and its local test keychain entry removed. Its cleanup does not
remove an object. Private remote synthetic
files remain because exact guarded deletion is unsupported. No user objects,
configuration or bucket settings were changed.

## Live outcomes

| Check | Observed result |
| --- | --- |
| Anonymous public recipient | Chrome sign-in page witnessed after signing out the current website session, then unsigned-in share opened and downloaded. Source and downloaded file: 65,536 bytes, SHA-256 `7daca2095d0438260fa849183dfc67faa459fdf4936e1bc91eec6b281b27e4c2`. |
| Password protection | Missing password left Continue disabled; wrong password stayed at the password gate. Correct synthetic password opened the protected file; its actual downloaded 65,536 bytes had the same SHA-256. No account login was used for recipient access. |
| Expiration | Both product-created links requested one-day expiration. To observe expiry within this run, a separate official-SDK fixture control shortened only the owned public test link to 60 seconds. After elapsed expiry, production inspection returned `private`, preserved receipt identity and no URL; the old link displayed “Shared link not found”. This verifies live expiration enforcement, not a day-long wall-clock wait or a changed product range. |
| Revocation | Production revocation returned `revoked` for both owned links; the protected recipient URL subsequently displayed “Shared link not found”. |
| Access-token refresh | Invalidated only a disposable session's access token while retaining its refresh token. Production stat succeeded as `own` through real account refresh, preserving the receipt. This forces access rejection; it is not natural token aging. |
| Cancel, reconcile, retry | Cancelled a 32 MiB upload after a real nonzero SDK progress frame. Adapter returned `unknown` / `proton_result_unknown`. Explicit scoped reconciliation completed and returned `missing`; only then an explicit retry returned `uploaded`, with the same publication, destination, candidate and checksum. No confirmed upload was duplicated. |
| Changed remote revision | A fixture control created a new revision of the owned test file using identical synthetic source bytes. Creating a share with its old receipt returned `changed`, retained old object identity and exposed no URL. Controlled tests additionally cover a revision changing during share creation. |
| Quota, production adapter | A 6 GiB sparse synthetic source exceeded demo-account capacity. Production upload returned `unknown` / `quota_exceeded` after 27.985 seconds, with no URL. Explicit reconciliation completed as `missing` in 2.770 seconds. The SDK sends intended size for early server quota validation, so filling the account was unnecessary. |
| Permission denial, live SDK | An anonymous public-link client opened the owned fixture folder as Viewer. Creating a child folder was rejected by the live service: HTTP 422, typed SDK `ValidationError` code `2011`, classified by the production helper function as `permission_denied`. This verifies a real SDK/service permission refusal; it does not establish the full application path for a second authenticated account losing folder access. |
| Bounded rate-limit attempt | Ten wrong-password authentications against only the owned synthetic share returned HTTP 422; no HTTP 429 occurred. No account-password attack or unrelated endpoint was used. The proposed extension was stopped after recognizing that authentication-abuse limiting does not represent the application upload/share path. A live 429 is not required to be manufactured for acceptance. |
| Revoked refresh, final helper | Current-session revocation returned HTTP 200. Subsequent production stat and share both returned `unknown` / `authentication_required`, with no link. Original private intents/receipts remained available for explicit recovery. Everyday app session remained usable. |

No capability URL, account/node/session identity, password, token or private
journal is included in the committed receipt. Local private artifacts are under
`/tmp/p1r1` (0700); they are temporary investigation material, not a durable
public replay bundle.

## Defects fixed and controlled checks

Previously, name checks swallowed service errors and uploads/sharing collapsed
permission, quota and rate-limit responses into generic uncertainty. The helper
now classifies only typed official SDK/account errors into static safe guidance.
Statuses remain inconclusive/unknown where absence or completion is unproved;
raw service messages, bodies, URLs and debug fields are excluded. No automatic
application retry was added.

The first real revoked-session run uncovered another defect: rejected refresh
cleared credentials, while the SDK surfaced a generic server error. Recovery
reported `operation_unknown` without reconnect guidance. A regression test first
failed, then the helper was changed to notice the cleared authentication state.
The final live repeat above verifies the fix.

Controlled tests use typed SDK errors and the **unmodified official account
ApiClient** with bounded synthetic fetch responses. They verify permission,
quota and rate-limit guidance; no old URL after sharing errors/denied postchecks;
revision races; unchanged protection options; expired inspection without a new
share mutation; successful refresh; revoked refresh clearing credentials; and
refresh 429 retaining credentials. Exact request counts distinguish SDK refresh
from application replay. The later live quota and viewer-permission checks are credited above. A new
test also exercises the actual pinned SDK `DriveAPIService`: two consecutive
429 responses with `Retry-After: 1` produce three requests separated by at least
950 ms each before successful recovery. Cancellation after a 429 prevents the
next request. This measures the two-delay/cancellation paths, not the SDK's
50-consecutive-429 cooldown threshold or a naturally occurring live 429. For this case, the bounded SDK tests are
the acceptance evidence; lack of a manufactured live 429 is a recorded limit,
not a separate release blocker.

## Builds, tests and retained limits

- Offline pinned provisioning passed **41 tests / 253 assertions**, 11 compiled
  no-account stdio cases and the 1,048,337-byte compiled stdout regression.
- Standard `cmake --preset dev` and `cmake --build --preset dev` passed with
  `PUTGD_BUNDLE_PROTON=ON`; the opt-in probe target also built.
- Full `TMPDIR=/tmp/p1r1/t ctest --preset dev` passed **59/59**, 500.29 seconds,
  before the final revoked-refresh helper fix. After that fix and comment review,
  both Proton contract/transport tests passed again (**2/2**, 14.79 seconds).
- Full TypeScript checking still reports exactly four upstream Crypto/Bun stream
  diagnostics; no helper-source diagnostics remain. It is **not** a passing check.
- Socket preflight covered 59 resolved paths, maximum 77 UTF-8 bytes, below 107.
  The probe creates no put.gd session socket; the keychain D-Bus path was checked.
- Reuse, quality, efficiency, test-value and comment reviews completed. Removed
  three redundant added comments. Targeted correctness/security inspection checked
  typed-error provenance, uncertainty, URL suppression, exact receipts and fixture
  boundaries. No separate automated code-review service receipt is claimed.

Retained unsuccessful attempts: the first socket-preflight invocation combined
exclusive options and was corrected before launch; a full build was interrupted
to prioritize focused targets and subsequently resumed successfully. Chrome's
download-event observer timed out despite completed downloads, so actual completed
files were independently hashed. Browser password reads substituted `<redacted>`;
that tooling transfer was corrected using only the generated synthetic password.
A second login relay initially lacked sandbox socket permission; its failed
attempt was preserved and a fresh output root used. Final controller module
resolution and a metadata-key error were corrected; the latter occurred after
the checks, whose private journals were recovered without repeating mutations. The new
anonymous fixture initially omitted app/session prerequisites and used `password`
instead of the SDK `customPassword` option; those failed fixture attempts were
retained, then corrected before crediting viewer-write evidence. Production uses
the correct option. One new test initially lacked a result type annotation; it
was corrected and the four-error upstream TypeScript baseline re-established.

Automatic review rejected the initial fixture cleanup because it classified
`unshareNode` as object deletion. The safer executed fixture uses automatic
three-minute expiry and contains no deletion/unshare call. A later request for
30 additional wrong-password attempts was rejected as a brute-force-like probe;
it was not executed. The user questioned its relevance to normal application
behavior, and the probe was stopped. No additional authorization is pending.

Still open in P1: authenticated folder-access loss in the complete application,
and final application failure/clipboard/restart acceptance. The SDK's extended
consecutive-429 cooldown remains source-inspected rather than timing-verified.
Do not treat the real viewer denial as second-account GUI acceptance. P2–P6
remain open; no new native Cinnamon/GNOME/KDE or authenticated memory acceptance
is established here. After the user's explicit Proton-only restriction, all
additional service traffic was restricted to `drive-api.proton.me`; no further
S3 tests or S3 account/bucket access were performed. See
[the gate register](../proton-drive-release-gates.md).
