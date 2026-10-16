# Lift and duotone implementation validation — 2026-10-16

PR #48 uses Lift for the tray, taskbar/window and packaged Linux launcher, and
duotone symbols for existing in-app actions. The branch was cut from main
`0ff7639d654cac6f55f519848eef937dbaeee4b6`.

The current native runtime candidate is clean commit
`22d2ec957a514d7a3e417c34b1d067f7ca696a05`, tree
`a0cc34b1ebeebe3669f4eb53d38100c7822c3e10`. Later commits `e0f2d49` and
`2e47092` change the host replay checker, its documentation and test fixtures;
they do not change application, provider or package runtime source. This final
documentation update also changes no runtime source. Native results below identify the actual `22d2ec9` binaries rather than claiming a later rebuild.

## Behavior and quick-upload feedback

The tray uses observed transferred bytes and the provider total when available.
When only transferred bytes are known, acquired source size supplies a labelled
estimate capped at 99% until finalization/receipt. Transfers are weighted by source
size; a retry can reset its fill. Work without a usable denominator remains dim
green after the reveal delay. Confirmed success displays full green before idle.

The user reported distracting dim-green/bright-green/blue transitions during small
uploads. Green now waits 200 ms before appearing, so successful uploads completing
inside that delay stay blue. Once revealed, green remains for at least 600 ms,
including at least 200 ms of terminal feedback. Preparation/storage gaps and
asynchronous receipt inspection retain the displayed frame instead of inserting
blue gaps. Errors still take immediate priority and blink red/blue every 300 ms
for three seconds.

Two tray regressions failed before this timing change and passed afterward. The
full tray group passed all 26 Qt cases. A native Cinnamon tiny-upload burst used
three queued operations, each with two 28-byte synthetic files. Its 125 dense SNI
samples over 5.993 seconds showed one uninterrupted green interval across the
sampled queued work; later independent operation/SNI observations confirmed blue
idle. Sampling did not cover the entire burst continuously. The deterministic
tray tests establish the sub-200-ms no-flash threshold.

## Build, tests and review

The clean candidate configured and built using CMake presets, Ninja, dynamically
linked distro Qt 6.4.2 and the pinned official AWS C++ SDK 1.11.907. All 59 Dev
CTest groups passed in 326.04 seconds. A separate clean Release build completed
all 199 targets and produced the Mint lab DEB with `PUTGD_LAB_FIXTURE=ON`.
The shipping audit correctly rejects fixture-enabled lab packages; release CI
uses the fixture-disabled shipping configuration.

The updated live-replay checker passed all 34 host contracts, including scoped
original-operation error pulses, actual red/blue/red recurrence, exact idle
recovery and independent counter correlation. These contracts do not establish
native rendering. A CI-only PID-file fixture race was reproduced as the original
`ValueError` before correction. Atomically publishing closed PID files fixed it;
all 18 cold-family observer contracts and the original reproducer passed. The
frozen observer and measurement harnesses were not modified. A separate focused
review of the fixture correction found no unresolved issues.

The initial feature review completed with local reviewers and an independent
Anthropic peer pass; its findings were resolved. The timing change completed a
five-lens simplification pass and focused correctness/adversarial review with no
unresolved findings. Its attempted Anthropic review failed OAuth refresh before
review; an independent in-process fallback completed, with no external review
claim. The host-checker audit also finished without unresolved findings.

All eight substantive CI jobs passed on implementation/checker/fixture head
`2e470929b4f6408085e662b32cfb87b3e7f09db4`; two release-only jobs were expected
skips. The current-base and live-review guard was clear after 321 quiet seconds.
The PR checks retain the result for the final documentation-only head separately.

## Current native desktop results

Cinnamon's functional and memory gates passed before GNOME/KDE expansion. All
three campaigns used new disposable overlays, sealed golden bases, visible
localhost VNC listeners, two vCPUs and 6 GiB guest RAM. Standard distro toolchains
and the same frozen source archive were used.

| Desktop | Native functional/view checks | 120-second post-use family observation |
| --- | --- | --- |
| Cinnamon 6.6.4 / X11 / Qt 6.4.2 | 12 cases and six routes passed | 25 samples; peak 29.73 MiB |
| GNOME 50.1 / Wayland / Qt 6.10.2 | 12 cases and six routes passed | 25 samples; peak 64.84 MiB |
| Plasma 6.6.6 / Wayland / Qt 6.10.2 | 12 cases and six routes passed | 25 samples; peak 32.09 MiB |

