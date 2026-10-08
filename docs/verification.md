# Implementation evidence

Evidence below records observed checks, not the remaining release scope. The full
contract is [issue #1](https://github.com/patrick-hudson/put.gd-desktop/issues/1).

| Unit / slice | Observed evidence | Remaining gate |
| --- | --- | --- |
| U1 bootstrap | Mint 22.3, GCC 13.3, Qt 6.4.2, QtKeychain 0.14.2; AWS 1.11.907 plus 15 pinned submodules built; Debug contracts and no-test Release build passed; ASan/UBSan contracts passed outside process sandbox | Authored CI jobs have not run on Ubuntu 26.04, Windows, or macOS |
| U2 shared services | Test-first service failures and returned/stored regressions; all 11 current executables pass Debug (16.47s) and ASan/UBSan with leak checking (37.67s); no-test Release builds; actual resident socket/CLI, asynchronous settings, manifest recovery, bounded acquisition/retry, opaque receipts/deletion, previews and 10,000-record history tested | Literal full-filesystem archive failure awaits the lab; GUI binding and production adapters belong to later units. All shared-service thread checks pass with instrumented dependencies; native adapters require separate acceptance |
| U3 S3 / secrets | Real pinned SDK and signed offline HTTP fixture; QtKeychain jobs through isolated D-Bus peer; full Debug 17/17 (37.00s), ASan/UBSan with leak detection 17/17 (67.81s), no-test Release build; adapter/secret-store TSan 5/5 (41.01s) | Real Mint GUI secret-store setup passed; live test returned AccessDenied before publication. Successful live S3/custom-domain workflow remains a gate |
| U4 native foundation | Native XCB/EWMH/RandR capture, unfocused keys, external-focus-preserving selector, bounded asynchronous clipboard transfers and composition; full Debug 19/19 (40.69s). Final changed operations/geometry/native targets pass ASan/UBSan with leak detection 3/3 (11.25s) and TSan with instrumented Qt/X11 3/3 (12.47s). Four reviewed simplifications preserved those checks | Named Mint guest capture/clipboard/paste checks passed. Physical workflow, successful live S3, mixed-display acceptance and release budgets remain gates |
| U9 headless MCP slice | Locked official Python SDK stdio discovery and strict public argument schemas; 12 tools forwarding argument-vector production CLI calls; actual SDK stdio plus production CLI against a synthetic Unix peer: 3 tests pass | Actual resident/native effects, U6 graphical instructions, and a fresh-agent walkthrough remain required for full U9 acceptance |
| U5 settings / resident GUI | Real settings/service widgets, independently routed local folders, session/keychain form, history paging/deletion, copy-only retry, observed-revision conflicts and startup refresh; current Debug23/23 (44.21s). Affected7 memory/thread checks passed, then final shared-copy/core3/3 memory38.15s/thread90.02s; final GUI history6cases Debug2805ms/memory8851ms/thread27190ms. No-test Release builds | Actual Mint settings, OS keyring consent, notifications and independent native paste witnessed; login, remote history and custom R35 visuals remain gates |
| U6 lab / development package | Authenticated Mint22.3 ISO, real installed Cinnamon6.6.4/KVM guest, console-pinned SSH/public lab key; normal CPackDEB generated with distroQt/privateofficialAWS runtime; host lab6contracts pass. Foreground-process interruption was diagnosed and replaced with normal QEMUdaemon/pidfile ownership | Two independent disks and region/window/desktop/text/image/two-file native paste checks passed; mixed-display and timeout/crash evidence remain gates |
| U7/U8/U10 remaining delivery | Shared native/UI/lab interfaces are implemented; MCP headless scope remains separately tracked | GNOME/KDE adapters, complete MCP acceptance, physical performance and release lifecycle evidence |

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
The later real Mint GUI secret-store setup succeeded. The live destination test and
read-only official-SDK GetBucketLocation/synthetic-prefix listing returned AccessDenied.
The prepublication object check failed, so no synthetic object was uploaded or deleted.
Successful live publication/custom-domain/history deletion remains unverified; bucket
policy and account controls were not changed.

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


U4 regression evidence uses genuine X11 selection owners, separate client windows,
XTest key events and an isolated Xvfb server. It covers target identity frozen before
asynchronous acceptance, framed visible-window cropping, outward fractional rounding,
selection cancellation/focus and a held-key grab handoff, lock modifiers and actual
keymap rebinding, shifted punctuation, ordered clipboard targets and INCR, queue bounds,
frozen owners, stalled owners with responsive event loops, cancellation isolation and
preencoded PNG transfer. These are native protocol checks, not a Cinnamon support claim.
The full pre-cleanup memory run passed the other 18 targets; its native failure was
reproduced as a held-key fixture cleanup ordering bug. The fixture now waits for the
server to acknowledge simulated key releases before disconnecting; the strengthened
assertions and all affected final Debug/memory/thread checks pass.

Qt thread instrumentation includes the official XCB plugin. Its normal required
extension packages and reproduction commands are in thread-sanitizer.md. No new race
filter was introduced. Production continues to use distribution Qt. No physical host
capture, live AWS request or guest desktop acceptance has occurred in this slice.


U9 headless tests exercise the real official MCP SDK/client transport and built CLI
with a synthetic session peer. They verify discovery, strict types/extra-key rejection,
request correlation, configuration conflict and deletion mappings, unavailable/malformed
session responses and process timeouts. They do not replace native/resident upload or
remote-deletion evidence. The helper uses a lockfile, stdio only and no shell execution,
model call, secret-store unlock or app autostart; see [mcp.md](mcp.md).

U5 regressions include optional S3 public URL omission, generic-provider fields preserved
without invented S3 keys, draft conflicts and invalid external bytes, explicit destination
creation/removal, invalid reload status, startup history refresh, removed-link copy blocking
through both history and operations, and provider-neutral superseded receipt indexing.
The original receipt is retained. Schema3 backfill covers more than one bounded page.
History layout is checked at 1000px; instrumented Qt's wider font fallback exposed the
single-row controls. Measured minimum widths led to two-column actions/four-column
filters; the original assertion now passes in all three configurations.

The real Mint guest installation finished after an interrupted foreground QEMU process
was traced to host command cleanup. QEMU's standard daemon/pidfile lifecycle preserves
long guest work and the runner retains bounded owned-process/QMP cleanup. Mint's installer
runs once; ordinary test runs use independent qcow2 overlays and apt-installed packages.
The bootstrap script recognizes actual QEMU/KVM identifiers and grants apt privileges
only inside the disposable guest. The later Mint destination test returned a permission error before publication; no physical host capture has run.

The first actual Cinnamon snapshot now passes region and focused-window capture with
independent native image paste. Region output is 80×60; framed window output is
640×452; pasted PPM hashes equal the preserved captures. Fresh setup initially covered
the fixture and the oracle rejected the captured settings pixels. QMP witnesses then
identified timed key release keeping Cinnamon's window switcher active. Explicit key
down/up events and a visibility witness check fixed the lab input ordering. These runs
prove named single-head actions, not complete profile or release support. Additional
actions and fresh independent repetitions are still being checked.

The second independent Mint overlay passed region capture and all clipboard source
types. Text bytes match; the image and pasted image are 96×64 with identical PPM hashes;
two regular files preserve ordered bytes and paste the saved file URIs. The first
overlay also passed full-desktop capture at 1280×800. These remain single-head checks.
A real test mismatch exposed apt skipping a rebuilt unchanged-version package: guest
fixture bytes differed from the requested archive. The runner now uses normal apt
--reinstall and verifies installed application SHA256 values before recording success.
The fixture source chord uses Ctrl+Alt+Shift+T/I/F, avoiding Cinnamon's terminal shortcut.
Host lab contracts pass 6/6 (0.003s), including rejecting VM performance certification.

The user added R35/AE15 on 2026-10-08: the current functional forms are scaffolding for
a polished custom Qt visual design. Their contract tests do not satisfy the final
rendered visual review. The user also provided Ubuntu26.04.1 and Fedora44 installation
media. Both SHA256 values match good official checksum signatures, pinned in image-lock.
Their golden disks and disposable overlays are being provisioned; no Wayland acceptance
is claimed from checksum verification. credentials.txt is explicitly ignored and its
commit/export is prohibited in AGENTS.md; it remains absent from the Git index.

Actual zero-deadline lab readiness failure preserved JUnit/results/QMP witness and
stopped the owned second Mint QEMU process. The disposable disk and prior scenario
evidence remain available. The live capture operation reached partial: local PNG saved,
S3 prepublication check denied, no result copied. This is failure/recovery evidence,
not a successful S3/public-link test.
