# Proton P1 normal-use recovery — 2026-10-15

P1's bounded recovery and recipient campaign is complete for the supported My files
workflow, combining this native application follow-up with the
[service and recipient receipt](2026-10-15-proton-p1-recovery.md).
P2–P6 remain open. This is not packaged or three-desktop release acceptance.

## Candidate and boundaries

The crash-recovery fix is clean commit
`e558436e30c96d0e68fea14d9429e5ed2b811eaf`, based on `f7b97bb` and main
`50fd37cafafcc4611925642b93dccb9d9f0949ab`. Release executables were built with the
standard distro toolchain and deployed into a private prefix in a fresh
Cinnamon/X11 VM overlay (Cinnamon 6.6.4, Muffin 6.6.1, distro Qt 6.4.2,
kernel 6.14.0-37-generic, KVM, 1280×800 at scale 1). The earlier destination-unavailable check used clean
`f7b97bb`; upgrade and crash/account/timeout recovery used `e558436`.
[The numeric receipt](2026-10-15-proton-p1-app-recovery.json) records binary hashes.
The bundled helper remains
`93fc77e63913dd39075a7ede049289325fb7c16a3aaf20985bc62adbc25b0e99`:
SDK 0.22.2, Crypto 2.0.0 and Bun 1.4.2.

Only an independently authenticated Proton test session, one new isolated folder
and synthetic files were used. The private guest configuration contained only a
Proton profile. No S3 module, credentials or configuration were deployed; no S3
tests, account or bucket operations were run. The common build compiled the
existing S3 targets without invoking them. The Linux golden disk was preserved;
the test console stayed visible on the lab wall through VNC port 5920.

Each operation acquired two 16,777,216-byte files from the real native clipboard,
each SHA-256
`341aacac661ccb210720bedaa9ead5d668fe5ea41a73532fc147c71e34040df1`.
Actions, inspection, restart, copy and retry used the production resident and CLI.
An independent Qt fixture pasted the native clipboard and compared its actual
contents with original file URIs or confirmed receipt URLs in memory. The fixture
is an observer/source, not a provider mock. Screenshots were inspected for the
visible native fixture; this campaign does not repeat the full tray rendering
protocol. No capability URLs, account identities, passwords, tokens or private
journals are committed.

## Normal-use outcomes

| Failure | Observed recovery |
| --- | --- |
| Selected destination becomes unavailable | A private SDK fixture moved only the new synthetic folder to recoverable Trash. Both app items failed with `destination_unavailable`, no URL and `no_result`; explicit Copy yielded no result. Restart preserved operation/artifact/publication/destination/candidate identities and earlier confirmed history. Restoring that same folder and explicitly retrying the original operation completed both items; an independent paste matched their confirmed links. |
| Resident crashes during upload | Killed only the isolated resident after nonzero real SDK upload bytes, then waited for its helper to exit. The original build recovered as `interrupted` with an unknown upload and no URL but incorrectly retained `copyStatus: pending`. The corrected build repaired that existing journal and passed a fresh real-upload crash: `interrupted` / `no_result`, identities retained, and explicit Copy left original native file URIs unchanged. Nothing was automatically uploaded or copied after startup. |
| Request deadline expires after partial success | The crash retry encountered a genuine `proton_request_timeout` under the test profile's 30-second budget. It retained one confirmed shared upload and one unknown item. Native paste contained exactly the one confirmed link, with none for the unknown result. Restart preserved the confirmed receipt. Increasing only the current profile's retry deadline to the product default 120 seconds and explicitly retrying the same operation completed both items. Accepted destination/settings/candidates/publications stayed fixed; the already confirmed receipt was byte-for-byte unchanged. |
| Saved account access is unavailable | Removed only the disposable guest keychain entry while retaining its snapshot in process memory. Both app items returned `authentication_required`, no URL and `no_result`; explicit Copy retained the source file URIs. Restart preserved the saved operation. Restoring that same entry through a private pipe and explicitly retrying the original operation completed both items and copied exactly its confirmed links. No everyday login was changed. |

A final official-SDK listing of only the isolated folder found **10 confirmed
nodes for 10 exact saved receipts**, all with matching publication/checksum and
revision ownership proof: no duplicate confirmed upload and no visible unfinished
draft. All ten links were revoked through exact production history IDs. The folder
was restored, private synthetic files were retained, and no remote object was
permanently deleted. The separate test session was revoked with HTTP 200 and only
its host/guest keychain references were removed. Guest keychain storage used an
unlocked memory-only Secret Service collection; this does not prove persistent
keyring provisioning across OS reboot.