Each memory observation retained one resident, no UI helper or escaped process,
and the original resident identity. These are bounded post-use VM measurements,
not physical cold-idle or leak/plateau proof. GNOME's host collector exited 143
after the guest benchmark completed; actual completed guest results and samples
were recovered and verified against the unchanged resident and harness identities.
The cause of the collector signal remains unknown.

The twelve native cases cover region/window/desktop capture, Qt clipboard text/image/
files, three region-cancellation routes, and GTK clipboard text/image/files.
Independent native paste and saved-byte checks passed. Lift taskbar/tray and
duotone controls were inspected in actual desktop frames.

All three desktops’ live S3 replays passed queue/concurrent admission, byte/fill
correlation, matching public downloads, cancellation and same-ID retry, local-save
failure/red-blue-red recovery, local repair, and exact receipts across resident
restart. Restarting each supported native tray host during a real S3 upload
reconstructed the current green icon with the same resident, followed by blue
idle. Cinnamon's XApp watcher and GNOME's AppIndicator extension were actually
restarted; GNOME Shell and its Wayland session stayed unchanged. In these S3
replays, finalizing and sharing were too transient to observe and are not inferred.

Cinnamon and GNOME also passed direct icon inspection at actual compositor
3840×2160 / DPR2, independently confirmed by native Qt fixture metadata. Original
monitor layouts were restored. This proves icon/view rendering at native scaling;
it does not claim full 4K capture functionality. Cinnamon's older 1x paint locator
rejected a 2x capture fixture before the action; that harness limitation is retained.

KDE also passed real S3 replay under Breeze Light, with readable green fill and
native red/blue error frames. Restarting its plasmashell service removed and
reconstructed the panel while KWin and the resident stayed unchanged. Native
frames after reconstruction showed real byte progress and eventual blue idle;
the pre-stop green frame represented active preparation, before positive bytes.
The independent watcher remained registered throughout, so service PID/panel
absence and reappearance establish the actual host restart.

KDE passed actual single-display 3840×2160 / DPR2 and mixed-display inspection:
1280×800 / DPR1 beside 3840×2160 / DPR2. The same UI process moved between the
outputs and remained crisp and unclipped. Original active layout and effective
Kubuntu theme were restored. A rejected per-output mode ID and earlier
non-effective theme application remain failed setup attempts, not acceptance.

GNOME's installed AppIndicator host has no graphical hover-tooltip support.
Actual SNI ToolTip properties were verified; native hover frames lacked a tooltip.
This host limitation remains explicit rather than being counted as visible-tooltip
acceptance. Native Qt 6.10 probes were built for GNOME/KDE. GNOME's original replay
used the Qt 6.4-built reader dynamically linked to the actual distro Qt 6.10;
a subsequent distro-built reader returned an identical actual idle snapshot.

The unchanged physical cold-idle workload passed on actual Cinnamon/X11 using
the clean Release runtime: 300.165 seconds, zero operations, no observed mapped
owned windows and uninterrupted idle. The whole-family observer retained 285
samples spanning 299.810 seconds with a maximum gap of 1.108 seconds. It observed
one resident throughout and no UI helper or escaped process. Peak summed RSS was
26,685,440 bytes (25.449 MiB), peak PSS 10,985,472 bytes and peak private memory
5,042,176 bytes. CPU was 0.0033315% of one core. Both the legacy budget and the
40 MiB cold target passed. This sampled cold workload establishes no post-use or
leak/plateau claim.

The exact invocation was `observe_cold_family.py --seconds 300` with source
revision `22d2ec957a514d7a3e417c34b1d067f7ca696a05`, clean Release app/CLI/UI
from `build/acceptance49`, output `/tmp/li48cold` and receipt `/tmp/li48family`.
All test VMs/builds/GUI campaigns were stopped first. The existing user S3
playground stayed running and outside the measured family. No view, capture or
provider operation was invoked in the isolated workload. Socket preflight and
frozen harness/binary hashes passed. Complete results, raw samples, visibility
sidecar and process cleanup receipts are retained under the private canonical
`work/i47/physical48-current2/` export, manifest SHA256
`20098c95b7b28da76cbda4c4707b5729399401fdde696ea9d9bd42640ad84b90`.
An initial evidence-copy attempt encountered the stopped app’s Unix socket;
that partial export remains preserved. The corrected export copies regular
files and records the socket identity without creating another socket.

