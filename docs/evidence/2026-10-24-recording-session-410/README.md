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

**Incomplete acceptance.** The final-source relevant sanitizer selection and
actual product probe passed, and the separately identified read-only VM storage
gate passed. Full Dev and final-source desktop/memory/visual/cleanup evidence
remain pending. [The JSON ledger](summary.json) keeps those gates explicit.
This documentation PR stays draft until the execution owner
supplies exact-commit receipts and the report owner reviews them, including
saved Cinnamon, GNOME and KDE visuals and separate functional/memory gates.
The older reproduction and test counts in #410 describe that issue's original
source; they are not results for the new candidate.

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
review five and full Dev/current-head desktop acceptance remain pending.

The first candidate, `b11c3ba6d64a60ace215aeb8ef9ac9cd8418f142`, tree
`f9d1b5e55bb37d76c2522b6229b52faa5af7b6bb`, is **superseded without acceptance**.
The execution owner reported two additional review findings:
an event and reply read together can be applied out of order, and a delayed stop
reply can replace a finalized review after an edit begins exporting. The current
candidate contains the corrections above. Intermediate `39d1945d` contained
the ordering/finalization fixes; a later review found the CLI deadline gap.
The before-fix and intermediate timeout failures below are independently checked;
final passing receipts remain pending. Old build/test attempts stay separate;
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
graphical acceptance credit to `102b6708`. Its exact-head recheck is pending.

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
probe output. This proves the relevant sanitizer selection, not full Dev,
whole-project sanitizer, desktop or release acceptance.

## Btrfs VM storage prerequisite

[PR #556](https://github.com/patrick-hudson/put.gd-desktop/pull/556), exact
`f1883c9fa23a72ab6aa3f539d8f751a95b476a77`, fixes the read-only capacity gate
that blocked the Cinnamon VM before product acceptance. The [receipt](storage-prerequisite.json)
records the preserved failed gate and the successful schema-2 retry at
15:24:59 UTC on f44, run `1543038f-acf2-4140-b4ac-f432cebb06a4` / QEMU 165763.
Host Btrfs raw inode counters remain 0/0/0, explicitly classified `dynamic`
with a null available count. The measured 713,997,602,816 available bytes still
meet the 10 GiB reserve. Guest home/tmp each report 20,739,493,888 bytes and
1,577,198 available inodes, with a 2 GiB reserve and zero build estimate for
the prebuilt-package run. A separate d69 ext4 host-only control retains a
reported finite count. No product, GUI or VM lifecycle acceptance is inferred.

[Btrfs documents dynamic inode creation bounded by metadata space](https://btrfs.readthedocs.io/en/stable/Administration.html#flexibility).
The [VFS zeroes its statfs buffer](https://raw.githubusercontent.com/torvalds/linux/v6.18/fs/statfs.c)
before the filesystem callback; [Btrfs leaves inode counts unset and accounts for metadata exhaustion through available blocks](https://linux.googlesource.com/linux/kernel/git/torvalds/linux/+/3f7938b1aec7f06d5b23adca83e4542fcf027001/fs/btrfs/super.c).
The tool preserves raw values and verified filesystem identity; reported
0/1,024 counts fail and 1,025 passes. Unknown/mismatched identity, negative,
malformed and inconsistent values fail closed. Byte reserves, per-guest growth,
device/private-run identity and exclusive output remain enforced. Thirteen focused
tests passed on each reported host. Required #556 CI passed on predecessor 0dbc. The optional
review then found a host-stat timeout could escape before a failed receipt was
written. Current f188 preserves measured raw capacity and sanitized error types
and produces a failed receipt/exit 2 on host measurement failure. Its focused
re-review and required CI remain pending; the original failure and successful
0dbc proof stay separate with their original hashes, modes and modification times.
Both code PRs precede docs.

## Required receipts

- Clean source commit/tree, build manifests and running executable hashes;
  exact host, distribution, desktop/compositor, session type/ID and Qt platform.
- Regression failures on the unfixed source and passing checks on the fixed
  source, with commands, test names, exit status and repetition counts. Fixture
  or offscreen checks establish the session/protocol contract only.
- Per-desktop `open`/`toggle`, command stop reason, ready/recording cancel and
  review Discard checks, graceful helper exits, private staging/admission
  results, and an independent memory window with duration, limits and host load.
- Saved complete desktop frames for decisive actions, failures and completion;
  file hashes and report-owner visual review. A live wall tile or SSH log alone
  is insufficient. Keep raw bulk evidence private.
- Socket path preflight, physical desktop lock/heartbeat/release receipts where
  applicable, owned-process cleanup and VM retirement/export receipts if used.

Preserve failed attempts and corrected commands. A missing or failed gate stays
pending or failed; this record does not certify unrelated release profiles,
physical performance, package lifecycle or published assets. Retention follows
[the evidence policy](../../evidence-retention.md).