## Fix and automated verification

Startup recovery bypasses the normal operation-finish path to avoid automatic
cloud or clipboard replay. It previously marked unfinished operations interrupted
without settling their pending copy intent. The fix sets only interrupted,
pending copies to `no_result`, including old journals already in that state.
Confirmed receipts and existing copied/suppressed outcomes are preserved.

The regression reproduced the native failure before the fix (seven failing
assertions), then passed (11 Qt test outcomes including setup/cleanup). It covers
persisted accepted/acquiring/stored/queued/running/interrupted states, confirmed
receipts, already settled copy states and repeat-recovery stability. Reuse,
quality, efficiency and test-value reviews found no further changes. Focused
correctness and an independent adversarial review found no issues. The Anthropic
pass could not start because the shared transport dependency returned npm
`ETARGET` before egress; a separately dispatched local reviewer covered that lens.
No cross-model review success is claimed.

The ordinary `cmake --preset dev` and `cmake --build --preset dev` passed after the
code commit. The Release native targets passed. The final scoped
`ctest --preset dev` run passed **5/5**: payload, history, operations, Proton
contract and Proton transport, in **85.05 seconds**. No full-suite result is claimed
for this follow-up: S3 tests were excluded under the user's restriction. The prior
helper test and four known upstream TypeScript diagnostics retain the exact scope
in the earlier receipt; the helper was unchanged here.

Socket preflight covered the VM QMP, native D-Bus/X11 and resolved private app
endpoint before launch (maximum 53 UTF-8 bytes; app socket 33), plus the scoped
unit-test roots (maximum 39), all below Linux's 107-byte limit. The frozen memory
observer attaches to that existing endpoint and creates no new app socket.

## Cinnamon post-use memory

The unchanged frozen observer `bench_ui_process.py`, SHA-256
`a28c94b075036ea65f8eca8e068d480cff46ee959d4680593f5c4d55cce3fc40`,
observed the existing resident after all builds, functional tests, graphical
interaction and service cleanup finished. Two serial 300-second windows sampled
every five seconds retained the same PID/start-tick/executable identity and clean
source/binary version. Each window contains 61 samples.

| Window | Peak family RSS | Peak PSS | Peak private | CPU, one core |
| --- | ---: | ---: | ---: | ---: |
| 1 | 26.738 MiB | 12.363 MiB | 6.629 MiB | 0.00333% |
| 2 | 26.738 MiB | 12.363 MiB | 6.629 MiB | 0.00667% |

Both windows passed the unchanged post-use gates: RSS at most 40 MiB, no helper,
UI worker or escaped descendant, no swap, and CPU below 0.5% of one core. This is
sampled Cinnamon VM post-use evidence. It does not establish P2 active-upload
resources, catch every descendant shorter than the five-second sample interval,
prove a leak impossible, or replace cold physical/package/multi-desktop acceptance.
Raw samples, exact commands and evaluated identities remain private; the numeric
receipt retains sanitized totals, identities, hashes and gate outcomes.

## Evidence limits and P1 disposition

Normal supported use selects My files or a direct child, not another account's
collaborative folder. Authenticated loss of that selected folder is covered by
Trash/restore above; missing local account access is covered end-to-end, while
actual server session revocation, refresh, live quota refusal, changed revision,
recipient expiry/password/revocation and anonymous Viewer write denial are in the
earlier service receipt. An authenticated second-account ACL downgrade was not
performed and is not claimed. It is not an additional gate for this owned-folder
scope. Permission-denial and 429 classification/backoff remain bounded controlled
coverage where no normal-use live stimulus was available. No more wrong-password
or traffic-volume probes were attempted; a manufactured live 429 is not required.
The SDK's extended consecutive-429 cooldown remains unmeasured.

Retained failed/setup attempts include missing fresh-VM Qt packages (installed
standard distro runtime packages), an unavailable persistent login collection
(replaced only for the disposable test with the unlocked in-memory collection),
a transient non-atomic tray title/icon observation, the original pending-copy bug,
a native retry that hit its real 30-second deadline, overwriting a running unchanged
fixture during deployment (corrected by verifying unchanged hashes), and a private
controller import syntax error corrected before further mutations. The full tray
harness and frozen memory observer were not changed. Private artifacts under
`/tmp/p1a1` retain failed attempts and exact operation/history IDs; they are not a
public replay bundle.

P1 is complete with these recorded limits. Authenticated active-helper resources,
redistribution materials, shipping integration, update rehearsal and final
packaged multi-desktop acceptance remain P2–P6. Issue #10 stays open.
