# Live S3 transport retest: source108

Measured source: `b4e16ba9971c2cd943c6fb0ec39d6ac421685a8a`, tree
`ba1b5ccff5967d9e5493b27e6b5788305f66b441`, generated version
`0.1.1-dev.108+gb4e16ba9971c`. This documentation does not rename that build.

The production failures reproduced on GNOME and KDE with an actionable numeric
signature: cURL 35 (`SSL_CONNECT_ERROR`), SDK 99 (`NETWORK_CONNECTION`), HTTP−1,
and `sdkRetryable=true`. The underlying TLS reason remains unknown. No automatic
retry or transport behavior change was made. The SDK retryability flag is
informational; it does not authorize replaying an uncertain mutation.

| Desktop | Native / GUI | Live upload | Fixed idle memory | Production deletion |
| --- | --- | --- | --- | --- |
| Cinnamon/X11 | 12 / 8 passed | 30 operations, 31 objects, all six sources passed | 28.90625 MiB RSS+swap; 0 swap; 0.0100% one core | 31 removed |
| GNOME/Wayland | 12 / 8 passed | Stopped at first SDK failure: 13 accepted operations, 12 successful objects | No complete workload/quiet acceptance | 11 removed, 1 unknown |
| KDE/Wayland | 12 / 8 passed | 30 operations, 31 objects, all six sources passed | Numeric attribution31.37890625 MiB; 0 swap; 0.01333% one core | 23 removed, 8 unknown |

Cinnamon passed the current-build functional and memory gate before sibling
expansion. GNOME and KDE used standard native CMake/Ninja builds, distro Qt and
the official pinned AWS SDK. Their dev suites passed58/58 and57/57 respectively;
package audits and exact installed/running identity proofs passed. All graphical
checks stayed visible on VNC5936/5934/5938 through the display wall.

This is a diagnostic retest with one source108 Cinnamon gate, not a new
optimization KEEP decision or proof of the fresh-profile repeat required by
the full optimization protocol. Earlier measured candidates retain their own
receipts; the failing Wayland gates remain open.

## Actual failures

GNOME operation `408de35d-099d-4573-83bb-7a853336aa37` failed during `name_head`,
148 ms, before any PUT. Its region artifact exists, but publication was
`not_requested`. Production cleanup of its12 successful objects then had one
`object_head` failure,138 ms, History ID
`5d66b713-11bb-4c8a-a2ec-79e89ebdb09d`.

KDE deletion operation `12a94c17-002b-4e78-9136-06fe25ae99b4` settled with23
removed and8 unknown. Five failed at `object_head` and three at `delete_object`,
108–154 ms. Three mutation targets retained `object_unknown` checkpoints.
The first failed History ID was `1b13aacd-31f4-49f4-b88c-ff2e58406a7c`,
`object_head`,137 ms. The bulk operation had already been accepted when the first
failure became observable; no new operation or automatic replay followed it.

The SDK's `CurlHttpClient.cpp:854–875` sets an error buffer, executes
`curl_easy_perform`, and maps a nonzero cURL result to `NETWORK_CONNECTION` when
the request continuation permits it. The adapter maps that result through
`src/storage/s3/S3Driver.cpp:45` into bounded numeric details. Name checking at
line 235 becomes inconclusive; object inspection at line 144 becomes unknown;
conditional deletion at line 605 preserves an unknown result. This explains the
observed app outcomes, but not the cause of the TLS handshake failure. Curl 35
does not establish a cancellation, certificate-verification failure, credential
failure or any particular OpenSSL defect.

Actual package-version differences are recorded separately: Cinnamon's
Curl 8.5 / OpenSSL 3.0 / Qt 6.4 versus newer GNOME/KDE libraries. They are investigation
inputs, not proof of cause. The safe logger intentionally excludes raw SDK
messages, headers, endpoints, payloads and credentials. Therefore it cannot
recover the TLS error-buffer reason from these completed requests.

## Acceptance and cleanup

Both Wayland live-S3 gates remain blocked. KDE's complete fixed300-second
memory window is valid numeric cost attribution, but its frozen evaluator still
requires successful production deletion. No `cleanup-completed.json` or passing
evaluation was manufactured. GNOME's drained475 observer samples came from a
partial workload and establish no full quiet-window acceptance.

Exact authenticated fallback cleanup removed KDE's eight unknown objects and
GNOME's one unknown object after matching publication metadata, SHA256, size, ETag
and version, followed by an IfMatch delete and authenticated404. The app's
unknown History/journal states were preserved. This fallback does not count as
production deletion acceptance. All 74 confirmed objects created across the
three desktops are removed. Budgets reserved 78 objects/18.625 MiB, including
uninvoked members of the interrupted GNOME region batch; reservations remain
nonrefundable. No bucket policy, versioning or existing user objects changed.
The failed GNOME region's329-byte artifact and journal remain in the private
test profile for reproduction; successful local archives/spools were cleaned.
That artifact is excluded from the committed numeric exports.

Failed/interrupted attempts remain separate: Cinnamon's initial tray frames,
GNOME's OS-modal and storage-interrupted native attempts, KDE's missing Escape
paste transfer and late witness. The unchanged fresh native matrices passed.
A new KDE metadata scaffold initially included its own sanitized outputs; its
filter/output directory was corrected with before/after hashes and no operation
replay. Its extra text-to-region pause remains a qualification. Frozen native,
driver, observer and evaluator behavior was preserved.

Worker exit0 is an enforced production callback contract. The supplemental KDE
sampler observed107 PID/start/hash identities but no actual zombie wait statuses;
disappearance is not independently observed normal exit. Error-only SDK fields
are unavailable on successes; they are not invented as zero.