## Provider cleanup and retained failures

Cinnamon's 18 and GNOME's 12 exact original synthetic S3 receipts were removed
through reviewed production deletion preparations and acknowledgements. Their
actual outcomes and history remote states were all `removed`; all disposable S3
credential references were cleaned. User playground uploads were excluded.
KDE’s 18 exact synthetic S3 receipts were likewise removed, for 48 across the
three desktops. All owned S3 credential references were cleaned.

The initial saved Proton test account was full. With the user’s explicit
permission to wipe its contents, official-SDK maintenance removed the inventoried
210 files and their folders while preserving the account/session and the exact
empty Cinnamon recovery folder. After delayed quota reconciliation, storage usage
was 0 bytes. This is the immediate post-cleanup reading; later acceptance uploads
consume space. The saved host profile’s old destination folder was removed, so
fresh tests create new isolated folders under the account root.

Cinnamon then recovered both original quota-failed operation IDs, preserving all
four artifact/publication identities and confirming all four upload/share receipts.
Its fresh Proton replay passed queueing, genuine encrypted counters with labelled
source-size estimates, concurrent app admission and original-ID sequential busy
recovery, cancellation/retry, local failure/repair and exact receipts after restart.
Native pixels independently prove green estimated fill and red→blue→red blinking.
The genuine sharing phase was observed; finalizing was not observed. Native pixels
and SNI reads can cross a 300 ms phase boundary, so supplemental dense frames,
rather than labels alone, establish the visible blink. All owned guest references
were cleaned and the latest session was returned by exact compare-and-replace.

GNOME completed the same native Proton checks through an integrated campaign
with exact-ID continuations. An unexplained host controller exit 143 interrupted
the first queue attempt; actual guest operations completed and the rotating
session was safely recovered. A restarted fixture then recreated 23-byte defaults:
those four remote items are retained as small-file evidence and excluded from
large-source acceptance. Exact original 16 MiB source hashes were restored before
the corrected concurrent admission check, which independently matched both new
IDs in 18 observations. Strict SNI polling missed returning-red phases in the
two expected busy windows; genuine dense native frames prove both exact
red→blue→red sequences. Those sampling failures remain failed driver attempts.
Sequential original-ID recovery, counter-based cancellation/retry, local-byte
repair and restart/exact receipts then passed without changing the checker.
The local retry repaired both saved items; its historical original operation
remains failed. Independent final native inspection confirmed partial green,
both busy pulses, the local failure pulse and returned blue. Sharing was observed;
finalizing was not. Session collect/CAS and all owned reference cleanup passed.

KDE also completed the integrated Proton campaign. Native queued and concurrent
rows, real counters and readable Estimated 25% at 4 MiB/16 MiB were inspected.
The strict poll missed returning red during expected busy responses; dense native
frames independently prove both exact busy windows. An SSH interruption during
original-ID recovery was preserved. The controller later verified all four
original operations completed/shared, safely collected the latest session,
confirmed exact source CAS and cleaned its reference before continuing. The
unchanged cancellation/retry, local-error/repair and resident restart tail passed;
its strict local red/blue/return-red/idle samples and supplemental native frames
both passed. Sharing was observed; finalizing was not. Session collect/CAS and
all owned reference cleanup completed; no app/SDK/fixture work remained.

At acceptance export, the private inventories recorded 38 confirmed synthetic
Proton objects in four isolated folders: Cinnamon’s 14 original-plus-fresh
receipts, GNOME’s ten 16 MiB plus four superseded 23-byte receipts, and KDE’s ten
16 MiB receipts. Production guarded deletion correctly reports unsupported
capability for Proton; the application’s deletion contract remains unchanged.

The user subsequently requested cleanup. Separate maintenance through the pinned
official SDK matched all 38 node identities, active revisions and parent folders
against those receipts, and matched all four folder names/UIDs against their
creation records. It rechecked the account root, live/trash roots, file revisions
and exact folder children before mutation. All four trash results and four
permanent-deletion results succeeded. The four folders and their 38 files are
removed; the unrelated live entry and existing trash set were preserved. The
account stayed logged in, the latest session was returned by exact source CAS,
and the temporary host LAB reference was cleaned. Historical acceptance journals
and receipts remain unchanged; they are not relabelled as production deletions.

