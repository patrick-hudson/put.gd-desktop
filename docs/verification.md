# Implementation evidence

## Required Linux coverage

The [acceptance matrix](acceptance-matrix.md) requires Mint, Ubuntu, Kubuntu,
Fedora and Debian, with Cinnamon plus GNOME/KDE and native X11/Wayland coverage.
Prefer the build servers for direct desktop tests and VM hosting; provision
missing profiles, and run physical-hardware release gates on physical machines.
This requirement is separate from the historical results below; missing new
rows and current-candidate evidence remain gates.

## Login-startup uninstall contract (#382) — 2026-10-24

Patrick corrected the contract: remove managed startup links for active accounts
when the installation disappears, and for inactive accounts at next login;
preserve settings and keep package scripts out of homes. Code PR #544 and docs
PR #545 implement/document that choice. Clean `d5b561dd` development artifacts
passed **five native and six actual AppImage** targeted lifecycle rows across
Mint/Cinnamon X11, Fedora/GNOME Wayland, Debian/Kubuntu Plasma Wayland, Ubuntu
GNOME Wayland and Aurora Plasma Wayland. Every row includes real enabled and
post-removal logins, active cleanup with the resident quit, unchanged settings/
data, explicit CLI disable, reviewed pictures and a 180-second memory check.
All RSS/PSS measurements are below 40 MiB. Fedora native's strict library-map
failure is a proven pre-existing distro Qt/OpenSSL dependency and remains
explicit in the raw receipt. Startup CTest, 26 shell tests, 14 source-collection
tests and complete branch/integration CI passed; earlier failing attempts are
retained. Test VMs were retired after verified export and physical locks released.

[Exact matrix, source/artifact identities, pictures and limits](evidence/2026-10-24-uninstall-startup-382/package-acceptance/README.md).
#382 is closed per Patrick’s explicit decision that old entries in accounts
which never ran the updated app are not a first-release blocker given limited
existing usage. The [decision and closure receipt](evidence/2026-10-24-uninstall-startup-382/legacy-rollout-decision.json)
record that exception. Code #544 merged into `nightly` at
`35a16d0e6c63bc57bffe8d7bfeeeca5ad428c28c`; companion #545 carries the docs.
Published-nightly lifecycle verification, including AppImage replacement with
real channel metadata, continues under #384. These results are targeted
development-artifact acceptance, not full A–Q/A–K or stable-draft certification.
The retained-link/TryExec tests at `91864c41` are superseded history.

## First stable gate plan — 2026-10-22

