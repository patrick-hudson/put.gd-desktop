# Implementation evidence

Evidence below records observed checks, not the remaining release scope. The full
contract is [issue #1](https://github.com/patrick-hudson/put.gd-desktop/issues/1).

| Unit / slice | Observed evidence | Remaining gate |
| --- | --- | --- |
| U1 bootstrap | Mint 22.3, GCC 13.3, Qt 6.4.2, QtKeychain 0.14.2; AWS 1.11.907 plus 15 pinned submodules built; Debug contracts and no-test Release build passed; ASan/UBSan contracts passed outside process sandbox | Authored CI jobs have not run on Ubuntu 26.04, Windows, or macOS |
| U2 shared services | Test-first service failures and returned/stored regressions; all 11 current executables pass Debug (16.47s) and ASan/UBSan with leak checking (37.67s); no-test Release builds; actual resident socket/CLI, asynchronous settings, manifest recovery, bounded acquisition/retry, opaque receipts/deletion, previews and 10,000-record history tested | Literal full-filesystem archive failure awaits the lab; GUI binding and production adapters belong to later units. All shared-service thread checks pass with instrumented dependencies; native adapters require separate acceptance |
| U3 S3 / secrets | Real pinned SDK and signed offline HTTP fixture; QtKeychain jobs through isolated D-Bus peer; full Debug 17/17 (37.00s), ASan/UBSan with leak detection 17/17 (67.81s), no-test Release build; adapter/secret-store TSan 5/5 (41.01s) | Live synthetic S3/public custom-domain smoke awaits explicit setup GUI; real desktop unlock/paste remain later gates |
| U4–U10 | Not implemented or verified yet | Native workflows, UI/history, real desktop lab, GNOME/KDE adapters, MCP, packaging and release evidence |

Configuration tests cover fresh setup without an implicit cloud profile, private atomic
writes, stale revisions, retained accepted settings after malformed/missing/oversized files,
repeated external replacements, actual periodic discovery when the parent directory was
initially missing, own-write notification deduplication, strict root types/keys/references,
cooperative lock contention, nullable patch values, and an unwritable parent directory.
All use temporary synthetic configuration; they access no clipboard, screen, cloud,
or secret store. Offscreen tests are not compositor acceptance evidence.

U2 operation tests use synthetic native/provider seams. They cover MIME precedence,
empty/inaccessible batch rejection before publication, content-based category routing,
atomic unique archives, archive retry without reupload, same-batch collision handling,
unknown publication/restart, original settings/destination snapshots, and clipboard
ownership/ticket arbitration. A committed private manifest recovers the file/SQL crash
window without publishing; abandoned staging is removed only after journal ownership
is acquired. A second resident cannot recover or remove the first resident's work.

History tests cover indexed pages/filters over 10,000 entries, transactional migration
and rollback, remote-management fields surviving operation updates, and cleared entries
staying cleared. Deletion uses original opaque receipts and durable step checkpoints.
The shutdown test drains the first sent DELETE and leaves the second target pending;
exactly one DELETE occurs. Real owner-only Unix-socket tests run outside the process
sandbox's socket restriction, with bounded versioned JSON and the built CLI executable.

Three simplification reviewers found four unique changes: reuse terminal-state checks,
avoid duplicate MIME reads and item serialization, and release unused object locks.
The lock-lifetime test first failed, then passed after replacing retained strong references
with weak references; active holders/waiters still share the same mutex. An additional
regression first accepted an own-publication receipt for a different destination;
reconciliation now retains unknown status and the original candidate instead.
The compiler build and whitespace check pass; no separate lint tool is configured.

U3 tests validate the actual signed HTTP requests, immutable file body, once-encoded
public URLs, storage class/ACL behavior, conditional writes, same-batch collisions,
credential revocation, bounded retries and unknown-publication reconciliation.
A lost-response regression exposed libcurl's transparent replay of empty-body DELETE
on reused connections despite disabled SDK retries. The supported SDK factory hook
now uses fresh, nonreusable connections for DELETE only; the real wire tests assert
one request and truthful unknown/partial outcomes. HEAD and PUT retain connection reuse.

Deletion covers unversioned conditional removal, immutable current/noncurrent versions,
protective markers, suspended null-version hazards, missing historical version IDs,
Object Lock/MFA/permission failures, checkpoint schema validation and cancellation.
A real SQLite-backed service/SDK fixture reopens the database before marker creation
and verifies the intent was durably committed. Unknown marker replies never authorize
another marker request; confirmed hiding with retained bytes remains partial.

The explicit synthetic destination probe uploads unique synthetic bytes, checks the
public URL without authentication, and removes only its recorded fixture receipt.
Its SDK HTTP response stream is bounded. Matching, wrong and oversized public bodies
are exercised; failed public checks still perform guarded fixture cleanup.
No live AWS request has run yet. Offline tests do not establish bucket permissions,
custom-domain behavior or secret-store behavior on a real graphical desktop.

The thread-check environment is reproducible in [thread-sanitizer.md](thread-sanitizer.md).
Per-process `setarch -R` resolves GCC 13's initial address-layout startup failure,
without changing host settings. Official Qt 6.4.2, GLib 2.80.0, libsecret 0.21.4,
D-Bus 1.14.10 and QtKeychain 0.14.2 are built with thread instrumentation using
normal CMake/Meson projects in ignored test prefixes. Production retains distro Qt.
Qt's upstream libxkbcommon compatibility patch is hash-verified and applied without fuzz.
Qt's supported fork backend retains all subprocess/IPC tests while avoiding GCC TSan's
internal clone/vfork startup assertion. GLib dispatch stays enabled for libsecret jobs.

A minimal queued-call reproduction passes with instrumented Qt. A dependency-only
D-Bus construction lock-order report is diagnosed and narrowly filtered at the
unpublished new-connection construction function; data-race detection stays enabled.
Committed opt-in reproductions return zero for queued calls and isolated D-Bus;
deliberately racy and reversed-lock controls both still report their expected defect
and exit 66 in the same final environment. There is no general Qt/application race filter.

Instrumented tests exposed Qt 6.4's HTTP connection-publication race, later fixed by
upstream [75d82afa](https://github.com/qt/qtbase/commit/75d82afa0d3aad9b4f9857e439535fc49c4616bc).
The application synthetic public probe now uses the SDK HTTP transport; fixture control
uses bounded private-loopback QTcpSocket requests. No upstream data race is suppressed
or patched into the production distribution. All five S3/collision/credential executables
pass TSan (41.01s); the remaining twelve contracts also pass (88.23s) with the same
instrumented dependencies. All 17 executables are covered across these two disjoint runs. The 10,000-row history dataset is retained, with a larger
TSan-only deadline for instrumented Qt/SQLite. Release SDK/OpenSSL/libcurl internals
are not instrumented, and neither sanitizer nor offscreen evidence certifies compositors.