The private completion receipt is `work/i47/p48-clean2/completion.json`, SHA256
`d532b9547fd8f52c4550e36100823b23595bf3340f971ec2988638861dc8ce82`.
The immediate account quota counter still reported its prior
570,572,074 bytes; actual SDK results and the absence of the exact target folders
establish deletion, rather than a quota assumption. The first private helper
attempt stopped before mutations because it compared the SDK’s Result-wrapped
name directly with a string. That attempt and its safe session handoff remain
preserved; the corrected helper unwraps the value before identity comparison.

Earlier failures remain unchanged in the private archive: initial shared-name
concurrency fixture, superseded test runs, missed error-blink samples, a missing
local-output precondition, one continuation bootstrap path, a corrected tiny-file
analysis expectation, GNOME's late paste collection, and the collection signal
above. Corrected continuations use the original operation IDs where applicable.
No failed attempt was silently relabelled as a pass.

Cinnamon and the first GNOME overlay were retired through guarded cleanup after
full campaign exports and checksum verification. A fresh GNOME overlay was used
only for Proton acceptance; its verified 336-file export preceded guarded
retirement. KDE’s original owned overlay was stopped and resumed for Proton,
retaining its run identity and sealed backing base. It was guarded-retired after
a verified 588-file campaign export. All remote Proton test objects were
inventoried before the later cleanup. No golden disk was booted directly or
modified. All isolated socket paths were checked against Linux’s 107-byte UTF-8 limit before launch; failed
preflight/setup attempts are retained.

## Exact identities and evidence retention

Live replays use the production CLI/native fixture and genuine native tray host
as described in [the replay protocol](../testing-tray-progress.md). Each main
replay uses a new private destination, two deterministic files per operation and
a 180-second operation timeout. Main S3 replay files were 32 MiB on Cinnamon and
16 MiB on GNOME/KDE; the final KDE host-reconstruction batch used 128 MiB files.
Proton files were 16 MiB on all three. The 28-byte Cinnamon burst and GNOME’s
superseded 23-byte attempt are separately identified. The official pinned Proton
SDK is 0.22.2; its runtime manifest SHA256 is
`b75fa484a33933c5e74d4cb0e5b5428f174cfabdb4aa932dd84dce2dfe233ff0`.
GNOME/KDE archive builds correctly report supplied source provenance with unknown
Git dirty status; archive producer evidence and exact binary hashes identify them.

Frozen runtime source archive SHA256:
`690d5f6321b03cad8541126eb4f6785527c8e3d926ed33488834f0023503a65e`.

| Package | Lab DEB SHA256 | Installed app SHA256 |
| --- | --- | --- |
| Mint | `5c23aa9af5f959ca534b24c3e3d201663a34f2696d34cbbb51ce1f62969ae254` | `acda17363ce9000f352605d959c0e92c5b84ad1e5050a4ef132f285d8a6d8c9f` |
| GNOME | `34bfff49661c892ba0cf249baa6538a4573129bc104d1e9f03412503e857e006` | `49e2eb74289c097de48c9aa4bf3cf9e85040c71efdf4fe868b8ec85699cd184b` |
| KDE | `5a7ce65098f5ad0fa020a32f37817e33035b4d3c3c9cd853bb947766bcfcf617` | `f1b78007422d169ea33aba648e873d18a3ec090bb8581c6e1d2411b7662129ad` |

The private canonical `work/i47/` archive retains source/package identities,
parameters, native frames, operation receipts, raw memory samples, review records,
all failed attempts and overlay retirement receipts. The earlier `75c56ca` native
and physical campaign remains historical evidence; it predates the timing change
and is not substituted for current-product results. Its [earlier validation record](https://github.com/patrick-hudson/put.gd-desktop/blob/3f98d01f9e3a104ab9629b62b423805dbd559240/docs/evidence/2026-10-16-lift-duotone-validation.md) remains at its original commit.

The local S3 playground remains separate from acceptance. Its taskbar initially
resolved older absolute-icon desktop entries; only their icon references and the
relevant Cinnamon taskbar group were refreshed to Lift, with backups preserved.
The playground was refreshed with the timing fix while idle, preserving its
configuration and history. No user-created playground object was removed.