No release asset has been accepted yet. The first stable switches to calendar
versions `vYY.MDD.N` first ([#373](https://github.com/patrick-hudson/put.gd-desktop/issues/373)),
so it won't be `v0.1.1`. There is no candidate channel: merging the promotion
tags `vYY.MDD.N` and stages the stable draft, and exact-asset acceptance runs on
that draft before it is published. If acceptance fails, the tag stays unreleased
and the next promotion gets a new version.
[Issue #44](https://github.com/patrick-hudson/put.gd-desktop/issues/44) holds the
current gate table, the proposal for what the first stable requires or defers, and these
work packages:

| Gate | Issue | Status on 2026-10-22 |
| --- | --- | --- |
| Calendar versions `YY.MDD.N`, prerequisite for promotion | [#373](https://github.com/patrick-hudson/put.gd-desktop/issues/373) | design approved 2026-10-22 (one code PR, no `-rc`); implementation in progress |
| AppImage full acceptance A–K, six desktops | [profile](acceptance-matrix.md#appimage-full-acceptance-profile), [first pass](evidence/2026-10-21-appimage-full-acceptance/README.md) | first pass on nightly 842 failed F (5 desktops), H and J (f44); rerun pending |
| DEB lifecycle, recording rows, live S3 | [#375](https://github.com/patrick-hudson/put.gd-desktop/issues/375) | partial: source144 Cinnamon VM only; no state-preserving upgrade or removal + login on Wayland; schema 5→7 never exercised |
| RPM lifecycle on f44 | [#377](https://github.com/patrick-hudson/put.gd-desktop/issues/377) | partial: repo upgrade 816→828 only |
| GUI/CLI/MCP parity, AE17, fresh-agent walkthrough | [#378](https://github.com/patrick-hudson/put.gd-desktop/issues/378) | partial: 2026-10-13, CLI `dev.143` |
| Physical Cinnamon latency, 10,000-entry history, burst, slow network | [#379](https://github.com/patrick-hudson/put.gd-desktop/issues/379) | latency stale (current63, 121.273 ms p95); history/burst/slow network not started |
| Multi-display geometry in VMs | [#380](https://github.com/patrick-hudson/put.gd-desktop/issues/380) | partial: 3-head Cinnamon X11 region, single-output fractional scale |
| Physical mixed DPI and rotation | [#381](https://github.com/patrick-hudson/put.gd-desktop/issues/381) | blocked: no builder has two outputs; proposed for 0.1.2 |
| Uninstall login-startup entry | [#382](https://github.com/patrick-hudson/put.gd-desktop/issues/382) | closed by Patrick; 11 development-artifact rows pass, legacy exception accepted; PR integration and #384 release checks remain ([evidence](evidence/2026-10-24-uninstall-startup-382/README.md)) |
| License, notices, provenance, dependencies | [#383](https://github.com/patrick-hudson/put.gd-desktop/issues/383) | AppImage rehearsal on `v26.1022.968-nightly` ([below](#appimage-exact-asset-audit-rehearsal-383--2026-10-22)): provenance passes; notices, complete source, Qt CVEs and codec patents open (#416–#419); DEB/RPM not yet audited |
| Release campaign and stable signing | [#384](https://github.com/patrick-hudson/put.gd-desktop/issues/384) | promote.py dry run passed on `0435ed6a` under the old scheme; repeat after #373 |
| KDE/X11 rows, graphical-store installs | [#385](https://github.com/patrick-hudson/put.gd-desktop/issues/385) | proposed for 0.1.2; GNOME/X11 obsolete (GNOME 50 has no X11 session) |

Flatpak ([#83](https://github.com/patrick-hudson/put.gd-desktop/issues/83)) is
outside the first stable.

## Native package full acceptance (#375, #377, #382) — 2026-10-23

[Evidence](evidence/2026-10-23-native-full-acceptance/README.md): the
[native package profile](acceptance-matrix.md#native-package-full-acceptance-profile)
(A–Q) on nightly `v26.1023.1058-nightly` (`05991d9d`). It covered the `ubuntu24.04` DEB on m74
(Mint Cinnamon X11), the `ubuntu26.04` DEB on k72 (Kubuntu Plasma Wayland, plus its X11
session for recording) and on an Ubuntu 26.04 GNOME VM, the `debian13` DEB on d69 and the
`fedora44` RPM on f44.
- **Passes everywhere:**
  - fresh install from pkg.put.gd;
  - the never-run dev.133 → nightly upgrade with a 10,000-entry history (#56 layout move,
    schema 5→7, rows, counts and config preserved);
  - 0.1.1-nightly.947 → CalVer through apt and dnf;
  - recording with the distro media stack, motion against a static control;
  - a real browser loading a live upload;
  - the read-only repository channel;
  - start at login and removal.
- **Fails:** item P on m74, because of [#490](https://github.com/patrick-hudson/put.gd-desktop/issues/490):
  the login-started resident survives logout on Cinnamon.
- **Other findings:** #491–#495.
- **#382:** the per-desktop behaviour of the retained login entry, with and without
  `TryExec`, is recorded for Patrick's decision.
- **Scope:** a nightly pass. The release campaign reruns the same drivers on the release bytes.

## DEB/RPM exact-asset audit rehearsal (#383) — 2026-10-22

Assets: the ubuntu24.04, ubuntu26.04 and debian13 DEBs and the fedora44 RPM of
`v26.1022.1017-nightly` (`3582b30a`, after #424 and #428), from
[Nightly run 39864143584](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39864143584).
The README lists each package's SHA-256. Audited on f44, with lint in rootless
containers, and identities on m74, k72 and d69. Evidence and repeatable scripts:
`docs/evidence/2026-10-22-native-license-audit/`. This is a nightly pass; it
does not certify release bytes.

- **Provenance passes.** `SHA256SUMS` and its minisig verify. Each receipt's
  package digest, manifest and inventory match (105/105 per DEB, 107/107 for the
  RPM), and `--version-json` agrees on each distribution. The pool DEBs equal the
  assets. The repo RPM is the asset plus the signer's signature (equal header and
  payload digests). `InRelease`/`Release.gpg` and the RPM signature verify with
  the keys the packages ship, and the stable key fails them. The AppStream
  catalogs match the packages.
- **Notices gap, fixed in [#468](https://github.com/patrick-hudson/put.gd-desktop/pull/468):**
  the AWS SDK's and CRT's NOTICE files and the notices of cJSON, TinyXML-2,
  libcbor and xxHash, compiled into the private SDK libraries, weren't shipped.
  The AppImage had the same gap. #468 also replaces leaked CPack defaults
  (Vendor "Humanity", boilerplate description, no Maintainer address) and the
  RPM licence id `GPL-3.0`.
- **Decided by Patrick and implemented in #468:** the declared licence names the
  main licences (`MIT AND Apache-2.0 AND GPL-3.0-or-later`; upstream allows
  "or later" for Proton's crypto), and `runtime-notices.txt` lists every other
  licence ([#469](https://github.com/patrick-hudson/put.gd-desktop/issues/469)).
  The native notices point to the release page with the Proton helper's source
  (#416, native side). The Maintainer stays `packages@put.gd`.
- **Open:** no Debian `copyright`/`changelog` and no RPM
  `%license` ([#470](https://github.com/patrick-hudson/put.gd-desktop/issues/470));
  non-PIE Fedora programs ([#475](https://github.com/patrick-hudson/put.gd-desktop/issues/475));
  hygiene ([#471](https://github.com/patrick-hudson/put.gd-desktop/issues/471)).
- **Dependencies and codecs.** The Proton helper's npm lock has no known
  advisories (npm audit and OSV). The release jobs' npm installs all 26
  packages, so the notices match (#369). No codec is bundled; recording uses
  the distribution's GStreamer/FFmpeg (#419 comment).

## AppImage exact-asset audit rehearsal (#383) — 2026-10-22

Asset: `putgd-26.1022.968-nightly-x86_64.AppImage`, SHA-256
`dc17d5c6fb92a35514f381051d58f9a0170fad2b40c7e0e328dae64b219f51cf`, from
[Nightly run 39737710388](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39737710388)
on `ddc7981e`. Audited on m74 (Ubuntu 24.04 base). Evidence and repeatable
scripts: `docs/evidence/2026-10-22-appimage-license-audit/`. This is a nightly
pass; it does not certify release bytes.

- **Provenance passes.** `SHA256SUMS` matches GitHub's digests and the
  pkg.put.gd copy. The minisig verifies with nightly key `71440FA5E439AEBB`.
  All 759 receipt inventory entries match the extracted image. The runtime is
  the pinned type2-runtime, and the update information, `.zsync`, the
  `/dl/nightly` permalink and the feed agree with the CalVer tag.
- **Contents pass.** No debug or test files beyond the intended test-pattern
  plugin, no secrets, one absolute CI RUNPATH
  ([#421](https://github.com/patrick-hudson/put.gd-desktop/issues/421)).
  The 110 MiB download is 36 % Proton helper, 29 % Qt, 14 % other Ubuntu
  libraries, 12 % recording and 6 % put.gd.
- **Ubuntu packages are current.** All 153 bundled distro ELFs match today's
  noble-security builds by GNU build ID. A stable image freezes them
  ([#420](https://github.com/patrick-hudson/put.gd-desktop/issues/420)).
- **Open, all release-blockers:** the AppImage notices
  ([#417](https://github.com/patrick-hudson/put.gd-desktop/issues/417)), and
  decisions for Patrick: complete source for Qt and the LGPL libraries
  ([#416](https://github.com/patrick-hudson/put.gd-desktop/issues/416)) and
  the H.264/H.265/AAC patent posture
  ([#419](https://github.com/patrick-hudson/put.gd-desktop/issues/419)).
- **Qt 6.8.3's later CVEs
  ([#418](https://github.com/patrick-hudson/put.gd-desktop/issues/418)):** Patrick
  chose to bump Qt; the AppImage now bundles Qt 6.12.0
  ([#497](https://github.com/patrick-hudson/put.gd-desktop/pull/497)), with an
  A–K re-acceptance on six desktops
  ([evidence](evidence/2026-10-23-appimage-qt612-418/README.md)).

Not covered: the Proton per-release check. The DEBs and the RPM are in the
section above.

## AppImage window icon on Plasma (#523) — 2026-10-23

Lab code: [#530](https://github.com/patrick-hudson/put.gd-desktop/pull/530).
Evidence: [2026-10-23-appimage-icon-523](evidence/2026-10-23-appimage-icon-523/README.md).

- **Cause.** The neon smoke runs' "W" icon was a lab artifact. `lab.py`'s
  private `XDG_DATA_HOME` hid AppRun's `putgd-ui.desktop` from KWin, and the
  1058 AppImage's Qt 6.8.3 can't send the icon itself.
- **Before and after.** On k72 (Plasma 6.6.6), d69 (Plasma 6.3.6) and a neon
  User 6.8.1 VM: 1058 showed "W" in the dashboard's and the editor's title bars.
  The Qt 6.12.0 AppImage (`26.1023.1101-dev+gaa047473ad2e`) showed the put.gd
  icon in the same setup, sending it over `xdg-toplevel-icon-v1`. 1058 with the
  real data home showed the put.gd icon too.
- **Lab fix.** For AppImage profiles, the runner links the private data home's
  `applications/` to the session's. In a fresh neon overlay, 1058 run through
  `lab.py` showed the put.gd icon and captured the desktop through KWin
  ScreenShot2.
- **Capture risk.** On k72 (KWin 6.6.6), 1058 with the private data home failed
  desktop capture with `capture_permission`, and passed with the real one.

Not covered: neon Unstable, and f44/m74 (no app change). Follow-up: launches
whose `XDG_DATA_HOME` differs from the compositor's lose KDE capture
([#531](https://github.com/patrick-hudson/put.gd-desktop/issues/531)).

## Package install retries and snapshot (#368) — 2026-10-22

Code: [#388](https://github.com/patrick-hudson/put.gd-desktop/pull/388), head
`aca7858f`, all CI jobs green
([run 39727602972](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39727602972)).
Policy: [CI coverage, package installs](ci.md#package-installs). Evidence:
`docs/evidence/2026-10-22-package-installs-368/`.

- **Before.** Run 39717118907 (#357, `fd9e255`): `native (fedora44)` failed
  twice because no mirror, `dl.fedoraproject.org` included, served mesa
  26.2.4.
- **Demo.** Run 39729960271 on throwaway `e8f9443b` (`aca7858f` plus injected
  mirror faults, not merged). A fault on attempt 1 passed on the retry (fedora44,
  ubuntu24.04). Four failed attempts installed from the snapshot and the
  packages built (fedora44-snapshot, debian13). A fault on every attempt and on
  the snapshot failed the job (fedora44-allfail).
- **Lab.** On f44 (podman, pinned `fedora:44` and `ubuntu:24.04`), with
  networking cut: the offline install from a snapshot passed, a package outside
  the snapshot failed, and an unknown package failed without a retry.

Not covered: the `ci.yml` jobs' installs (#389).

## Stalled package installs (#467) — 2026-10-23

Code: [#498](https://github.com/patrick-hudson/put.gd-desktop/pull/498), head
`1926941a`, all CI jobs green
([run 40008537011](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40008537011)).
Policy: [CI coverage, stalled installs](ci.md#stalled-installs). Evidence:
`docs/evidence/2026-10-23-package-install-stalls-467/`.

- **Before.** Nightly run 39851369595 (`eab13a18`): `packages / appimage`
  hung for 93 minutes after `Get:5 https://archive.ubuntu.com/ubuntu
  noble-security InRelease` until it was cancelled.
- **Demo.** Run 40008409878 on throwaway `04aac39e` (#498's install script plus
  a stalling apt proxy, not merged). A stall was stopped after 60 s and the
  retry passed. Two stopped attempts fell back to the snapshot, which
  installed. With the default limits and no snapshot, four 10-minute stalls
  failed the job red after 43.5 minutes, inside its 90-minute timeout.

Not covered: retries for the `linux` and `sanitizers` installs (#389), the
AppImage install's lack of a snapshot (#500), the Windows `portable` job
(#501), and the Qt ASan download (#502).

## Weekly stress lane on k72 (#360) — 2026-10-22

Code: [#364](https://github.com/patrick-hudson/put.gd-desktop/pull/364). How it
works: [CI coverage, weekly run on k72](ci.md#weekly-run-on-k72) and
[remote builders](remote-builders.md#weekly-stress-lane-on-k72). Evidence:
`docs/evidence/2026-10-22-stress-weekly/`.

- **Installed.** `tools/lab/install-stress-weekly.sh` ran on k72 and enabled
  lingering. `systemctl --user list-timers` shows the next run at
  Sun 2026-10-25 03:00:00 CDT.
- **Skip path.** With a fake quiet A/V lock in a test copy, the service skipped
  the week, logged why and wrote a `weekly.json` with status `skipped`. A dry
  run with the same lock reported the same skip.
- **Full run.** At `3ae60a5`, ten iterations of the 74 integration tests ran
  with four busy loops at time scale 3, 117.5–119.9 s each.
  `capture_process_integration` failed once
  ([#398](https://github.com/patrick-hudson/put.gd-desktop/issues/398)). The
  earlier run at `fe24932` reached the live desktop through the inherited
  session environment, which the wrapper and the unit now remove.
- **Token missing.** `~/.config/putgd-stress/github-token` doesn't exist yet.
  The run logged that it wouldn't comment and kept its results; #398 was
  filed by hand from the run's ledger with `flake_ledger.py issues`.
- **Tests.** `tests/ci/test_stress_weekly.py` (19) and
  `tests/ci/test_flake_tools.py` (20) run in the `contracts` job.

## Flaky-test rethink (#351) — 2026-10-22

Code: [#351](https://github.com/patrick-hudson/put.gd-desktop/pull/351), head
`7ffcf42`, all CI jobs green
([run 39712963621](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39712963621)).
Policy and tools: [CI coverage, flaky tests](ci.md#flaky-tests). Evidence:
`docs/evidence/2026-10-22-flaky-tests/`.

- **History.** Every failed `linux` and `sanitizers` job with a failed test from
  2026-10-16 to 2026-10-22, all attempts, re-pulled from the Actions API
  (`ci-failures-2026-10-16-22.csv`): 22 flakes, 2 open non-wait failures (#352,
  #353), 6 real bugs.
- **Stress lane, before and after.** `tools/ci/stress_suite.py`, the twelve
  tests that flaked in CI. Nightly `33c768e`: 6 of 8 runs failed on d69 with the
  tests and four busy loops on two CPUs, 1 of 8 on m74, 6 of 6 on d69 with three
  busy loops on one CPU (`gui_history_view` overran its 5 s budget, "5350 ms
  would have been sufficient"). #351: 0 of 8 on d69 (`4984e80`), 0 of 10 on m74
  (`7ffcf42`), 0 of 6 on d69 (`ea703c6`, timeouts scaled), and the 74
  integration tests 0 of 8 on k72 with four busy loops, twelve at once
  (`c6918fe`). Runs during the work found four races scaled waits alone did
  not fix; all are fixed in #351.
- **Retry and ledger.** On #351 a temporary forced-flaky integration test passed
  its retry, was listed in the job summary and opened tracker issue #355 (run
  39710070705); a forced-flaky `unit` test failed the job without a retry (run
  39711215419). Both were reverted (`0ed3b74`, `f49ffcb`) and #355 closed.
- **Parallelism.** k72, full suite, three runs each: 106.8 s at eight with the
  old pool; with the profiled pool 137.5 s at six, 113.2 s at eight, 97.4 s at
  ten, 83.8 s at twelve. Hosted CTest at twelve: 76.6 s (24.04), 95.0 s
  (26.04), against 107.4 s and 97.8 s at eight.

Not covered: the lane is not scheduled (proposal #360); Python contract suites
(#99, #160 were flakes there) are outside this policy.

## Recording session contract (#410) — 2026-10-24

The [companion report](evidence/2026-10-24-recording-session-410/README.md) and
[JSON ledger](evidence/2026-10-24-recording-session-410/summary.json) track
graceful cancel/discard shutdown, recording `action.invoke` modes and command
stop reasons. Clean `3440ab9c` passed the three relevant whole ASan/UBSan CTest
entries and the actual product probe with leak detection enabled. Full Dev,
Cinnamon/GNOME/KDE functional/memory checks, final visual review and cleanup
remain pending. The separately identified #556 read-only Btrfs VM storage gate
passed; it establishes capacity only. The documentation PR stays draft;
headless checks and prior-candidate results do not establish new desktop
acceptance. Merge recording #551 and prerequisite #556 before companion #550.

## Recording closeout (#87, #166, #165, #164, #89) — 2026-10-20

The [closeout receipt](evidence/2026-10-20-recording-closeout.md) finishes #87's
lab-reachable items on nightly `9c035bf` and re-measures #166. Every capture
used the lab cover pinned to a named output, a VNC precheck of at least 98%
cover, and frames kept only at least half cover (`tools/lab/cover_player.py`,
`tools/lab/record_run.py`, #165).

- **Ubuntu 24.04 DEB on physical Mint 22.3 Cinnamon X11 (m74):** monitor and
  region recordings in all four formats, motion 27–39 against a frozen control
  of 0.000. A/V with `vah264lpenc`: medians −17.7 to +10 ms, pause step within
  ±0.9 ms, export within ±1.0 ms.
- **Ubuntu 26.04.1 GNOME 50.1 Wayland, two outputs (VM on f44):** monitor and
  region on both outputs in all four formats through Mutter. The guest's full
  disk capped monitor clips at about 2.7 s (#213).
- **Physical Plasma 6.6 Wayland (k72):** after a first attempt stopped by two
  i915 GPU hangs in `kwin_wayland` (load 5–7), a quiet redo recorded the 26.04
  DEB (monitor and region) and the AppImage (monitor) in all four formats via
  `kde-screencast` and `vah264lpenc`, with no hang. A/V: medians −18.5 to
  +6.7 ms, pause step within +0.8 ms, export within ±1.0 ms.
- **#166, f44 GNOME portal (window):** pre-#143 `134a9be` lost about 10 ms at
  every pause (−9.2 to −10.5 ms) and in every export. #143 `cc4568f` held
  within ±1.0 ms. The d69 portal (monitor) run is pending one picker approval.
- **#165:** the cover goes to the named output (two-output GNOME guest, nested
  two-output KWin), and the player refuses without `--output`.
- **#164:** the wall watchdog runs on d69. It restarted a stopped server
  after 300 s and brought the tile back, and it ignores a reset the stream
  survives. The k72 re-login and power-cycle checks are pending Patrick's OK.
- **#89:** GPU-first encoder order kept. Seven Skylake hangs were all in the
  compositor's render engine, and only three, in two events, overlapped a
  VA-API encode ([recording.md](recording.md)).
- Blocked or moved: AMD and NVIDIA encode (#138), the headphone and Bluetooth
  listening test (#212).

## Screen recording acceptance sweep (#87) — 2026-10-19

The [#87 acceptance receipt](evidence/2026-10-19-recording-acceptance-87.md)
records the lab-reachable checks from issue #87, on nightly `5ac6bbc` (AppImage
and Ubuntu 26.04 DEB, CI run 39325350860) and `8e568fd` (Fedora 44 RPM, Ubuntu
26.04 DEB, CI run 39336656763). Every run used a moving pattern with a scored
static control, saved frames and recorded identities.

- **AppImage A/V** (`--av-marker`/`--av-sync`, built-in speakers): exported
  MP4/WebM within −24 to +27 ms on k72 X11, d69 `kde-screencast` and portal, and
  f44 Mutter and portal (window); the RPM on f44 read +10.0/−2.5 ms.
- **#81 AppImage on Wayland:** d69 Plasma 6.3 and f44 GNOME 50 recorded regions
  and monitors in all four formats as native Wayland clients (no X11 socket),
  motion 14–37 against controls of 0.02–0.06.
- **Packaged GNOME extension** (f44, RPM `8e568fd`, re-login): version 2
  `ACTIVE`; dragged and selected regions plus monitor, four formats, motion
  22–38. RPM removed and the session restored afterwards.
- **MCP end to end:** 20/20 checks on k72, d69 and f44.
- **Ubuntu 26.04 GNOME 50.1 and Kubuntu 26.04 Plasma 6.6.6 guests:** Mutter,
  KWin and the GNOME portal; all four formats recorded, saved and uploaded.
- **Multi-monitor Wayland** (`lab.py --heads 2`, both guests): each monitor's
  canvas matched its output, regions on either head and across both had the
  right size and content, and the ring and bar stayed outside on the right head.
- **Share URLs** (bucket `put.gd`, prefix `acceptance/87-20261019/`, a scoped
  key Patrick entered in Settings): MP4, WebM, GIF and WebP played in Firefox and
  Chromium on f44, then were deleted through put.gd's history (S3 origin 403;
  CloudFront kept cached copies). #121 had landed, so WebM ran with its fix.
- **Pause/resume A/V:** sync holds within each segment, but a resume steps it by
  −27 to +30 ms, and the export moves the post-resume part ~10 ms earlier.
- Follow-ups: #129 (Stop hangs with a null audio sink, seen in VM guests), #130
  (pause/resume A/V step), #131 (stale-extension message after an upgrade), #137
  (AppImage can't record on Plasma 6.6 Wayland).
- Blocked: AMD/NVIDIA encoding, physical Mint Cinnamon, a headphone listening
  test and Bluetooth/other sinks.
- **Physical Plasma 6.6.6 Wayland (k72):** the Ubuntu 26.04 DEB recorded regions
  and monitors in all four formats (motion 25–57), passed MCP 20/20 and read
  −15 to +10 ms A/V; the AppImage can't start a recording there (#137). The k72
  wall tile now has a Wayland VNC server too.

## Wayland recording A/V sync — 2026-10-19

The [A/V sync receipt](evidence/2026-10-19-wayland-av-sync.md) records #86 on
`claude/wayland-av-sync`, final build `0500748` (scratch builds on each desktop's
builder), measured with the `--av-marker`/`--av-sync` check through the built-in
speakers with a GStreamer player on every desktop.

- Cause: the export rebased audio to its own first sample, though a Wayland
  working clip's audio starts 20–110 ms after its video, so MP4/WebM played the
  sound that much early. PipeWire video was also anchored to the first frame's
  arrival delay (0–90 ms per run on KWin).
- Exported MP4 medians, before → after (n runs): d69 Plasma 6.3 `kde-screencast`
  −86.7 / −25.3 → −8.7 / +4.7 ms (n=2; ffplay +18.7), d69 portal −112.7 →
  −10.7..+7.3 (n=3), f44 GNOME 50 `mutter-screencast` −92.7 → −6.0..+3.3 (n=3),
  f44 portal (window) −54.0 → −4.7..+6.0 (n=3).
  k72 Kubuntu 26.04 X11 is unchanged at −12.0 / −18.0 (ffplay +7.3). Working
  clips and exports now agree within 0.7 ms.
- `test_export_av_sync`: MP4 and WebM keep a 110 ms audio start (nightly's
  exporter: −110 ms); silent MP4, GIF and WebP carry no audio, and their frames
  match nightly's exporter.
- Not covered: headphone or Bluetooth sinks, multi-monitor, the AppImage build
  (tracked in #87), and ffplay on GNOME Wayland (it doesn't play there).
  Follow-ups: #119 (an undecodable Opus packet in WebM exports, pre-existing),
  #120 (the bar's System audio box on GNOME).

## X11 editor window refusal — 2026-10-19

The [X11 editor refusal receipt](evidence/2026-10-19-x11-editor-refusal.md)
records #106 (issue #103, the X11 half). The candidate was `85d04f0`, packaged by
manual CI run 39266644046: the Ubuntu 26.04 DEB (sha256 `0b8aa778…`) and the
AppImage (sha256 `d14dc770…`).

- On Kubuntu 26.04 Plasma X11 (k72), window capture of the Edit & upload editor
  was refused (`capture_target`) from both builds. The editor was the only
  put.gd window, and its `putgd-ui --edit-image` process was a direct child of
  the resident.
- Settings and History were still refused, and an external `kdialog` window was
  still captured.
- `x11_isolated` and `x11_native_snapshot` add an unregistered resident-child
  case. Both fail against nightly's X11 sources and pass with the change, on d69.
- Not covered:
  - a Cinnamon desktop (the core route ran on Xvfb only);
  - recorder windows;
  - Flatpak.

## GNOME editor window refusal — 2026-10-19

The [GNOME editor refusal receipt](evidence/2026-10-19-gnome-editor-refusal.md)
records #104 (issue #103), from pull request CI merge `3a9438c`. The Fedora 44
RPM (sha256 `ba4d1774…`) and the AppImage (sha256 `3a4cae53…`) both ran with
the RPM's Shell extension, version 2, loaded.

- On Fedora 44 GNOME 50 Wayland, window capture of the Edit & upload editor
  was refused (`capture_target`) from both builds. The editor was the only
  put.gd window. In both builds its `putgd-ui --edit-image` process was a
  direct child of the resident.
- Settings and History were still refused, and an external window was still
  captured.
- The GJS ownership contract adds the editor case. It fails against nightly's
  extension.
- Not covered:
  - recorder windows;
  - the X11 adapters, which still don't refuse the editor;
  - the Flatpak app-ID rule (#83 change 2).

## AppImage Wayland window icons — 2026-10-19

The [Wayland window icon receipt](evidence/2026-10-19-appimage-wayland-icons.md)
records #101 (issue #88): candidate CI AppImage `930d228`, sha256 `98bba655…`,
against nightly `9042776`. Both ran from the image with nothing installed.

- In a Wayland session AppRun writes `putgd-ui.desktop` and
  `putgd-record.desktop` while the image runs. On Debian 13 Plasma 6.3.6
  Wayland, KWin's title bar and task switcher then showed the put.gd icon. On
  Fedora 44 GNOME 50 Wayland, the overview and dash showed it. The baseline
  showed generic icons on both.
- Both desktops: no entry was left after a normal quit, and the next launch
  removed the entries of a SIGKILLed launch.
- With the put.gd Shell extension (temporary user copy) on GNOME, window
  capture of the native Settings and History windows was refused
  (`capture_target`), and an external window was captured.
- Not covered:
  - The editor's separate `putgd-ui --edit-image` process is not refused on
    GNOME. This is independent of native windows and tracked in #103.
  - Recorder windows were not opened.
  - KWin 6.6 Wayland was not run.

## AppImage screen recording on Wayland — 2026-10-19

The [Wayland AppImage recording receipt](evidence/2026-10-19-appimage-recording-wayland.md)
records the nightly `9042776` CI AppImage (#75, #77 and #81), sha256
`1be3fbdc…`. It ran from the image with private XDG directories and nothing
installed.

- Desktops: the physical Debian 13 Plasma 6.3.6 Wayland (`kde-screencast`, host
  VA-API) and Fedora 44 GNOME 50 Wayland (`mutter-screencast`, the bundled `x264enc`)
  desktops, with Kubuntu 26.04 Plasma X11 as the control.
- Flow: each desktop recorded a dragged region, a region completed through
  `recording.select`, and the whole monitor, in MP4, WebM, GIF and WebP. Every clip
  was reviewed, saved and uploaded to a local destination: 89 clips in total.
- `overlayPlacement` matched [recording](recording.md): region `outside` everywhere,
  monitor `inside` on KWin 6.3, Mutter and X11.
- The recorder's bar and review window were native Wayland clients, using
  `xdg-shell` and LayerShellQt, with no X11 connection.
- Against the same test pattern standing still, every clip's motion score was
  hundreds of times higher.
- On GNOME, a rectless region needed the put.gd Shell extension, run as a temporary
  user copy. Without it the region ended `region_unavailable`.

Not run:
- A/V sync;
- the portal path;
- `window` and `desktop` targets;
- KWin 6.6 Wayland on hardware;
- multi-monitor;
- AMD/NVIDIA encoding;
- the window icons and own-window refusal of #88.

The image's own `putgd-record` diagnostics can't read its exports (#92).

## Screen recording packages — 2026-10-18

The [recording packaging receipt](evidence/2026-10-18-recording-packaging.md)
records pull request #75, with final code `7aa1aff`:

- Pull request CI on `7aa1aff`: contracts, Linux suites, Windows build, three
  native packages, and the AppImage. The AppImage's audit runs the relocated
  recorder and requires its bundled x264/x265 to work.
- On the physical Kubuntu Plasma/X11 desktop, the Ubuntu 26.04 DEB recorded,
  reviewed, exported MP4, WebM, GIF and WebP and uploaded to a local
  destination. So did the Fedora 44 RPM on GNOME/Wayland (Mutter, and the
  portal on an earlier commit). Every clip carries the moving pattern.
- The AppImage did the same on Kubuntu, on Debian Plasma/Wayland (KWin's
  screencast protocol through AppRun's entry) and on Fedora. There it used its
  bundled x264, because the stock driver cannot encode H.264.
- The Fedora `noopenh264` placeholder produced the actionable dialog on the
  dashboard, and stock Fedora containers pull Cisco's OpenH264 through the
  RPM's Recommends.
- The AppImage is 13.8 MB larger than nightly 317, and its idle family is
  within 0.6 MiB.

Blocked or not run:
- the Debian-built DEB's recording flow (a pre-existing `Settings::validate`
  crash, fixed in #76; only capabilities and KWin authorization ran);
- the Ubuntu 24.04 DEB on Mint;
- the Ubuntu 26.04 Wayland VMs;
- AMD/NVIDIA GPU encoding from the AppImage;
- A/V sync, MCP end to end and browser playback.

## Single-name configuration and data folders — 2026-10-18

The [issue 56 receipt](evidence/2026-10-18-issue56-storage-locations.md) records
candidate `d37a975`:

- Workstation dev suite 73/73 on `d37a975` (671 s). The first candidate, `d7366b8`, failed
  `history_contract` (a null `previewPath` added to schema-1 records); `d37a975`
  fixes it. The new storage-locations suite covers a
  fresh install, an upgrade that keeps settings, history and failed or interrupted
  uploads, both roots in use, a stop before every rename, staged conflicts, linked
  and shared roots, and the real resident and CLI.
- Native Cinnamon/X11 on the workstation desktop: a profile written by the
  pre-fix build `625b7ff` moved on first start. Settings showed the new
  configuration path and `migrated` diagnostics. History kept both captures and
  the moved preview.

GNOME, KDE, build-server and package-upgrade runs were not part of this change;
package upgrade acceptance stays with issue #44.

## Inherited capture output in Settings — 2026-10-18

The [issue 58 receipt](evidence/2026-10-18-issue58-inherited-capture-output.md)
records candidate `48c2b57`: `ctest --preset dev` 72/72, the MCP suite 3/3, and a
native check on the workstation's Cinnamon X11 session with an isolated instance
and synthetic destinations. Inherited per-action cards stayed visible with their
controls disabled, followed Default capture output live, and unchecking saved an
explicit exception; `ui.viewState` and MCP `get_view_state` reported the same
state. GNOME, KDE, Wayland and memory were not tested.

## Wayland recording regions and system audio — 2026-10-18

The [Wayland region receipt](evidence/2026-10-18-wayland-recording-region.md)
covers the follow-up on `claude/wayland-recording-region`: region selection
before the stream (KDE layer-shell selector, GNOME Shell extension), ring,
dimming and bar placement with `overlayPlacement`, and the synthetic A/V-sync
check. Verified with a moving pattern and each clip's motion score on the
physical Debian Plasma 6.3 and Fedora GNOME 50 Wayland desktops, the
Ubuntu 26.04 GNOME, Kubuntu 26.04 Plasma 6.6 and Fedora 44 GNOME KVM guests,
and system audio through the speaker and headphone ports on the physical
Kubuntu, Debian and Fedora desktops (headphone route not measurable on
Fedora, where WirePlumber moves the default sink). Windows, microphone
capture and listening tests with real headphones remain uncovered.

## Screen recording — 2026-10-18

The [issue 50 receipt](evidence/2026-10-18-issue50-screen-recording.md) records
candidates `8252f03` (X11 capture, review, export, tray) and `625b7ff` (Wayland
acquisition) on pull request #68:

- Full suites on the Kubuntu, Fedora and Debian builders and the workstation
  (70–73 tests each, all green), plus pull request CI on the head.
- Capture scenarios on the Cinnamon guest, the GNOME and KDE Wayland guests and
  the physical Kubuntu, Fedora and Debian desktops; the recording flow with a
  moving pattern on the Cinnamon guest, the workstation and the physical
  Kubuntu desktop (motion 0.48–0.63 against a 0.012 static control).
- Wayland: the portal with one picker click, then restore tokens and the
  KWin/Mutter native streams reaching `ready` with no input on the physical
  Debian Plasma and Fedora GNOME desktops, clips carrying the pattern.

Windows, microphone capture and AMD GPU export remain uncovered; the Wayland
guests were rerun by the follow-up above.

## Resident PNG readers — 2026-10-17

The [resident PNG receipt](evidence/2026-10-17-resident-png-codecs.md) records the
kept reader change `e9a4dac` and the dropped libpng writer. The writer prototype
passed the Mint dev suite 69/69 and 18/18 scenarios each on a Mint Cinnamon VM and
the physical Debian Plasma desktop. It saved only the 220-436 KiB of plugin files
there, so it was removed. The reader-only change passes its three plugin tests and
the affected capture, clipboard, GNOME, KDE and editor suites.

## KDE window frame check — 2026-10-17

The [KDE window frame receipt](evidence/2026-10-17-kde-window-frame.md) records
the decision to keep frame-inclusive window capture on Plasma/Wayland, and the
lab fix. The runner now asks KWin for the frame on any observed KDE Wayland
session, not only a profile named `kde-wayland`. Window runs pass 2/2 on
physical Debian Plasma 6.3 (642×449) and 2/2 on the Kubuntu Plasma 6.6 VM
(640×448), each matching KWin's own frame geometry.

## X11 clipboard image plugins — 2026-10-17

The [X11 clipboard receipt](evidence/2026-10-17-x11-clipboard-plugins.md) records
candidate `d11940f` on `feat/edit-and-upload`:

- Dev suite 64/64 on Mint.
- In parallel: Cinnamon VM, physical Kubuntu Plasma/X11, physical Debian
  Plasma/Wayland and physical Fedora GNOME/Wayland, each with native cases, views,
  restart and two 300-second memory windows (25.9–37.6 MiB peak).
- Plasma/X11 resident 10.2 MiB smaller after its first clipboard import, with no
  image plugin mapped. The one Debian window failure was a lab expectation tied
  to the physical profile name, fixed in the
  [window frame receipt](evidence/2026-10-17-kde-window-frame.md).

## Edit & upload — 2026-10-17

The [issue 51 receipt](evidence/2026-10-17-issue51-edit-and-upload.md) records
candidate `329d736`:

- Dev suites on Mint, Kubuntu and Debian.
- Cinnamon-first graphical (21/21), memory and MCP gates, then GNOME/Wayland and
  KDE/Wayland on KVM.
- The same gates on the physical Debian Plasma/Wayland and Kubuntu Plasma/X11
  desktops, including both halves of the issue #54 outline fix and the
  wlr-data-control clipboard fallback.

Physical Fedora and Windows remain uncovered.

## Local S3 settings controls — 2026-10-16

The [scoped receipt](evidence/2026-10-16-s3-settings-controls.md) records the
user-approved saved-secret reveal, visible connection-test progress/success,
actionable sanitized S3 errors and synthetic regression checks. Current-candidate
graphical and resident-memory acceptance remains pending; prior desktop evidence
does not cover this change.

## Clipboard tray wording — 2026-10-16

The [issue 36 receipt](evidence/2026-10-16-issue36-tray-label.md) records the exact
**Upload Clipboard** label, two passing tray suites, and a real Cinnamon menu
selection with synthetic local-save and independent paste verification. Memory
testing was waived by explicit user instruction for this string change. This
focused development check makes no wider desktop or release acceptance claim.

## Unified UI acceptance — 2026-10-16

The [issue 40 receipt](evidence/2026-10-16-issue40-redesign-acceptance.md) records
frozen product source `1010842`, including the History clipboard ownership and
16-bit precision corrections: full
58/58 Dev and ASan/UBSan suites, audited native
packages, Cinnamon-first functional/memory gates, then GNOME/Wayland and KDE/Wayland
acceptance with independent paste and two five-minute post-use observations each.
The JSON companion binds package, running-process, module and helper identities.
The wider physical, mixed-display and U9/U10 release gates below remain separate.

## Current acceptance index — 2026-10-13

The historical source108 GNOME/KDE TLS failures were superseded by the passing
[source114 transport retest](testing-live-s3-transport.md). Keep their failure
receipts as diagnostic history; they are not current release blockers.

| Scope | Latest linked evidence | Limit |
| --- | --- | --- |
| Build and automated contracts | [Main b1ed4ae Build run](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/38438946727), all four jobs passed | Windows/macOS compile shared code only; CI is not compositor proof |
| Cold first capture (#3) | [Unable-to-reproduce closure](https://github.com/patrick-hudson/put.gd-desktop/issues/3#issuecomment-6149342985): b75c0dd, 3/3 cold runs each on GNOME/KDE, exact image/native-paste comparisons | Does not prove the S3 fix caused the historical timeout to disappear or rule out a rare race |
| Live S3 and retained memory (#4) | [Closure and exact scope](https://github.com/patrick-hudson/put.gd-desktop/issues/4#issuecomment-6152173663): source114 full workload plus bounded b1ed4ae-tree Cinnamon/GNOME/KDE checks | The newer check covers region/text publication, restart/history/deletion and two idle windows per desktop; it is not a new full geometry matrix |
| Release and fresh-agent acceptance (#5) | [Dated audit](evidence/2026-10-13-issue5-acceptance.md): clean SDK/dev/release builds, production CLI/MCP/live S3, Cinnamon upgrade with 10,000 records | Initial unexplained auto-copy suppression blocks sibling expansion; physical hardware and full geometry/performance acceptance remain separate gates. Remaining findings and acceptance work: [#24](https://github.com/patrick-hudson/put.gd-desktop/issues/24) |

Full release readiness has not been established. The requirements remain in the
[approved plan](plans/2026-10-07-1926-feat-qt-linux-share-plan.md), even though
[issue #1](https://github.com/patrick-hudson/put.gd-desktop/issues/1) is closed.
[Lab storage safeguards](lab-storage.md) gate capacity and verified retention.

## Proton evidence reconciliation — 2026-10-15

The [Proton register](proton-drive-release-gates.md) credits the merged integration
and the source-specific P1 recovery/recipient, P2 resource and P3 source/rebuild
receipts. The [final P4/P6 receipt](evidence/2026-10-15-proton-p4-p6-acceptance.md) records the actual packaged
experimental Proton candidate `a5112bc`: source publication/extracted production
rebuild, package inventories, Cinnamon-first functional/memory acceptance then
GNOME/KDE, Fedora/AppImage smoke checks and secure test-session cleanup. P5 credits
the actual completed Crypto maintenance transition. The official JS helper is
retained; the native trial remains separate and unadopted.

Merge the local wrap-up changes before closing #10. No tagged application release,
full hosted CI, physical/current-main certification or new S3 regression result
is claimed. The wider project release gates above remain in force.

## Historical implementation checkpoints

The entries below describe what was known at each implementation checkpoint.
Their version-specific failures and pending statements are preserved as history;
use the index above and later dated receipts to determine what superseded them.

| Unit / slice | Observed evidence | Remaining gate |
| --- | --- | --- |
| U1 bootstrap | Mint 22.3, GCC 13.3, Qt 6.4.2, QtKeychain 0.14.2; AWS 1.11.907 plus 15 pinned submodules built; Debug contracts and no-test Release build passed; ASan/UBSan contracts passed outside process sandbox | Authored CI jobs have not run on Ubuntu 26.04, Windows, or macOS |
| U2 shared services | Test-first service failures and returned/stored regressions; all 11 current executables pass Debug (16.47s) and ASan/UBSan with leak checking (37.67s); no-test Release builds; actual resident socket/CLI, asynchronous settings, manifest recovery, bounded acquisition/retry, opaque receipts/deletion, previews and 10,000-record history tested | Literal full-filesystem archive failure awaits the lab; GUI binding and production adapters belong to later units. All shared-service thread checks pass with instrumented dependencies; native adapters require separate acceptance |
| U3 S3 / secrets | Real pinned SDK and signed offline HTTP fixture; QtKeychain jobs through isolated D-Bus peer; full Debug 17/17 (37.00s), ASan/UBSan with leak detection 17/17 (67.81s), no-test Release build; adapter/secret-store TSan 5/5 (41.01s) | Real Mint GUI secret-store setup passed; live test returned AccessDenied before publication. Successful live S3/custom-domain workflow remains a gate |
| U4 native foundation | Native XCB/EWMH/RandR capture, unfocused keys, external-focus-preserving selector, bounded asynchronous clipboard transfers and composition; full Debug 19/19 (40.69s). Final changed operations/geometry/native targets pass ASan/UBSan with leak detection 3/3 (11.25s) and TSan with instrumented Qt/X11 3/3 (12.47s). Four reviewed simplifications preserved those checks | Named Mint guest capture/clipboard/paste checks passed. Physical workflow, successful live S3, mixed-display acceptance and release budgets remain gates |
| U9 headless MCP slice | Locked official Python SDK stdio discovery and strict public argument schemas; 12 tools forwarding argument-vector production CLI calls; actual SDK stdio plus production CLI against a synthetic Unix peer: 3 tests pass | Actual resident/native effects, U6 graphical instructions, and a fresh-agent walkthrough remain required for full U9 acceptance |
| U5 settings / resident GUI | Real settings/service widgets, independently routed local folders, session/keychain form, history paging/deletion, copy-only retry, observed-revision conflicts and startup refresh; current Debug23/23 (44.21s). Affected7 memory/thread checks passed, then final shared-copy/core3/3 memory38.15s/thread90.02s; final GUI history6cases Debug2805ms/memory8851ms/thread27190ms. No-test Release builds | Actual Mint settings, OS keyring consent, notifications and independent native paste witnessed; login, remote history and custom R35 visuals remain gates |
| U6 lab / development package | Authenticated Mint22.3 ISO, real installed Cinnamon6.6.4/KVM guest, console-pinned SSH/public lab key; normal CPackDEB generated with distroQt/privateofficialAWS runtime; host lab6contracts pass. Foreground-process interruption was diagnosed and replaced with normal QEMUdaemon/pidfile ownership | Two independent disks and region/window/desktop/text/image/two-file native paste checks passed; mixed-display and timeout/crash evidence remain gates |
| Current native/UI wave | GNOME and KDE production adapters compile; bridge/FD contracts pass. Integrated Debug25/25 (44.16s), affected memory6/6 (17.26s), thread5/6 plus real-font history1/1 (25.80s); final changed S3/UI memory2/2 (10.34s), thread2/2 (21.13s). Native Ubuntu Qt6.10 full25/25 (36.84s). All six GNOME single-head local acquisition/paste workflows pass; Escape and right-click cancellation preserve the independent text source | GNOME GTK interop, zero-area cancellation, ownership/lifecycle/display matrix and live publication; actual KDE, complete MCP acceptance, physical performance and release lifecycle evidence |

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

The user added R35/AE15 on 2026-10-08: the original functional forms were scaffolding for
a polished custom Qt visual design. Their contract tests do not satisfy the final
rendered visual review. The user also provided Ubuntu26.04.1 and Fedora44 installation
media. Both SHA256 values match good official checksum signatures, pinned in image-lock.
Their golden disks are installed and disposable overlays are used for testing; no Wayland acceptance
is claimed from checksum verification. credentials.txt is explicitly ignored and its
commit/export is prohibited in AGENTS.md; it remains absent from the Git index.

Actual zero-deadline lab readiness failure preserved JUnit/results/QMP witness and
stopped the owned second Mint QEMU process. The disposable disk and prior scenario
evidence remain available. The live capture operation reached partial: local PNG saved,
S3 prepublication check denied, no result copied. This is failure/recovery evidence,
not a successful S3/public-link test.

## Current native desktop and visual checks

The custom Qt visual system has been inspected in installed Mint and Ubuntu
applications. Settings, activity and populated history use the same design.
An actual compact-history preview/expanded-controls failure was fixed; the installed
Mint witness fits 1000×700 with a visible preview. Offscreen GUI tests retain strict
layout and keyboard checks. Full AE15 scale/error/confirmation-state inspection
remains pending. Shortcut statuses now show readable per-action results with exact
JSON behind a keyboard-accessible advanced disclosure.

A fontless instrumented Qt run expanded history to 1045 pixels and failed its
unchanged assertion. The same thread-checked test passed with installed DejaVu fonts
selected through QT_QPA_FONTDIR. No layout assertion or race/leak filter was relaxed.

Ubuntu's native Qt6.10 build exposed a missing direct QElapsedTimer fixture include
and an unsigned public-GET check that spent six seconds discovering EC2 metadata.
The include is explicit and the SDK configuration disables unnecessary IMDS lookup,
as the configured S3 client already did. The original five-second synthetic probe
test then passed; its timeout was not increased. Native packaging also aligns the
desktop executable with the configured installation prefix.

Authenticated Mint, Ubuntu and Fedora images have been installed, booted into their
native desktop, powered down cleanly and checked/hashed. Tests use disposable qcow2
overlays. Kubuntu's one-time installation also completed, with fresh automatic Plasma
6.6.6/Wayland login and clean disk check. This establishes base provenance,
not full desktop support. Credentials are absent from base disks and Git.

GNOME50's removed grab API caused a genuine region-selector TypeError. The helper
now uses the public is_revoked API, cleans up failed setup, and preserves half-open
drag geometry. Native operation3ae7b7c4-aa27-40dd-92af-1de2fab42675 produced an80×60
image with expected pixels and an identical independent Qt paste. Desktop operation
8374ea3c-6d17-4b03-9677-aa8c4f8fb513 likewise produced1280×800 and identical paste.
The runner locates complete synthetic paint in a pre-action QMP image because Qt
on Wayland cannot report the global window origin. Active-window operation
e73d7b29-b38c-41b2-9395-4c2436dded50 produced662×480, with dimensions derived from
the pre-action paint and the fixture's native frame margins; independent paste matched.

A real clipboard import failure exposed use of an absent Meta.SelectionType.CLIPBOARD
enum. The installed Mutter18 typelib exposes SELECTION_CLIPBOARD. Correcting those
three references restored native text,96×64 image, and ordered two-file imports and
independent Qt paste. St.ClipboardType.CLIPBOARD remains correct and unchanged.
These are six named single-head local workflows, not complete GNOME certification.

Real selector cancellation exposed a durable pending copy status after acquisition
ended. A failing operation regression preceded the shared finish-path fix, which
settles pending failed/cancelled/interrupted results as no_result. The full operations
suite then passed Debug(4.59s), ASan/UBSan with leaks(5.06s), instrumented Qt TSan(6.75s),
and native Ubuntu(3.32s). Installed Escape/right-click cancellation now creates no
artifacts and preserves independently pasted synthetic text. QMP button press/release
must be separated so the compositor receives both transitions.

### Latest native and physical evidence (2026-10-08)

The actual GNOME 50.1 Wayland guest passes all six local capture/clipboard workflows,
including independent GTK 4.22 native Wayland paste for text, images, and files.
Escape, right-click, and zero-area cancellation produce no artifacts and preserve
the independent clipboard source. Helper disable/re-enable during selection settles
the interrupted operation and restores all four bindings; disabled per-action
diagnostics now report unbound rather than retaining stale bound status.

The locked official MCP 1.30.0 helper was exercised over actual stdio against the
installed GNOME production CLI: discovery, content-free inspection, revision-checked
configuration updates/conflicts, clipboard invocation, operation identity, and
history parity pass. This does not complete the Cinnamon, fresh-agent, or remote
upload/deletion acceptance gates.

The physical Cinnamon release resident measured 84,783,104 bytes RSS (80.9 MiB)
and 0.009995 percent of one core over 300 seconds, with empty history and no
capture/network invocation. The original idle budgets pass for that scope. The
user requested approximately 40 MiB; optimization and populated-history/capture
performance checks remain pending. The native KDE Qt 6.10/LayerShell build and
25/25 tests pass in Kubuntu. All six named single-head local KDE workflows now
pass with independent Qt paste: exact 80×60 region, independently queried
640×448 active-window frame, full 1280×800 desktop, text, image, and two files.
Native checks also prove saved shortcuts rebind and a foreign shortcut is not
stolen. The region check caught fractional-pointer rounding; the corrected result
has identical capture/paste hashes. Each new attempt retains parameters, JUnit,
JSONL events, witnesses, and a checksum manifest. This is partial KDE evidence;
mixed-scale/multiple-display, GTK, cloud, and lifecycle gates remain.
KDE Escape cancellation also passes with no artifacts and preserved synthetic text;
right-click and zero-area checks remain pending.

The refreshed host standard Debug suite passes 25/25 in 46.79 seconds outside
the process sandbox. A sandboxed attempt could not open local test sockets/Xvfb
and is not a code regression or a passing acceptance result.

The second UI pass removes slogan headers, compacts action/folder/destination layouts,
and passes six affected host tests (16.38s) and four native KDE GUI tests (7.14s).
Installed single-scale witnesses cover all settings sections and populated local
history/activity. Standard desktop icon installation fixes the taskbar icon; the
window-decoration glyph still needs investigation. Simple setup, human-readable
size controls, scale/error/confirmation acceptance remain pending.

A fresh independent agent exercised fixed production CLI/official MCP metadata
inspection, revision patches/conflicts and deliberate configuration-error recovery
in an isolated offscreen process. Its structured record reports14CLI calls,7MCP
requests and12discovered tools, with zero operations/provider calls. This is partial
U9 evidence; real Cinnamon, build and U6 lab walkthrough gates remain pending.

### Simple setup and measured performance follow-up (2026-10-08)

The simple destination form now opens with Advanced settings collapsed. Standard
S3 defaults and an explicit local-only choice share the validated settings draft;
expansion retains values. Credential storage remains a separate explicit action.
MiB/GiB controls preserve exact byte counts, including previously accepted odd-byte
values; activity/history summaries use readable units without changing JSON receipts.
The native KDE test first failed for missing disclosure and size controls, then
passed after implementation. The host full suite passed 25/25 (40.47s); the final
compact form passed the three affected host GUI targets (6.17s) and native KDE Qt
targets (5.16s). Those widget tests run offscreen and do not certify KWin rendering.
The standard CPack DEB was installed and its binary fingerprint verified. Installed
single-scale inspection is partial: complete scale, focus, error, and remote-deletion
visual acceptance remains open. Simple setup is implemented; AE15 is not complete.

The physical capture supervisor records private isolated configuration, hardware,
binary hashes, parameters, logs, operation IDs, and result data. The final actual
Cinnamon three-head run completed five warmups and 30 measurements with 35 cancelled
operations and no saved payloads. Its p95 painted overlay latency was 235.821ms,
which fails the 150ms budget. Earlier fixture-readiness failures remain recorded.
The fixture background is labelled neutral grey. The actual KDE selector was
separately inspected over the normal desktop with a drawn rectangle. Capture
latency still requires attribution and correction; this is not a passing U10 gate.

The VNC viewer showed fragmented incremental updates after a KDE window drag,
while contemporaneous direct QMP framebuffer witnesses remained clean. The exact
viewer/codec cause is unconfirmed. Investigation was parked at the user's request;
no viewer source was changed and this evidence does not establish a Qt paint bug.

An unchanged 300-second physical Cinnamon idle comparison at baseline 1e3735d measured
87.0 MiB peak. Deferred AWS initialization measured79.0 MiB peak and 0.1366% of one core,
passing the original idle budgets. All 25 existing release tests passed (47.83s),
as did 12 deferred runtime generations, 3072 concurrent initialization calls, eager
and unused-deferred lifecycles, and the production safe DELETE factory. No network
requests occurred. This candidate is retained on the isolated optimization branch
at 23102be. The 40 MiB
target is unmet. The earlier lazy-view candidate remains rejected because its
five-minute CPU gate failed; short diagnostics do not replace that result.

The next candidate constructs each persistent view on first request. Independent
review identified a GNOME shutdown callback that outlived its captured window
owner; a context declared after the view owners now disconnects it before teardown.
On the retained SDK baseline, the unchanged 300-second measurement passed at
58,671,104 bytes (55.95 MiB), with 0.00333% of one core. This is 23.05 MiB below
the SDK candidate and 31.06 MiB (35.69%) below the original 87.0 MiB baseline.
All 25 release tests passed (46.75s). Actual physical Cinnamon activation mapped
all three requested views and reused the same windows on repeated requests, with
no capture, clipboard-content access or network request. The optimized snapshot
is 016edb8; canonical integration is 9c1b128 plus d6c7519. Its fingerprint predates
the latest simple form changes, whose separate GUI evidence is above.

The bounded batch ended after three experiments: two retained and one degenerate.
The remaining one-hour budget could not fit another full measurement. The 40 MiB
target remains open, and neither empty-history results nor native mapping checks
establish upload/populated-history memory or complete visual acceptance. The
[structured comparison](evidence/2026-10-08-resident-memory.json) retains parameters,
hashes, time series, page attribution summaries and the failed candidate.
These savings describe a background resident before views or S3 are first used.
Views remain cached to preserve drafts and selections; AWS stays initialized after
first S3 work. These measurements do not establish a reduction after those actions.
The integrated current dev build passed all 25 targets (48.02s), including the new
unused/rejected SDK-owner and barrier-started concurrent first-use regressions.
The four affected SDK/GUI targets also passed ASan/UBSan with leak detection
(14.33s) and the documented instrumented-Qt TSan environment (42.72s). The release
AWS SDK/OpenSSL/libcurl remain uninstrumented; this does not certify their internal
race freedom. No new suppression was introduced.
The focused Compound Engineering review combined a local correctness read with
an independent Anthropic Claude Opus 5.5 pass and returned no findings for the
six-file optimization slice at 016edb8. Its
[receipt](evidence/2026-10-08-memory-review.json) records exact scope and limitations;
it does not certify whole-product readiness or the later permanent-test diff.


A fresh agent followed the normal dev build, CPack installation, native Cinnamon
lab and locked official MCP instructions on a new KVM overlay visible at VNC5935.
The host 25-target suite passed. Native region capture and independent Qt paste,
then GTK text, image and ordered-file acquisition/paste passed. The GTK fixture
initially requested an unadvertised URI format and accepted an empty stream;
filtering its preference list by actual advertised formats fixed negotiation.
The original failures remain preserved, and byte/pixel/paste comparisons were
unchanged. Eight lab contracts and Python compilation passed for the fixture fix.

The actual graphical resident exposed all 12 MCP tools. An action through the
official SDK yielded the same operation result as the production CLI. Stale
configuration revision rejection, invalid reload recovery and visible settings
refresh passed. The stdio client needed the explicit resident socket because its
minimal child environment omitted XDG_RUNTIME_DIR; the documented endpoint option
worked without launching another resident. The
[structured receipt](evidence/2026-10-08-fresh-cinnamon.json) retains run parameters,
package/binary hashes, operation IDs, failures and exact limitations. This local
single-head subset does not complete full U6/U9, remote publication/deletion, the
compositor matrix or release lifecycle acceptance.

### Settings, filenames and image ownership refinement (2026-10-08)

The simple form now exposes editable S3 fields on first use, with searchable
region suggestions and standard provider defaults. Detailed adjacent info icons
are reachable by keyboard; plain labels, neutral charcoal surfaces and a blue
accent replace the green treatment. Cancel discards unsaved fields and clears
credential input. Reload from file rereads accepted external configuration;
an accepted save shows a blue confirmation banner. The installed Cinnamon
1000×700 window was inspected at scale 1 on a 1280×800 display through VNC5935.
Witnesses cover fresh setup, region selection, help, advanced filename patterns,
save feedback, unsaved Cancel/reopen and Reload. Full AE15 scale/state acceptance
and final user visual approval remain open.

Fresh configuration preserves imported file names and uses timestamped capture,
clipboard image and text names. Optional templates share the validated JSON
contract; existing generated-mode settings retain their choice. Names use the
accepted action's local timestamp even when queued, then persist for retries.
Local archive collisions preserve existing files. Review identified schema drift,
queue-time naming and a History reopen/cancellation defect; each has a genuine
failing regression and a tested correction.

Image payloads move through acquisition and are cleared after immutable disk
persistence, before preview/archive/network consumers. Cancelled queued images
release immediately. A configurable aggregate queued-image budget defaults to
64 MiB. The cleanup-callback test proves decoded pixels are freed while upload
is still blocked and retry reuses the same disk artifact. History decodes one
selected bounded thumbnail at a time and clears it when hidden. The 10,000-row
metadata ledger remains tested. This proves ownership and admission behavior;
it does not measure allocator page return, post-use RSS or complete the 40 MiB goal.

The final standard Debug suite passes 25/25 (49.71s); eight affected contracts pass
ASan/UBSan with leak checking (37.47s) and the documented instrumented-Qt TSan
environment (82.72s). The optional actual-resident/CLI JSON schema test passes
1/1 (0.256s). No new sanitizer suppression was added. Production continues to
use distro Qt; the release SDK/OpenSSL/libcurl are uninstrumented. The rebuilt
standard CPack DEB was installed and its binary hashes verified in the disposable
Cinnamon guest. Native region/Qt paste (80×60) and clipboard-image/GTK4.14 X11
paste (96×64) pass with matching pixel hashes and correct timestamped local names.
The lab's original configuration was preserved and its resident restored.

The [structured record](evidence/2026-10-08-settings-refinement.json) retains source,
binary and package fingerprints, commands, parameters, failure/green log hashes,
JUnit hashes, controlled GUI inputs, screenshots and separate review/fix receipts.
Only synthetic local workflows were exercised here. Successful live S3, full
desktop/profile, release lifecycle, MCP and physical performance gates remain open.

### Memory continuation (2026-10-08)

The final reviewed SDK module completed the same-observer five-minute physical
Cinnamon cold workload at 54,407,168 bytes (51.887 MiB), compared with 59,002,880
bytes (56.270 MiB) for its reference repeat. CPU was 0.003331% of one core, with
zero operations and no observed mapped owned window. The 7.789% improvement is
retained at optimization revision `cadd9ed` and integrated into canonical source.
The original 51.996 MiB candidate result is preserved rather than replaced.
Native Cinnamon fonts and dialogs remain enabled; approximately 40 MiB is unmet.

The actual review's two P2 findings were fixed: package mismatch errors explain
restart recovery, and wrong-build/wrong-transport rejection fixtures fail when
the corresponding guards are removed. Eight affected SDK contracts pass in
Dev, Release, ASan and TSan; installed-package checks pass. The combined canonical
SDK/live-region/cleanup development suite passes 26/26 (112.90 seconds).
[The SDK record](evidence/2026-10-08-sdk-memory.json) retains exact source, binaries,
review and raw measurements. Third-party uninstrumented library limits remain.

Three empty-view cycles on the reviewed binary finish at 79.551 MiB; no operations
or cloud requests occurred. This is a view-lifecycle diagnostic, not a permanent
plateau or leak proof. The module remains pinned after first S3 use, so cold gains
do not establish post-S3 memory savings. The [replay instructions](testing-memory.md)
describe private inputs, timing, logs and the physical-machine/CI boundary.

### Live selection and post-use cleanup (2026-10-08)

The region selector now leaves the desktop live while drawing its white outline.
Native Cinnamon proof changed the independent fixture color during dragging,
then verified both normal and rapid-release saved pixels and independent Qt paste
contained the final color without the outline. Escape and right-click cancellation
preserved clipboard text. The single-head record is in
[live-region evidence](evidence/2026-10-08-live-region.json); fresh GNOME/KDE
graphical proof, mixed displays and the physical latency gate remain open.

After five local capture/clipboard cycles, production idle heap reclamation
finished at 66.473 MiB in the disposable Cinnamon lab, versus 75.648 MiB in the
earlier attribution run. It performs one glibc pass after work drains, retaining
live UI and native clipboard state; the longest observed call was 750 microseconds.
This comparison uses different binary identities and establishes neither cold
40 MiB acceptance nor post-S3 savings. A test-first cancellation fix also covers
acquisition that never enqueues a payload. Final coherent development tests pass
25/25 (81.39s); affected operation ASan/UBSan with leak detection passes 1/1
(24.70s), and instrumented-Qt TSan passes 1/1 (26.34s), with no new suppression.
[The cleanup record](evidence/2026-10-08-heap-reclaim.json) retains source identities,
complete controlled samples, JUnit reports and the cancellation test delta.

A test-first lab-finalizer correction also verifies that input cleanup failures do
not skip restoring the original resident/configuration. Five actual-finalizer AST
fault cases pass, including restoration failure and startup that never completed.
Cleanup failures are saved as failed runs and still propagate. This is synthetic
orchestration proof; the prior native payload measurement remains separately labeled.

### Combined memory and live-selection checkpoint — 2026-10-08

The Linux native IPC implementation is committed as `1fdfff0`; live transparent
selection and one-pass image-heap reclamation are committed as `2e789b8`. Source
closure checks match the earlier full/follow-up reviews and the separate IPC
review, including the successful Claude retry and reproduced disconnect fix.
The combined Debug suite passed 28/28 in 127.35 seconds; the four affected
Release contracts passed in 46.79 seconds. Separate IPC ASan/UBSan and
instrumented-Qt TSan runs each passed 4/4; those are not a new whole-tree
sanitizer certification.

The isolated fixed IPC source measured 46.020 MiB against a paired 52.012 MiB
reference over five minutes. The committed combined
source measured 46.188 MiB over another 300 seconds at 0.00666% of one core,
without operations or owned window activation. Full identities are recorded
in [the IPC evidence](evidence/2026-10-08-local-ipc.json). Keep the
40 MiB target, post-S3/populated-history memory and full desktop acceptance open.
The temporary region trace still fails the unchanged 150 ms warm p95 gate at
160.004 ms; it points to journal initialization overlapping a prior cancellation
commit, with individual permission/PRAGMA waits not yet resolved.

### Startup cleanup checkpoint — 2026-10-08

Successful startup now requests one pass through the existing two-second idle
cleanup gate on glibc. Failed initialization requests none; active workers,
uploads, selectors and shutdown retain their existing protections. Three new
success-path assertions failed before the change; the failed-startup test passed.
The reviewed change is kept as `68e5549` and integrated as `fc69d38`.

The unchanged five-minute physical Cinnamon workload fell from 46.1875 to
44.9648 MiB, a 1.2227 MiB saving. CPU was 0.0133% of one core. The predeclared
1 MiB threshold and existing idle budgets pass; the 40 MiB target does not.
Native GTK remains mapped. Three empty-view cycles reopened Settings, History
and Activity and ended at 73.8242 MiB hidden RSS; that is a distinct post-use
measurement, not cold idle or a long-session plateau.

Integrated development tests pass 28/28 (144.57s). The affected operations
contract passes Debug and Release, ASan/UBSan with leak checking (37.56s), and
instrumented-Qt TSan with unchanged suppressions (39.47s). These are bounded
application checks, not new desktop/cloud or whole-dependency certifications.
[Complete evidence](evidence/2026-10-08-startup-reclaim.json) preserves exact
source/binary identities, tests, review and both physical workloads.

### On-demand UI implementation checkpoint — 2026-10-08

Settings, History, Activity and About now run in `putgd-ui`. The resident owns
the tray, capture actions, shortcuts, clipboard, configuration and accepted work.
Closing the final view checkpoints bounded safe state and releases the helper.
Strict build agreement, bounded startup/activation, explicit shutdown and typed
error recovery preserve the resident command interface. Typed credentials and
image buffers are excluded from retained drafts.

Development tests pass 41/41; the 16 affected ASan/UBSan and instrumented-Qt TSan
contracts pass. Release builds and the separate three-case MCP integration suite
pass. A production-helper Xvfb test checks four ordinary close/reopen generations,
safe drafts, zero child exit, clipboard ownership and completion of accepted work.
The GTK debug startup failure was reproduced and corrected using private control
descriptors established before QApplication. These are headless/isolated checks,
not current Cinnamon/GNOME/KDE acceptance.

[The structured checkpoint](evidence/2026-10-08-ui-process.json) retains the
completed review, four confirmed findings and caller fixes, command scripts,
JUnit results, source hashes and a 15-case total-process sampler test receipt.
GNOME ownership-clear and expired setup-result recovery remain scheduled fixes.
Graphical AE17, deterministic GNOME selector readiness, total-process memory and
reopen latency remain open. The 40 MiB target is not yet met.

The two conditional review concerns now have bounded recovery fixes. Setup
inspection retains accepted jobs across transient errors, removes expired IDs
without starving later results, and never repeats the accepted action. Its three
new regressions failed before the fix; the full Settings suite and affected
ASan/UBSan and instrumented-Qt TSan checks pass. GNOME now retries failed UI-PID
clearing through the existing probe. A native Qt Wayland/private-D-Bus regression
reproduced the failure and now passes all three recovery scenarios, including
newer PID and Shell-owner protections. See the [setup receipt](evidence/2026-10-08-ui-setup-recovery.json)
and [GNOME receipt](evidence/2026-10-08-ui-gnome-recovery.json). These checks do not
close the pending current-source graphical or total-process memory gates.

### Current-source UI split native wave — 2026-10-08

The frozen build is `0.1.1-dev.41+g97ca3a521714`, full commit
`97ca3a521714e2717cedc1139976130627bfb8a8`. Its clean host Dev suite passes
41/41 and the locked MCP suite passes 3/3. GNOME and KDE each pass 40 guest Dev
tests; the host-only GNOME ownership contract requires Node, absent in those
guests. All three executables and packages have independent checksums. Older
app residents were stopped after identity and terminal-operation checks; the
three existing test overlays were reused without changing their golden disks.

Cinnamon/X11, GNOME/Wayland and KDE/Wayland each pass the current 12-case native
capture/clipboard matrix and four actual tray actions with the UI absent. All
four views open and close normally, leaving the original resident and tray alive.
Safe drafts, external-edit conflicts, Cancel, native folder dialogs, own-window
and own-modal capture exclusion, and real clipboard paste after helper exit pass.
On each desktop, an accepted two-file local save remained active after ordinary
UI exit, then completed with exact sizes and hashes: one 512 MiB streamed fixture
and one 23-byte file. This establishes local-work continuity, not slow S3 upload
continuity. Cinnamon's additional user-confirmed manual region is excluded from
scripted acceptance; its payload was never read, hashed or exported.

Quiet post-use VM observations each contain 25 family samples over 120 seconds,
with no helper/escaped child and stable operation/UI guards. RSS/PSS/private are
57.65/37.32/35.91 MiB on Cinnamon, 65.32/38.77/35.22 MiB on GNOME and
145.50/45.17/32.76 MiB on KDE. KDE's LLVM mapping contributes 48.28 MiB RSS but
5.98 MiB PSS; retain proportional/private figures alongside RSS. These are
post-capture/local-save observations, not matched historical VM savings or the
physical cold 40 MiB target. GNOME's successful sampler was followed by a host
launcher `Popen.check_returncode` error; retain it and the corrected future replay
driver. No timed sample was repeated to hide that error.

[Current-source receipts and raw replay evidence](evidence/2026-10-08-ui-process-native-memory.json)
distinguish these demonstrated subsets from full AE17. Real S3 publication and
precise deletion, native crash/version/typed-credential cases, the historical
GNOME readiness/focus concerns, mixed displays and remaining release gates stay
open.

The matched physical Cinnamon comparison passed all four serial runs, two per
build, with three Settings/History/Activity cycles each. Median final closed
RSS/PSS/private falls from 65.09/40.40/37.59 MiB to 45.04/25.40/22.89 MiB: savings
of 20.05/15.00/14.70 MiB. Retain the split for this measured closed-state benefit.
Maximum sampled open RSS/PSS is higher: 120.84/55.01 MiB versus 73.19/44.49 MiB.
Four repeated Settings reopens per build have mapped-readiness medians of
332.68 ms versus 100.92 ms. These include observer overhead and do not certify
completed painting or p95 latency. The separate 300.605-second cold family run
measures 44.390625 MiB peak RSS and 0.00998% of one core; original idle budgets
pass and the 40 MiB target fails. Native fonts/dialogs remain enabled. Raw
commands, complete time series, grouped latency events and a measured decision
are retained in the linked current-source record.

A four-run standard CMake/Ninja Qt styling cost probe finished once, without
product changes or native-font/dialog overrides. Settled full-versus-native RSS
differs by 0.63 MiB, with 0.19 MiB immediate full-style growth. Preserve custom
styling: this five-second temporary-tray diagnostic does not establish product
savings or a route to 40 MiB. Exact source/binary hashes, stage timing, warnings
and replay instructions are retained separately under the physical evidence.


### GNOME test runtime correction — 2026-10-08

The earlier source-41/source-43 suites used a Node-based isolated extension
ownership fixture on the host. It was absent in the guests and did not establish
GNOME Shell acceptance. The user rejected that guest dependency. The fixture now
uses GNOME's existing GJS interpreter, without Node or JavaScript packages, and
runs where that native runtime is already installed. The shared C++/Qt tests
remain portable; real GNOME capture/clipboard tests remain separate from these
mocked ownership assertions. Do not install Node to fill a guest coverage gap.
All five ownership assertions pass under the guest’s existing GJS 1.88.0.
Host CMake configuration passes without Node lookup. [The receipt](evidence/2026-10-08-gjs-ownership.json)
keeps this result separate from the frozen source-43 native build.

### Live S3 and source57 Cinnamon checkpoint — 2026-10-10

The [source55 live S3 record](evidence/2026-10-09-live-s3-cinnamon.json)
uses the clean `0.1.1-dev.55+g033b73fb58a3` resident, commit
`033b73fb58a38f853bd79b1902d88117e723a122`, in the Cinnamon/X11 VM.
One synthetic 30-byte clipboard text upload passed unsigned public-byte
verification and automatic URL paste into an independent native receiver.
The actual History **Copy link** button passed after a clipboard reset;
guarded deletion removed the exact owned test object and left its History
record marked Removed. Original settings were restored, the UI exited normally,
the owned fixture was cleaned up and the resident exited normally. Earlier
AccessDenied attempts remain failure evidence; they no longer describe the
outcome of this specific successful workflow. Follow
[testing-live-s3.md](testing-live-s3.md) with fresh synthetic object identities.

The single post-cloud diagnostic was 49.14 MiB RSS. It was neither cold nor a
frozen memory workload and does not establish post-upload memory acceptance.
This source55 text workflow does not prove source57 cloud behavior, all content
types/collision/failure cases, GNOME/KDE remote history or MCP cloud parity.
No browser/CDN invalidation or credential-memory zeroization was tested.

The isolated clean source57 candidate is `0.1.1-dev.57+g2a02118ffc7b`, commit
`2a02118ffc7bcf3f4643b83f83d5dd7594a8207f`. Cinnamon's implementation is unchanged
by its GNOME startup/argument corrections. It passed twelve native local
capture/clipboard cases, with Qt and GTK independent paste, plus current tray,
simple/advanced settings, native GTK folder dialog/font integration, transparent
live selection, History preview and Activity parent/item result-copy checks.
Each Activity copy oracle first replaced the clipboard with an unrelated image.
Root inspected all 21 actual current-version witnesses. The native functional
receipt is SHA256 `6dacb8b11072b61c0141dd24235139a707918b9de23a56828a4415cc3c05d981`.
Failed attempts remain preserved. Two early confirmed-icon tray gestures showed
no popup; attribution remains unresolved. A controlled gesture opened the native
menu and Settings, and the menu reopened after normal Settings close.

The authorized one-pair physical Cinnamon regression measured:

| Workload | Actual result |
| --- | --- |
| Region-selection timing | 113.61 ms p95 over 30 measured invocations; all 35 warmup/measured operations cancelled with no result |
| Cold idle | 24.62 MiB peak RSS over 300.19 seconds; measured CPU 0.0% of one core; one process throughout and adequate sample coverage |
| Three empty-view cycles | 25.19 MiB worst settled RSS / 16.64 MiB PSS; 51.53 MiB peak open PSS |
| Separate normal-exit supplement | Three actual normal-zero helper exits; 25.10 MiB worst settled RSS / 16.59 MiB PSS; 51.66 MiB peak open PSS |

The original view run omits the final generation's exit status. The separate
unchanged-workload supplement adds three read-only queries and proves all three
normal exits; it retains the original result and labels its observer noise.
Its 27 synthetic safety contracts also pass. After cleanup, a fresh physical
inventory found no owned app/helper/capture/fixture/benchmark or known observer
processes, and the serial slot was released.

The [local source57 expansion gate](../work/gnome-native-boundary/cinnamon-candidate57/physical/cinnamon-expansion-gate.json)
has SHA256 `41041a41048f0cfb34d8c7ee514d6cb065c76c97b2d1f635c2f748b9b80123d3`;
root rehashed its 81 references. These raw receipts remain local work artifacts.
This establishes the approximately 40 MiB Cinnamon cold/empty-view regression
target and its timing guard, with native fonts/dialogs preserved. It does not
establish post-S3, post-capture or populated-history retained-memory bounds,
a permanent plateau, mixed-display acceptance, full AE17/U9/U10, or current57
GNOME/KDE acceptance. GNOME comparison/confirmation and KDE work remain separate;
their memory failures must not roll back the passing Cinnamon changes.

Source57 GNOME subsequently passed all fifty registered guest tests, the twelve
native capture/clipboard/cancellation cases and the local focused UI gates on
its verified Core route. Two separate 300-second post-use observations both
peaked at 32.77 MiB RSS, with 61 valid samples each and worst CPU 0.0334% of one
core. The [structured record](evidence/2026-10-10-gnome-core57-memory.json)
pins exact source, workload, process and original result hashes. The approved
30-minute extension completed the fresh empty-profile cold confirmation at
26.95 MiB peak RSS and 0.0067% of one CPU core over 300.000 seconds. All 61
samples passed coverage and exact identity checks. Independent numeric and
lifecycle review confirmed the external 5.119-second warmup, normal shutdown
and unchanged populated profile. Both preceding orchestration failures reached
no samples and remain preserved. Source57 is retained in its clean isolated
checkout; it is not integrated into main. This establishes the scoped GNOME VM
cold/post-local-native target, with Cinnamon implementation unchanged. GNOME
remote History, KDE and post-S3 memory remain open; see
[the replay procedure](testing-gnome-memory.md).

### KDE source57 memory baseline — 2026-10-10

KDE now uses the same clean source57 keeper. Its standard development/release
builds, package and all 49 registered tests completed; its registry matches the
host exactly. GNOME's additional GJS ownership contract is conditional and absent
on KDE. The installed package and live resident match
`0.1.1-dev.57+g2a02118ffc7b`. Twelve native single-head Qt/GTK capture, clipboard
and cancellation cases passed, along with focused tray, native dialog/font
integration, notification, live background, Activity copy and normal view-helper
exit checks. Single-item Activity checks do not prove multi-item subset copy.

The complete native crosshair is confirmed by actual cursor alpha. The installed
QEMU's legacy VNC mask keeps only fully opaque pixels and exactly reproduces the
incomplete crosshair shown on the display wall. That infrastructure limitation
and the rejected original mask observation remain recorded; no cursor image was
substituted and no product or host QEMU change was made.

One five-minute populated post-local-native observation measured 139.42 MiB
peak/final RSS, 33.96 MiB PSS, 21.15 MiB private memory and 0.0667% CPU of one
core. All 61 samples passed coverage and exact process/family identity checks:
299.958 seconds, maximum gap 5.050 seconds, one resident, no UI/helper and a
normal generation-four helper exit. Independent recomputation verified counters,
attribution hashes and native/source joins. Measurement integrity and CPU pass;
the 40 MiB RSS target fails. PSS does not replace the requested RSS target.

The [bounded record](evidence/2026-10-10-kde-source57-memory-baseline.json) and
[procedure](testing-kde-memory.md) preserve deterministic parameters, commands,
hashes, failed attempts and workload qualifications. Current cold and post-S3
memory, remote History, full geometry, physical KDE, canonical clean-checkout
lab replay, MCP and release acceptance remain open. No KDE optimization candidate
has been implemented. The retained Cinnamon and GNOME implementations are unchanged.

### Source114 live-S3 regression — 2026-10-12

Runtime `0.1.1-dev.114+g0258600b752d` passed all three native/GUI/live-S3 regression
workloads and exact production cleanup. Five-minute RSS+swap peaks were 29.059 MiB
on Cinnamon, 36.078 MiB on GNOME and 31.285 MiB on KDE, with zero swap/remaining
helpers and idle CPU below 0.0201% of one core. All 105 source114 synthetic objects
(including the targeted KDE 12) were uploaded, independently verified and removed
through production. No SDK error, automatic retry or emergency cleanup occurred.

[The transport report](testing-live-s3-transport.md) and
[source114 replay](evidence/live-s3-memory/source114/README.md) retain exact
identities, parameters, historical failures and qualifications. One current full
regression per desktop does not claim a fresh-repeat optimization decision or
full release/fresh-agent acceptance. Documentation commits do not rename the
measured binaries.


## Closed dropdown wheel input — issue #26 (2026-10-14)

The shared `VisualStyle::ChoiceBox` now ignores wheel input on the closed control,
so Qt can scroll its containing pane without changing the selection. Popup mouse
selection, keyboard arrows, and editable text entry retain their existing behavior.
A source audit found all current dropdowns in the Capture and Destinations settings
panes; History and Activity do not construct dropdowns.

On fresh `main` base `bc4215d`, the new regression initially failed for all 21 dropdowns.
The final focused run passes all 23 cases (25 QtTest results including setup/cleanup)
on distro Qt 6.4.2: each dropdown with and without focus, both wheel directions,
parent scrolling where space remains, and plain/editable deliberate selection.
QtTest's spontaneous-event marker and `qApp->notify` are required for the wheel
propagation check; `sendEvent` clears that marker. Earlier harness attempts are
retained under `/tmp/pg26-red`, `/tmp/pg26-red2`, `/tmp/pg26-green`, and `/tmp/p26v`.
The final focused command uses `XDG_RUNTIME_DIR=/tmp/p26v/r TMPDIR=/tmp/p26v/t
QT_QPA_PLATFORM=offscreen build/dev/test_gui_settings
closedDropdownWheelPreservesValueAndScrollsPane dropdownMouseAndKeyboardSelection`.
The fixed runtime path and equal-length templates for randomized test socket paths
passed the 107-byte preflight; the longest checked template is 77 bytes.

The standard development configure and full build passed using the existing pinned
AWS SDK and dynamically linked distro Qt. `ctest --preset dev` passed all 59 tests
in 446.92 seconds, including the full Settings, Activity, and History suites.
These are synthetic widget/core checks, not compositor or memory acceptance.
Current-candidate Cinnamon functional/memory acceptance and subsequent GNOME/KDE
regressions remain pending; no VM base was booted or changed for this slice.


## Column drag direction — issue #27 (2026-10-14)

Activity and History previously used a stretched first column. Growing another
column took the same width from that first column, moving the target's left edge
against the pointer; the first column itself was not interactive. Both views now
use shared `VisualStyle::configureColumns` sizing on their existing Qt headers.
It fits the first column when the
viewport changes width, preserving the compact layout, but leaves column drags
to Qt without redistributing their width. Native minimum widths still apply.

On fresh `main` base `7e5a6a0`, the new pointer regression failed all 20 cases
before the implementation change. It then passed for both directions on all five
visible columns in each view, including the first and last columns and drags past
the minimum width. The final regression also checks two positions during a held
drag, then the released boundary. The existing compact History test covers
window resizing and the receipt inspector at 1024 and 1280 logical pixels.

The standard development configure and full build passed. `ctest --preset dev`
passed all 59 tests in 452.34 seconds, including the complete Settings, Activity,
and History GUI suites. `git diff --check` passed.

Local build and test logs are retained in `/tmp/p27v`; the initial configure and
regression build logs are `/tmp/p27-configure.log` and `/tmp/p27-build-red.log`.
Tests use synthetic local-only payloads, distro Qt 6.4.2,
`XDG_RUNTIME_DIR=/tmp/p27v/r`, and `TMPDIR=/tmp/p27v/t`. Socket preflight passed
393 fixed paths/equal-length randomized-path templates, with a maximum of 77
UTF-8 bytes. Widget tests use the offscreen platform; they do not establish
compositor or memory acceptance. Current-candidate Cinnamon functional/memory
acceptance and subsequent GNOME/KDE regressions remain pending. No VM base was
booted or changed for this slice.

Correctness review caught that replacing Activity's header disabled its native
column reordering. A failing native-defaults comparison is retained in
`/tmp/p27v/header-defaults.txt`. The final implementation configures the existing
header instead, preserving movable/clickable flags; the regression checks them.
The full rebuild and all six GUI suites passed after this correction
(`ctest --preset dev -R '^gui_'`, 68.71 seconds). The 59-test result above
precedes this correction; only the affected GUI suites were repeated.

## Storage-driven tray feedback — issue #33 (2026-10-14)

The candidate starts from pending PR #34 head `a4140ea`. Capture selection,
acquisition, local preparation and queue waits now stay blue. Destination checks,
uploads, reconciliation and sharing use scoped storage activity instead of the
active-operation count. S3 and Proton declare reliable upload phases, so their
uploads stay blue through setup/preparing and turn green on uploading/finalizing.
Providers without progress remain valid and use the request boundary.

Errors flash red/blue every 300 ms for three seconds using precise timers. A new
error restarts the interval; recovery restores green if another request remains
active, otherwise blue. Capture cancellation and deliberate clipboard supersession
produce no error pulse. Genuine local-copy errors, including failed retries, do.
The tooltip retains its operation/progress rows independently of color. Color-only
native updates emit icon changes without redundant tooltip signals.

The initial acquisition-color assertions failed against the supplied base before
production changes. Correctness review then found clipboard-supersession and
local-retry feedback defects, and the host review found an initial green event
before provider-reported upload preparation. All three added regressions failed
before their fixes (`review-red.txt`, 19.197 seconds) and passed afterward
(`review-green.txt`, 8.317 seconds). The final development rebuild and full
`ctest --preset dev` passed **59/59**, in **477.68 seconds**, after these fixes.
`git diff --check` passed. The preceding full run also passed 59/59 but predates
the review fixes.

Tests cover selection, deterministic preparation/queue waits with no storage
dispatch, provider preparation without even an intermediate active transition,
optional progress, success, cancellation, failed capture/preparation/upload,
notification-disabled feedback, local-save retries, deliberate deferred-copy
supersession, overlapping successful/failed uploads, pulse restart/recovery, native
wire pixels and host restart. These are offscreen, isolated D-Bus and Xvfb
contracts; mock wire peers are not compositor acceptance.

Logs and before/after failures remain in `/tmp/i33v`. The full-suite tested build
is `0.1.1-dev.175+ga4140ea053ed.dirty`; `build-info-tested.json`,
`binary-hashes-tested.json`, `candidate.patch` and `source-tested.json` identify
its actual bytes and uncommitted patch. It uses GCC 13.3, dynamic distro Qt 6.4.2
and the existing pinned official AWS SDK 1.11.907. The first configure needed
the SDK source/license path as well as its install prefix; both were reused
through ignored build-directory symlinks. Tests use private `XDG_RUNTIME_DIR`
`/tmp/i33v/r` and `TMPDIR=/tmp/i33v/t`; socket preflight passed 395 fixed paths
and equal-length randomized templates, maximum 77 UTF-8 bytes.

Preserved failed attempts include sandbox D-Bus binding denial, a coarse timer
that could recover late, and an overlap fixture that serialized same-name uploads.
The corrected run uses precise recovery timing, two upload slots and independent
generated targets; socket ownership and frozen performance harnesses are unchanged.
The external Anthropic review could not start because shared transport provisioning
returned npm ETARGET for `acpx@0.19.5`, before review-content egress. A separate
local adversarial reviewer completed the lens; all three code findings were applied.
The completed review and follow-up receipt are under
`/tmp/compound-engineering-1000/ce-code-review/20261014-172954-89b34b02/`.

The [native VM follow-up](evidence/2026-10-14-storage-tray-native.md) tested frozen
commit `061c2d9c569d` on Cinnamon 6.6.4/X11/Qt 6.4.2, GNOME 50.1/Wayland/Qt 6.10.2
and Plasma 6.6.6/Wayland/Qt 6.10.2. Each desktop passed twelve native
capture/clipboard/cancellation cases, six view opens/closes with normal helper
exit, and five rendered storage/error color checks. Cinnamon passed its required
functional and memory gates before the Wayland builds began. GNOME and KDE's
native development suites passed 60/60 and 59/59; their post-build tracked source
hashes matched all 5,998 files in the frozen archive.

Two serial five-minute post-use guest observations per desktop passed the 40 MiB
RSS/no-helper/no-swap/CPU gates. Peak RSS was 29.96 MiB on Cinnamon, 33.98 MiB on
GNOME and 31.05 MiB on KDE. The report retains exact identities, raw samples,
synthetic console frames and failed diagnostic attempts. The first KDE region
attempt had an SSH interruption and later helper timeout; its repeat and an
independent fresh-overlay first gesture passed, without a candidate code change.
These are bounded single-head compositor and post-use VM results. The offline S3
wire oracle does not establish cloud/Proton acceptance, sampled frames do not
certify each 300 ms repaint, and physical performance/full release gates remain
separate. All golden/backing disks were preserved; only fresh overlays were booted.

## Cinnamon login startup — issue #24 (2026-10-16)

The [login-fix report](evidence/2026-10-16-issue24-login-fix.md) and its payload-free
JSON receipt record frozen implementation `09ce25b7851624e602c96dba855b4465e688fec9`.
Two actual Cinnamon console logout/login cycles started distinct verified
residents with a registered tray, preserved configuration and no app window or
UI helper. The new production-process startup regression failed before the fix
and all nine final scenarios pass. Full Dev passed 59/59; affected ASan/UBSan
tray/startup checks passed 3/3. These private-bus/Xvfb checks are host contracts;
the separate native VNC evidence establishes the bounded desktop results.

Cinnamon's twelve native capture/clipboard/cancellation cases, repeated native
view reopen, restart and both five-minute memory observations passed before the
GNOME/KDE package build and native campaigns. Both Wayland profiles passed the
same twelve cases and view/restart checks. Memory sampling followed each
restart and native History reopen/close: two serial 300-second intervals per
desktop met the 40 MiB RSS, <0.5% CPU, zero-swap and no-helper gates. They do not
establish same-resident post-capture retention, physical performance or complete
U9/U10 acceptance; broader workloads and release gates remain issue #44.

All three overlays were retired through guarded cleanup after evidence export.
Exact package/binary/run identities, raw samples, failures and cleanup receipts
remain private under `work/i24-fix/`; public summaries bind them by checksum.
The historical local-only suppressed-copy observation remains unreproduced and
is not claimed fixed by the startup change.

## Destination controls regression — 2026-10-16

The 2026-10-16 combined S3/Proton controls campaign passed the current-source
Cinnamon-first three-desktop functional matrix and user-requested 30-second
post-use memory observations; see [the native receipt](evidence/2026-10-16-destination-controls-native.md).
Its synthetic S3 and minimal Proton scope does not establish broader U9 or live-cloud gates.
