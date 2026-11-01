# Source63 memory checkpoint

Tested product: `0.1.1-dev.63+g050b702c6a6d`, commit
`050b702c6a6da5c945ea8b4e138dee50e6fdbd83`, clean tree
`553a904064be556ebeec5ae06593994429b356b4`.
Documentation commits do not change the identity of these tested binaries.

The cumulative implementation meets the 40 MiB resident target in the measured
local workflows on all three desktops. Settings, History and image decoding use
transient helpers that exit normally; the resident retains its tray and native
capture/clipboard routing. Cinnamon and GNOME implementation routes remain
preserved. KDE avoids persistent graphical platform initialization, delays unused
image codecs, shares complete output frames, and fixes glibc's large-allocation
threshold. This accepts the local memory experiment, not the full release.

| Desktop and machine | Populated post-use peak RSS | Fresh empty-profile peak RSS | One-core idle CPU |
| --- | --- | --- | --- |
| KDE Wayland VM | 30.051 MiB in both runs | 27.914 MiB | 0.047–0.053% |
| GNOME Wayland VM | 32.809 MiB in both runs | 27.004 MiB | 0.0067–0.040% |
| Physical Cinnamon/X11 | 25.004–25.293 MiB after three view cycles | 24.762 MiB | 0.00666% cold |

Cinnamon's view-cycle values are settled RSS observations, not five-minute
post-native peaks. Its separate original cold protocol recorded 282 samples;
the GNOME/KDE protocols used 61 samples per five-minute window. Do not combine
these different workloads into a single comparison.

The matched KDE source57 post-local-native baseline was 146,194,432 bytes
(139.422 MiB). Source63 peaked at 31,510,528 bytes (30.051 MiB): a reduction of
114,683,904 bytes (109.371 MiB), or 78.446%. Both independent post-use windows
had the same peak. The fresh cold window independently passed. This is the
integrated result across the retained changes, not an allocator-only saving.

## Forecast and attribution

The native resident boundary was motivated by the old resident's graphics
mappings, including LLVM and Gallium. Their mapped RSS did not establish a
removable-byte forecast; expected product savings were unknown. Avoiding eager
codec discovery, explicit PNG decoding, a transient generic-image worker and
complete-frame sharing were likewise supported by source/probe evidence with
unknown integrated RSS benefit. The final combined workload confirms their
collective result. The earlier three candidates were diagnostic-only, not scored
against the full primary workload; their incremental contributions remain
unmeasured.

The final allocator probe retained about 4,096,000 fewer bytes in six standalone
Qt/glibc processes. That probe and the source62 40.508 MiB diagnostic used
other workloads. They are not comparable with the matched source57-to-source63
full-workload comparison. The allocator's incremental product saving is unknown.
The policy applies only to the KDE glibc resident; it adds no dependency and does
not affect sibling allocator settings.

## Correctness and limitations

Current local checks include all four capture/clipboard actions, native Qt/GTK
clipboard oracles, tray/menu, native fonts and folder dialogs, Settings
Save/Cancel/Reload with full configuration restoration, local History previews,
Activity parent/item independent paste, live transparent selection/crosshair,
and normal helper exits. KDE also passed its optional SVG paths. Host and KDE
registered suites passed 51 tests; GNOME's standard guest build passed 52.
Internal helper/optional-codec skips retain their qualifications in the original
build evidence; registered test passes alone do not prove compositor support.
Cinnamon's physical capture timing passed at 121.273 ms p95 across 30 measured
samples plus five warmups; all 35 operations cancelled without results.

GNOME's clock advanced from 08:03 to 08:04 during the same pending selector.
Root context compaction delayed Escape beyond the unchanged acquisition timeout;
that attempt remains failed. A separate fresh operation cancelled normally with
no result or items. These are separate proofs; same-operation clock plus normal
cancellation is not claimed. Failed setup, private-file guards and input-driver
attempts remain preserved rather than being relabelled as successful tests.

