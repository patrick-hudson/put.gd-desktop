# Recording session contract — issue #410

Companion documentation [PR #550](https://github.com/patrick-hudson/put.gd-desktop/pull/550)
for [issue #410](https://github.com/patrick-hudson/put.gd-desktop/issues/410),
covering graceful cancel/discard shutdown, `action.invoke`'s recording `mode`
and `stopReason` after a command stop. The code and documentation PRs target
`nightly`; merge [recording code PR #551](https://github.com/patrick-hudson/put.gd-desktop/pull/551)
and the narrow [VM storage prerequisite #556](https://github.com/patrick-hudson/put.gd-desktop/pull/556)
before this companion. The [recording contract](../../recording.md)
retains private review and explicit Upload as the only admission path.

## Evidence status

**Scoped validation complete on source `3440ab9c`.** Full Debian Dev CTest
(108/108), MCP (5/5), relevant unsuppressed ASan/UBSan (3/3) and the actual
capabilities probe passed. The report owner independently reviewed the final
Cinnamon/GNOME/KDE changed-behavior receipts, separate 180-second resident
memory windows, saved visuals and cleanup. The separately identified #556
read-only VM storage prerequisite also passed. [The JSON ledger](summary.json)
links each result to its actual source, binary, host/session and package bytes.

This is targeted #410 validation. Physical GNOME remains blocked by its recorded
prerequisites; the final GNOME result is a fresh guest. Original Mint package
full-run evidence and corrected Mint package smoke evidence stay separate.
No full release, published-asset or complete package-format acceptance is claimed.

The current candidate is [`3440ab9c4fb6bd745d50eb6c0728a318951ee481`](https://github.com/patrick-hudson/put.gd-desktop/commit/3440ab9c4fb6bd745d50eb6c0728a318951ee481),
tree `17b445850b0f1d5258b77bf88a8ccbcc5edbb3e8`. It queues command replies in
the same order as helper events and retains capture finalization independently
of the current review/export state. Its regression source includes adjacent
event/success and event/error replies, a real trim to revision 2 during held
Save copy/Upload exports, and a cancelled-session successor. It also gives
`action.invoke` the same 45-second reply deadline as `recording.stop`, because a
toggle can stop. The real CLI regression holds that stop for 16 seconds, beyond
the old 15-second wait. Deterministic event/reply boundary rows complement the
pipe fixture, whose writes need not land in the same read. The last four-line
test-only change in `102b6708` waits for an overwrite export to finish before
asserting stale revision rejection. The current candidate additionally frees the
probe's caller-owned GStreamer version string. Static review four covered 102b;
[review five](review-results.json) found no actionable issue in the minimal
3440 ownership delta. It did not repeat the earlier broader reviews or execute tests.

The first candidate, `b11c3ba6d64a60ace215aeb8ef9ac9cd8418f142`, tree
`f9d1b5e55bb37d76c2522b6229b52faa5af7b6bb`, is **superseded without acceptance**.
The execution owner reported two additional review findings:
an event and reply read together can be applied out of order, and a delayed stop
reply can replace a finalized review after an edit begins exporting. The current
candidate contains the corrections above. Intermediate `39d1945d` contained
the ordering/finalization fixes; a later review found the CLI deadline gap.
The before-fix and intermediate timeout failures below are independently checked.
Final passing receipts use their own identities. Old build/test attempts stay separate;
no graphical or CI credit transfers between these commits. This paragraph
records source review and reported findings, not a new execution result from
the report owner.

## Before-fix negative control

[The sanitized baseline receipt](baseline-results.json) binds d69's two failing
integration suites to production baseline
`1a70ac7f02b75bc4c75a9d768b2775e837e771a6` plus the test-only patch SHA-256
`3f59b3f5aff9623802069b96a17635f22cd0582fe1236e7246018944256f3488`.
Both CTest entries failed; QtTest reported 20 passes/5 failures in the service
suite and 11 passes/8 failures in the helper suite, including setup/cleanup.
The failures show the missing user reason, four crashed cancel states, rejected
record mode, a 9-second budget clip replaced by 12 seconds, edited export state
changes, and adjacent budget replies losing the reason or returning an error.
These are expected failures, with no candidate or desktop pass implied.

The report owner exported the log, JUnit, configure/build logs, source, patch
and socket preflight through pinned SSH, matched every file against remote
SHA-256, read the failure diagnostics and verified the patch applies cleanly.
The exact patch can be reconstructed from public source:

```sh
git diff 1a70ac7f02b75bc4c75a9d768b2775e837e771a6 \
  39d1945d3a8a5139d140f8d5748b6f831a392c26 -- \
  tests/fixtures/FakeRecordingBackend.h tests/fixtures/record_helper_fixture.cpp \
  tests/integration/test_recording_helper.cpp \
  tests/integration/test_recording_service.cpp > baseline-test.patch
sha256sum baseline-test.patch
```

Its digest matches the saved patch above. Apply only that patch to a fresh
baseline checkout, reuse the provisioned official SDK/dependencies, and follow
the commands in the receipt. The original isolated paths `/tmp/r410d/r` and
`/tmp/r410d/t` are historical; use a new private short directory for a replay and
recompute every socket path before launching. The original preflight passed at
31, 54 and 55 UTF-8 bytes. The initial configure failed with `Pinned SDK LICENSE
required for packaging`; the dependency/license-path correction and successful
configure retry remain in the private evidence. Baseline executable hashes and
a standalone build manifest were not supplied, so this records reproducible
patched test source rather than a fully fingerprinted binary comparison.

## Intermediate CLI timeout negative control

[The intermediate receipt](intermediate-cli-results.json) uses production
`39d1945d3a8a5139d140f8d5748b6f831a392c26` plus test-only patch SHA-256
`1790f3200f8c9365e79e70f3b337e5ed41185ba43011f9c993410a4c053bc438`.
That patch exactly matches the diff to `052d4363` for `test_cli.cpp` and
`test_recording_service.cpp`, excluding the production deadline fix. On d69,
the real CLI toggle-stop regression returned exit 1 rather than 0 with its fake
stop held for 16 seconds. QtTest reported 15,396 ms elapsed, two setup/cleanup
passes and one failure; that elapsed time is not an independent client timing
measurement. The log identifies dynamic Qt 6.8.2 and GCC 14.2.0 on Debian 13.
The log, configure/build output, patch and 31/49-byte socket preflight were
exported and hash-checked privately. This reproduces the old wait budget;
it does not establish a passing final candidate.

## Prior candidate: GNOME monitor subset on 052d4363

[The prior-run receipt](prior-052d-gnome.json) records clean
`052d4363826fc01fa62a552f91db48b74667383b`, build
`26.1024.1207-dev+g052d4363826f`, on physical Fedora 44 GNOME Shell 50.6.
Six monitor-target cases passed in the execution receipt: ready/recording cancel,
CLI/MCP/toggle user stops and a real budget stop with a late duplicate stop.
The report owner matched running executable hashes, paired all six helper starts
with requested exit 0, recomputed 274 lifetime samples over 138.079 seconds with
at most one recorder, and reviewed the cleared live-review frame. Its Discard
button matches the recorded RFB click. The measured resident RSS maximum was
40,173,568 bytes; this is debug lifetime coverage, with no RSS performance claim.

The original receipt accidentally selected SSH tty session 1289. A separate,
hash-bound correction identifies active local Wayland session 1100, compositor
PID 144305 and its generation; the originals remain unchanged. GNOME region
selection was blocked by the absent Shell extension, so this is a monitor subset.
Only the specifically cleared live-review frame was exported; private failed
cover frames remain on the host. This prior candidate supplies no test, CI or
graphical acceptance credit to later source. Final guest GNOME results below
are independently identified.

## Unsuppressed product-probe leak

[The leak receipt](preexisting-probe-leak.json) retains the failed 052d product
probe separately from the reported passing fixture cases. The deep trace shows
one 17-byte allocation from `gst_version_string` reaching `probeMedia` at
`src/record/MediaProbe.cpp:87`. The report owner verified identical file SHA-256
`bbba00f926d3e84ec2682e7fa95e5e8934659d074abaf2c5447d88742517ff3b`
at baseline, 052d and 102b; blame places the line at `f91ae2711` on October 17.
The [official GStreamer contract](https://gstreamer.freedesktop.org/documentation/gstreamer/gst.html#gst_version_string)
returns a newly allocated caller-owned string. No suppression or fix was applied
in the recorded 052d run; that product probe remains failed. Root subsequently
authorized the minimal ownership fix in #551. The exact 3440 source passed the
unsuppressed product-probe rerun below, with no permanent probe exclusion.
The original failure provenance does not turn the old probe or whole sanitizer
suite into a pass. Final-head results use their own exact-case receipts.

## Current-source sanitizer results

[The exact-build receipt](final-3440-sanitizers.json) binds clean 3440 source,
Debug `asan` preset, product/test/fixture SHA-256 and ELF build IDs to the
unsuppressed run on d69. All three whole CTest entries passed: `cli_integration`
(43.37 s), `recording_service_integration` (5.68 s) and
`recording_helper_integration` (12.37 s), total 61.42 s, zero failures/skips.
The helper suite includes the actual product probe. A separate real
`putgd-record --capabilities` run returned valid JSON and exit 0 without the
earlier leak report.

The saved options are `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1` and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`; the direct probe also uses
`fast_unwind_on_malloc=0`. No `LSAN_OPTIONS`, suppression or function exclusion
was present. The report owner verified export hashes, manifests, JUnit and
probe output. This proves the relevant sanitizer selection; the full Dev and
desktop results have separate receipts below. It is not whole-project sanitizer
or release acceptance.

## Final Dev, MCP and desktop results

[Dev/MCP commands and exact identities](final-3440-dev-mcp.json) bind the
108/108 CTest pass (880.39 s) and 5/5 MCP pass (21.144 s) to clean 3440 source.
The historical socket preflights passed at 34/49 bytes (Dev) and 34/42 bytes
(MCP). The required code CI passed on the same head in
[run 40105903461](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40105903461).
Its selected Linux, Windows, sanitizer, PipeWire and contracts lanes succeeded;
packages were intentionally unselected, with no CI package-acceptance credit.
The separately retained [052d oracle failure](oracle-results.json) explains the
four-line test-only completion wait; it remains a failure for that old head.

[The desktop receipt](final-3440-desktops.json) preserves the three product
manifests, installed/package/running hashes, active local session and desktop-shell
generation, native Qt platform, case IDs, helper PIDs and independent memory
joins. KDE
uses `plasmashell` for the harness field named `compositorPid`; this identifies
the desktop shell generation, not a separately fingerprinted KWin process.
An installed GNOME version in that host inventory is not GNOME execution proof.
Every full run covers ready cancel, recording cancel, CLI and MCP command
stops with `stopReason: "user"` and inspect parity, toggle stop followed by an
actual Review **Discard** click, and a real 1 MiB budget stop followed by another
stop returning the unchanged review. All six helpers per full run exited
normally with requested exit 0; at most one recorder was observed, no protocol
kill occurred, and no implicit archive payload was admitted.

The delayed stop reply arriving **during** edited revision-2 Save copy/Upload,
and adjacent budget-event success/error replies, are deterministic fixture
regressions. They preserve the existing reason, details, clip and edit through
pending export. The desktop budget cases prove the later-stop path, not those
precisely scheduled edited-export races.

| Desktop / exact placement | Functional scope | Independent idle RSS / PSS maxima (bytes) |
| --- | --- | --- |
| Mint 22.3, Cinnamon 6.6.4 X11, f44-hosted guest `1543038f` | C4 six cases on original `c5bb1171` DEB | cm3: 32,641,024 / 21,794,816 |
| Debian 13, Plasma 6.3.6 Wayland, physical d69 session 20103 | K1 six cases on `20c9b47e` DEB | km3: 34,967,552 / 11,900,928 |
| Ubuntu 26.04.1, GNOME 50.1 Wayland, f44-hosted guest `afa7f719`, session 35 | G1 six region cases through Mutter; packaged extension version 2 ACTIVE, corrected `73faf5f6` DEB | gm1: 35,151,872 / 23,522,304 |
| Mint 22.3, Cinnamon 6.6.4 X11, fresh d69 guest `7b7493a7` | Corrected `fb19e0c7` DEB: three-case smoke, actual Review Discard, UID 1000 access to 89 files / 19 directories | No new idle run attributed to this package |

Each independent idle window contains 181 samples over at least 180 seconds;
RSS and PSS maxima stay below 41,943,040 bytes (40 MiB). Each resident exits 0.
These are fresh resident idle windows, separate from functional lifetime samples;
they do not establish open-helper family memory, the complete U15/native package
walkthrough, or physical #379 performance. KDE's actual budget check used a
25-second seeded moving grid (script SHA-256
`02278a08eae573b78d46e14a85ecf814187073bce9f4cfaffae3868093eb079d`),
without changing the product's 1 MiB threshold or the frozen harness.

### Package invocation correction

[The package-scope receipt](package-scope.json) records a private CPack invocation
under umask `077` that created 19 archive directories with mode `0700` in both
Mint's original `c5bb1171` package and GNOME's rejected `eea19fae` package.
GNOME caught extension unreadability before product acceptance. Fresh staging
under umask `022` produced corrected Mint `fb19e0c7` and GNOME `73faf5f6`
packages with all 19 directories `0755`; all nine executable hashes are identical
within each old/new pair. No source or C++ binary rebuild supplied that change.
The Debian archive actually has 19 `0775` directories, executable files `0755`
and no directory missing world traversal; its generated-data modes are retained
in the receipt. The earlier status message calling those directories `0755`
was inaccurate.

The original Mint full six cases and idle values remain bound to `c5bb1171`.
Root accepted the unchanged nine-binary join without repeating memory; the
corrected `fb19e0c7` package then passed a fresh normal-user path-access and
three-case GUI smoke on a different guest. This does not relabel the old full
run as corrected-package execution. GNOME's full functional and memory results
belong only to the corrected `73faf5f6` package after reinstall and guest re-login.
These custom packages are separate from CI or published release assets.

### Reviewed saved visuals and failure context

[The visual receipt](visual-results.json) binds unchanged PNG bytes and actual
Discard clicks to their source/run/package. The report owner viewed all eleven
published frames and checked each click against the visible button; the
synthetic surround and app UI contain no protected login input or user payload.

| Run | Live Review / actual Discard witness | Real budget stop | Independent idle |
| --- | --- | --- | --- |
| Original Mint C4 | [Review](visuals/cinnamon-c4-review.png) | [1:44 retained clip](visuals/cinnamon-c4-budget.png) | [cm3 cover](visuals/cinnamon-cm3-idle.png) |
| Corrected Mint C2 | [Review](visuals/cinnamon-c2-review.png) | Outside this smoke scope | Outside this smoke scope |
| Physical d69 KDE K1 | [Review](visuals/kde-k1-review.png) | [0:56 retained clip](visuals/kde-k1-budget.png) | [km3 cover](visuals/kde-km3-idle.png) |
| GNOME guest G1 | [Review](visuals/gnome-g1-review.png) | [1:52 retained clip](visuals/gnome-g1-budget.png) | [gm1 cover](visuals/gnome-gm1-idle.png) |

Cinnamon C3's original 90-second budget wait failed while the encoder needed
about 104 seconds to reach 1 MiB; its [saved failure](visuals/cinnamon-c3-budget-timeout.png)
shows the recording still active at 1:31. C4 changed only the private harness
wait to 180 seconds, preserving the threshold. Earlier guest clock-lock and
inhibitor-denial failures and GNOME's bad-package/missing-venv preparation
failures remain in the private catalogs. Early cm1/cm2 and km1/km2 cover failures
retain numeric diagnostics **without a saved failure frame**. No missing visual
is fabricated. The specifically cleared disposable Cinnamon clock image remains
private; incomplete-cover physical GNOME frames were excluded from transfer. The
[physical-attempt receipt](physical-gnome-attempts.json) independently preserves
numeric/source/time/failure fields and original hashes for g1/g2/g3/g5/g6/g7,
without transferring their failed pixels.

### Reproduction, retention and cleanup

Use clean source/tree 3440 and the exact native build/package identity for the
target row. Reproduce Dev/MCP with the commands in their JSON receipt and the
sanitizer selection with its separate recorded options. Desktop drivers are
private source artifacts, separately hash-identified in the final catalog;
their original and historical hash-matching versions remain retained. Functional
parameters are a synthetic 960×540 region at `[100,100,960,540]`, H.264/MP4,
system/microphone audio off, native `xcb` or `wayland`, isolated profile/archive,
and a 1 MiB limit for the budget case. A replay must use new short private paths,
run socket preflight before launch, authenticate the active graphical session,
acquire the physical lock when applicable, maintain the visible wall and retain
its own saved visuals. Never reuse retired overlays or boot a golden.

[Cleanup receipts](cleanup-results.json) join each completed guarded retirement
to its exact run ID, exported completion manifest, overlay identity and unchanged
golden SHA-256. All three disposable overlays were retired after verified
exports. The execution owner reports the owned QEMU processes, tunnels, wall
entries and bootstrap agents stopped/removed. Physical d69's checked desktop
release is at 15:45:48 UTC, preserving its preexisting signer. The old shared
lab CLI lacked `cleanup`; the exact unmodified 3440 `overlay_cleanup.py` API
was configured with the actual canonical lab root, preserving all guards.
Root confirmed this as a path-context correction, not a waiver.

[The retention index](retention-index.json) locates nine immutable private
catalogs, all 229 independently verified members, original failures, historical
harness sources, source-info, review and command receipts. Raw bulk is directly
usable outside release source; no evidence was deleted or recompressed.

## Btrfs VM storage prerequisite

[PR #556](https://github.com/patrick-hudson/put.gd-desktop/pull/556), exact
`796d41f291f34aabe60064ae1ac86ad2b221941e`, fixes the read-only capacity gate
that blocked the Cinnamon VM before product acceptance. The [receipt](storage-prerequisite.json)
records the preserved failed gate and the successful schema-2 retry at
15:29:33 UTC on f44, run `1543038f-acf2-4140-b4ac-f432cebb06a4` / QEMU 165763.
Host Btrfs raw inode counters remain 0/0/0, explicitly classified `dynamic`
with a null available count. The measured 713,978,998,784 available bytes still
meet the 10 GiB reserve. Guest home/tmp each report 20,730,822,656 bytes and
1,576,773 available inodes, with a 2 GiB reserve and zero build estimate for
the prebuilt-package run. A separate d69 ext4 host-only control retains a
reported finite count. No product, GUI or VM lifecycle acceptance is inferred.

[Btrfs documents dynamic inode creation bounded by metadata space](https://btrfs.readthedocs.io/en/stable/Administration.html#flexibility).
The [VFS zeroes its statfs buffer](https://raw.githubusercontent.com/torvalds/linux/v6.18/fs/statfs.c)
before the filesystem callback; [Btrfs leaves inode counts unset and accounts for metadata exhaustion through available blocks](https://linux.googlesource.com/linux/kernel/git/torvalds/linux/+/3f7938b1aec7f06d5b23adca83e4542fcf027001/fs/btrfs/super.c).
The tool preserves raw values and verified filesystem identity; reported
0/1,024 counts fail and 1,025 passes. Unknown/mismatched identity, negative,
malformed and inconsistent values fail closed. Byte reserves, per-guest growth,
device/private-run identity and exclusive output remain enforced. Fourteen focused
tests passed on each reported host. Required #556 CI passed on predecessor 0dbc. The optional
review then found a host-stat timeout could escape before a failed receipt was
written. The follow-up also validates a bounded ASCII filesystem name and
hexadecimal magic before retaining either field. Current 796d preserves raw
capacity and sanitized error types and produces a failed receipt/exit 2 on host
measurement failure. Required CI passed on exact 796d in
[run 40107259458](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40107259458)
(changes/contracts/CI summary; other lanes intentionally unselected). The final
focused root review independently rejected the malformed-private-text
case and recomputed all 12 catalog members; it found no actionable issue. The
review did not repeat all 14 tests or SSH/GUI workflows. Optional Greptile also
succeeded on current 796d at 15:46:23 UTC; the predecessor timeout discussion is
resolved/outdated, with no new finding. The original failure and successful 0dbc/f188 proofs stay separate with their
original hashes, modes and modification times.
Both code PRs precede docs.

## Remaining limits

Physical GNOME and full release/package-format gates remain outside this scoped
pass; no native credit transfers from the older 052d monitor run. Baseline
binary hashes were not archived before rebuild, though the exact source/test
patch and negative JUnit remain reproducible. Fixture/headless checks retain
only their stated contract scope. Merge #551 and #556 before #550; Patrick owns
the merge decision. Retention follows [the evidence policy](../../evidence-retention.md).