## Reproduction and next diagnosis

The numeric archives include fixed parameters, source/package/binary hashes,
safe JSONL events, operation IDs, timing and cleanup qualifications. Replay
Cinnamon with the unchanged saved-evidence evaluator. Replay KDE's numeric
attribution with its separate `replay_numeric_memory.py`; it always reports
overall acceptance false. Neither offline replay establishes fresh desktop proof.

From the repository root, extract into fresh private directories:

```sh
replay_output=$(mktemp -d work/s3-replay.XXXXXX)
mkdir "$replay_output/cinnamon" "$replay_output/kde"
tar -xzf docs/evidence/live-s3-transport/source108/cinnamon108.tar.gz -C "$replay_output/cinnamon"
python3 docs/evidence/live-s3-memory/evaluate.py --bundle "$replay_output/cinnamon"
tar -xzf docs/evidence/live-s3-transport/source108/kde108.tar.gz -C "$replay_output/kde"
python3 "$replay_output/kde/kde108/replay_numeric_memory.py" --bundle "$replay_output/kde/kde108/a1"
```

The tracked manifest hashes identify the archives. GNOME's archive supplies
partial samples and failure joins for inspection, with no passing full-window
evaluator. The KDE replay is an attribution tool, not a replacement for the
unchanged production-cleanup acceptance evaluator.

The next useful diagnostic is a narrow, bounded TLS error classifier inside
the S3 adapter: capture only allowlisted fixed TLS classes or numeric OpenSSL
reason codes from the SDK error buffer, never the raw message. Reproduce the
readonly name/object HEAD and receipt-guarded bulk-delete failures with those
details. Compare connection/SDK lifecycle and actual distro libraries after
obtaining the reason. Preserve the passing Cinnamon changes and all existing
mutation/durability safeguards; another allocator experiment will not resolve
this production correctness gate.

## Storage correction

GNOME was99% full. Archived and reference-checked obsolete builds reclaimed
4.14 GiB there and 4.56 GiB in KDE. Lossless compression of 5,052 obsolete PPM
frames reclaimed 11.73 GiB on the host; per-file original/compressed hashes and
restoration metadata are retained privately. All three guests pass the new
space/inode preflight. Old GNOME/KDE disks remain backing the current golden
chains and were protected. No VM image or power/compositor setting changed.
See `lab-storage.md` for capacity gates, current-plus-one-rollback retention,
restoration and future immutable-base migration.

Committed evidence is under [source108](evidence/live-s3-transport/source108/). Raw frames, payloads, protected login inputs and credentials remain outside these archives.


## OpenSSL 3.5 handshake compatibility follow-up

Cinnamon passed the source108 live upload/delete gate. GNOME and KDE alone
reported intermittent Curl35 / SDK99 before an HTTP response. This follow-up
isolated a transport trigger, rather than treating their runs as memory passes.
On the existing KDE guest, 32 interleaved, unauthenticated `openssl s_client`
connections to `s3.us-east-2.amazonaws.com:443` failed 7 times with the default
first handshake and zero times with `-groups '?X25519MLKEM768:DEFAULT'`.
Certificate and hostname verification were required in both variants; no retries,
credentials, uploads, packet bodies or HTTP headers were involved. A separate
metadata-only message probe measured logical first ClientHello lengths of 1563
bytes by default and 343 bytes with the smaller predicted share. Failed default
connections reported errno 104. These are message lengths, not packet captures;
the exact peer or middlebox causing the reset remains unidentified.

Successful connections in the peer-metadata probe negotiated TLS1.3 and
X25519MLKEM768 in both variants. The smaller first share can require a
HelloRetryRequest and another round trip. This is the compatibility measure
[documented by OpenSSL](https://docs.openssl.org/3.5/man3/SSL_CTX_set1_curves/).
It changes prediction of the initial share, not certificate verification, TLS
versions, cipher policy, the advertised built-in group list or request retries.

The SDK's supported Curl per-handle option hook applies this only with Curl 7.73+
and an active OpenSSL 3.5+ backend. Older and other TLS backends keep their defaults.
OpenSSL's built-in `DEFAULT` group list must not override explicit administrator
policy. The guard uses OpenSSL's `NCONF` parser, including file inclusions and
variable expansion, to inspect the configured `system_default` section. Explicit
`Groups`/`Curves`, unreadable or malformed configuration, unresolved sections and
pre-existing library errors disable the override. Configuration values and paths
are never logged. The inspector does not load configuration modules or providers.
The guard resides in the transient S3 worker; the resident loads no additional
TLS library. Destructive requests still require fresh, non-reused connections.

Reproduction parameters: existing KDE p2k-a1, Curl 8.18/OpenSSL 3.5.5, verified S3
regional hostname above, 8-second connection timeout, 32 interleaved pairs,
reverse variant order on odd iterations. Capture subprocess output in memory;
serialize only attempt index, variant, elapsed time, process status, connected,
verified, protocol and bounded numeric error codes. Never store raw TLS messages
or process output. Product verification uses a separate new profile and 20 small
synthetic text uploads with exact public-body/hash checks, native URL paste and
receipt-guarded production deletion, under a 32-object/2 MiB ledger. No automatic
retries or broad desktop matrix precede that targeted KDE gate. Passing the TLS
probe does not establish product parity; product results will be recorded below.

The SDK contract suite checks backend/version boundaries, a custom P-256 group
policy, Curves aliases, dotted command keys, includes/variables, absent group
policy, missing/malformed files and preservation of a prior OpenSSL error. The
custom-policy test failed against the former unconditional override and passed
with the guard. The focused SDK, worker-protocol and deletion safety checks passed.