The numeric gates charge the complete observed product family, require unchanged
PID/start/executable/device/inode/hash identity, no live helpers or escaped
processes, adequate sample coverage, peak RSS <=41,943,040 bytes, and idle CPU
<0.5% of one core. PSS/private memory is diagnostic. GNOME/KDE cold runs use
prepared empty local profiles, blank shortcuts and notifications disabled.
Their externally warmed steady windows do not measure transient launch peaks,
stock onboarding, physical GNOME/KDE certification or freshly rendered tray
acceptance. Numeric sampling is discrete, so brief peaks between samples remain
unobserved. No new precision estimate is inferred from identical recorded peaks.

Real current guest S3 upload, remote History direct-link copy/deletion and
post-S3/populated-remote-history memory remain separate acceptance work. No
synthetic URLs establish those gates. Full U9/fresh-agent release acceptance is
pending. No credentials, raw configuration, image/text payloads or raw smaps are
in the portable exports.

## Reproduce and resume

Use [the recorded-evidence checks](reproduction.md) to verify sample hashes,
identity joins, coverage, RSS and CPU with Python's standard library. These can
run in GitHub Actions without Qt, Node.js or AWS credentials. They verify saved
data, not a new desktop session. Real compositor testing still uses
[the KDE procedure](../../history/testing-kde-memory.md) and
[the VM lab](../../vm-lab.md), current source manifests and visible VNC.
Use standard CMake presets/Ninja, the official AWS SDK and distro Qt.

Keep golden disks immutable and test independent overlays. Normally close the
populated resident before launching the independent cold profile. Preserve its
metadata fingerprint in place, verify the new process and socket peer, check all
Unix paths with the official 107-byte checker, wait five quiet seconds externally,
then record the unchanged 300-second/5-second observation. Verify normal final
quit, independently recompute raw rows, persist the worktree result marker first,
then the experiment log, read both back, and only then report or advance.
No favorable-run selection, forced shutdown or hidden persistent GUI is accepted.

The retained optimization branch is `codex/optimize-kde-native-boundary` in
`/home/patrick/.codex/worktrees/kde-native-boundary/put.gd`.
Local originals, authorizations and failed attempts remain under
`work/kde-native-boundary/` and the ignored experiment log:
`.context/compound-engineering/ce-optimize/kde-native-boundary/experiment-log.yaml`.
Portable JSON/JSONL evidence in this directory travels with the documentation
commit. The bundled CE decision script runs on the host only; no Node.js
fixture or dependency was installed on a guest.

## Accounting and next work

The stop condition is the confirmed local memory target. Four cumulative
candidates were attempted: three diagnostic-only predecessors and one confirmed
keeper. None of the three predecessors is retrospectively scored as a rejection
or isolated winner. There are no dependency-deferred hypotheses or separate judge
charges. New packages, cloud machines and desktop-setting changes in this final
regression extension: zero. Ordinary model cost is unavailable; no separate
optimization service fee is asserted. Elapsed wall time at the local-target
checkpoint was 22.587 hours, including
setup, approved pauses and regression extensions. Active work time is unknown;
wall time is not continuous sampling time.

Current savings from another optimization are unknown. The next useful work is
current real remote-workflow acceptance and the remaining UI/product plan,
followed by a new measured scope if further memory work is requested. Do not
claim a new bottleneck from the old source57 profile. Review, document the
strategy, create a PR, continue with a newly approved scope, or leave the branch
for manual review; the final user choice is not recorded until it is made.

Completed KDE and Cinnamon VMs were already parked. GNOME received one normal
session power-off request after final numeric persistence. The shutdown poll
raced process exit and failed its identity guard; that original failure remains
recorded. Separate read-only root and agent checks confirmed exact QEMU absence,
closed SSH/VNC listeners and unchanged golden disk/retained test overlay. No
force, retry, reset or obsolete app restoration was used. See
[the shutdown record](gnome-normal-parking-a1.json).
