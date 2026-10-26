# Integrated Linux development acceptance, 2026-10-25

**Targeted campaign completed and cleaned up on 2026-10-26. No full-profile or stable acceptance is claimed.**
The frozen source is `6b7949f5cab8ee9d9564d5ebb4f2cee81881cbf8`, tree
`2911da34467c4d0b062a9c79d72baa39a79d7640`, version
`26.1025.1362-dev+g6b7949f5cab8`. The assets are CI development artifacts from
[run 40244194828](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40244194828),
not the older published nightly. [freeze.json](freeze.json) records exact package
and AppImage digests. The required CI summary passed. This combines #572 disk
pressure, #574 deferred Settings and #576 resident allocator policy in one source.

## Measurements and attribution

The unchanged #555 performance driver is revision
`00ab777f2413a1fbd7670a7259dc614eced896e6`; its main script SHA-256 is
`3cb511e0f0e7e4de63afa68d0ae66a56a8f6abcc028aa0f2df426e27d21a6676`.
Separate functional observers import frozen components without changing them;
their results are not attributed to the frozen harness. All attempts, including
observer refusals and failed attempts, remain indexed. No best-run selection.

Physical Mint/Cinnamon native DEB:

- Empty-History 300/30/300 seconds: cold RSS 29,908,992 B, PSS 18,881,536 B,
  CPU 0.00666379% of one core; History-open family PSS 46,482,432 B;
  closed RSS 29,995,008 B, PSS 18,913,280 B, CPU 0.00333227%. Budget checks pass.
- Seeded 10,000-row 300/30/300 seconds: cold RSS 32,186,368 B / PSS
  21,089,280 B / CPU 0.086650973%; History-open family PSS 58,519,552 B
  over 30.066547 seconds; closed RSS 35,819,520 B / PSS 24,741,888 B /
  CPU 0.003332851%. All memory/CPU and cleanup gates pass.
- Capture: 5 warmups / 30 warm samples; drawable-paint p95 73.873497 ms empty
  History and 74.387889 ms with 10,000 rows, both within 150 ms. This witness
  observes X11 drawable paint, not compositor presentation.
- Actual native capture burst: 50 region captures, independent synthetic pixel
  checks before Upload, distinct redactions, 50 uploaded hash/pixel/history/link
  checks; concurrency 4, queued peak 19. After 300 seconds, RSS 33,292,288 B
  and PSS 21,663,744 B pass. This private observer is distinct from #555's
  file-editor burst and does not change that harness.
- Slow/stalled loopback proxy: shared 262,144 B/s, cancellation terminal latency
  733.716555 ms, retry completed; settled RSS 32,870,400 B/PSS 21,068,800 B pass.
  Cancellation latency has no approved numeric limit and remains ungraded.
- History: all 200 pages / 10,000 rows traversed. Preview p95 upper bound
  412.367046 ms passes. Open, scroll and search remain unassessed because the
  frozen observer did not witness their content endpoints. Generic paint values
  (339.718593 / 49.420392 / 204.366743 ms) cannot replace those endpoints.

Patrick explicitly approved **open ≤1,000 ms, scroll p95 ≤100 ms, search p95
≤250 ms, preview p95 ≤1,000 ms**, replying “Approve these limits” to the quoted
proposal in the integrated acceptance thread. The canonical shared decision is
[history-thresholds-approved.json](history-thresholds-approved.json); its
[approval-source quote](history-thresholds-approval.md) is retained here. Approval
does not convert generic repaint timing into content-response evidence.

GNOME native file-editor burst has an unresolved **40 MiB RSS budget failure**.
The integrated result is 42,532,864 B versus the exact 41,943,040 B limit:
+589,824 B. Matched pre-integration source `98878cfe31c2506d25ea6cbec97af002210319d2`
produced 42,192,896 B: +249,856 B. Both use the same GNOME session 3012,
packaged extension bytes, file50 workload and 300-second quiet settle. PSS passes.
The current-minus-baseline difference is 339,968 B (332 KiB). This establishes
that a miss predates integration of #572/#574; it neither attributes the added
delta nor compares against pre-#576. The budget remains open. One justified
integrated repeat and a read-only memory-map snapshot are retained separately.
The repeat is 42,610,688 B RSS (**FAIL**, +667,648 B) / 27,520,000 B PSS
(PASS), with normal cleanup. Integrated spread is 77,824 B; its two deltas
against the single baseline are 339,968 and 417,792 B. No additional repeat is
used to chase a pass. The current-only map has a 5,042,176 B heap plus substantial
file-backed Qt/code pages; it does not establish a causal fix or a baseline map
comparison. [Matched results](f44-matched-summary.json),
[memory-map summary](f44-memory-map-summary.json).

## Functional and media scope

Current native GNOME/KDE idle gates pass independently of the post-burst GNOME
failure. Deferred Settings, History/session commands, queued-editor cancellation,
synthetic explicit Upload and focused GTK4 link-paste are exercised separately.
Tiny in-flight uploads can already reach the fixture before cancellation; the
initial observer's contrary expectation is retained, not reported as a new
regression. Queued-target controls verify the target remains unpublished after
blockers drain.

Normal and paused disk-pressure cases use an owned 1,536 MiB tmpfs, with a paced
competing writer stopping at 1,120 MiB free. No host disk is filled. The checks
require `budget/disk_space`, preserved private Review, no publication before
explicit Upload, independently decoded local export, and independently verified
uploaded bytes. Early hidden-overlay pictures do not prove visible Review;
later raised-Review pictures and Mint/GNOME/KDE visible-overlay and independent
recording-link paste runs supply those specific observations. The observer calls
`recording.raise` before the Review frame; its unchanged pre-integration handler
also shows the gray control bar. That frame is not proof of spontaneous overlay
survival caused by #572. [Overlay attribution](overlay-observation.md). A transient `copyStatus=pending`
snapshot is not a terminal clipboard failure.

Recording rows use the exact AppImage or installed distro package, saved Review
pictures, strict owned-edge export frames, moving content and separate static
controls for MP4/WebM/GIF/WebP. Available live encoder lists do not, by themselves,
prove which encoder was selected. These targeted checks do not establish audio
synchronization, trim/timed blur or every native A–Q/AppImage A–K requirement.

## Coordination and retained limitations

Native GUI work requires an owned, heartbeated desktop lease. Quiet measurements
exclude shared heavy builds/VMs and topology changes. d69 and k72 were released
with verified-clean plus `status --check` receipts before the pilot acquired its
own leases. Existing signer, collectors and unrelated VMs were preserved.

k72's native X11 run used an actual X11 login, then restored exact SDDM bytes and
Wayland with a visible wall. A complete user-app inventory was saved immediately
before switching to X11. The return command checked completed owned runs and
resident absence but omitted a second complete user-app inventory. No impact was
observed; that is **not retrospective proof** of the missing check. The procedural
gap remains, and no extra switch is performed to erase it.

The original shared watchdog disappeared for an unknown reason after its last
00:10:50Z sample. At 00:35Z the deputy found PID 546928 absent, checked current
leases directly, and restarted it as a user systemd service, still bounded to the
original 2026-10-26T23:30:14Z ceiling. Continuous monitoring during that gap is
not claimed. Completion accounting includes the separately authorized RSS
investigation, so the two original lane markers cannot stop it prematurely.

#378 fresh-agent/parity, #380 geometry (owned by the pilot), #381 physical-display
limits, remaining full-profile requirements and final stable exact-byte gates
remain explicit. Neither this CI build nor an earlier-build pass certifies the
published nightly or a future stable draft. No merge, promotion, publication,
tag change or acceptance waiver is authorized.

[runs.json](runs.json) records per-attempt outcomes and identities;
[private-evidence-index.json](private-evidence-index.json) identifies retained
private artifacts by digest. The private source/result archives remain outside
release source exports.

## Additional current-build guest evidence

Ubuntu 26.04.1 / GNOME 50.1 / real Wayland on f44 passed targeted native DEB
and exact-AppImage deferred Settings, synthetic editor Upload and focused GTK4
link-paste checks. The 180-second native peak was RSS 33,046,528 B / PSS
21,329,920 B. The corrected AppImage observer accounts for both resident and
FUSE runtime: RSS 38,076,416 B / PSS 31,227,904 B, both within this C-profile's
40 MiB limits. This is not physical #379 CPU proof or the native 29 MiB PSS gate.
The first AppImage observer incorrectly required one process and refused the
resident-plus-runtime family; that failed attempt remains retained.

Both formats completed native Mutter capture, pause/resume, private Review and
local export for MP4/WebM/GIF/WebP. Native export motion scores were
25.828 / 25.474 / 25.276 / 30.614; AppImage scores were
27.282 / 27.220 / 26.968 / 31.521. Every corresponding static score was zero.
The AppImage's initial moving attempt completed MP4 but a VNC frame read timed
out during WebM's post-stop observation. Its corrected private observer permits
one new connection after a recorded timeout and still fails if no frame arrives;
the production recording driver and pixel thresholds are unchanged.

Before these media runs, the cover player refused a missing GTK sink; normal
`gstreamer1.0-gtk3` 1.28.2-2ubuntu0.4 supplied it. The next attempt correctly
refused recording at 1.4 GiB free. Three owned duplicate AppImage extractions
were archived and checked before removal, and the normal apt download cache was
cleaned; the corrected admission had 2,662,686,720 B free. Existing golden/test
source was preserved. Earlier C180 measurements retain their original environment.
[Media environment and archival receipt](ubuntu-media-environment.json),
[exact guest/module identities](ubuntu-identities.json).

The Ubuntu overlay was stopped and guarded-retired after a verified archive of
322 synthetic result/media files. The first cleanup dry-run refused a canonical
run invoked from a scratch tool tree. The unchanged guard was placed at its
previously absent canonical module path; existing lab tools were preserved, and
the subsequent dry-run and retirement passed. Failed attempts stay failed in
the completion manifest.

Ubuntu 24.04.5 / GNOME 46.0 / real X11 is newly provisioned from a signed official
cloud source into an independent overlay on m74. It does not substitute XWayland
for the required session. Native and AppImage targeted functional/paste checks
pass; native C180 peak RSS 38,965,248 B / PSS 26,416,128 B passes.
[Session/module identities](gnome46-identities.json).

The first AppImage idle window ended with `LockedHint=yes` and inactive QEMU
output, so its visible-session evidence is invalid. The initial media attempts
also refused their cover precheck. These failures are retained. Patrick explicitly
authorized correcting that invalid window and making guest visibility prevention
durable. The corrected `xai2` window has successful real-home policy, logind
inhibitor, unlocked native session and complete QMP/VNC output checks before and
after measurement:

| C180 scope | Peak RSS B | Peak PSS B |
| --- | ---: | ---: |
| Resident | 39,915,520 | 33,291,264 |
| AppImage FUSE runtime | 8,568,832 | 8,564,736 |
| Whole owned family | **48,484,352** | 41,856,000 |

The observer's whole-family RSS check **fails** 41,943,040 B; its PSS check passes.
The resident alone is within 40 MiB. These scopes remain separate; neither
rounding nor selecting the smaller scope erases the family failure. No further
identical repeat is used. Swap is zero, owned processes exited and the session
socket was absent. Saved complete post-window output shows an unlocked desktop
with first-login/updater dialogs; no OS upgrade was performed.

Both native DEB and AppImage completed moving/static MP4, WebM, GIF and WebP
capture, pause/resume, private Review and export. Static working clips have zero
motion; encoded export scores include 0.001/0.002 noise and remain recorded
exactly. These are targeted media checks, not full recording-profile acceptance.

The common helper in [code PR #584](https://github.com/patrick-hudson/put.gd-desktop/pull/584)
checks actual desktop configuration rather than the product XDG sandbox. A live
GNOME46 drift injection was refused; restoration while the screensaver was still
active was also refused by the blank-output check. After visible console wake,
a real guest relogin restored a deliberately changed timeout through the new
autostart helper. The subsequent check verified timeout zero, lock disabled,
idle/sleep inhibition, X11 DPMS disabled and an active unlocked visible session.
The updated helper explicitly checks ScreenSaver activity as well as LockedHint.

All 313 synthetic bulk files verified on the host and workstation before guarded
retirement. Only the newly added host `cloud-image-utils` package was removed;
the original package-name set and Windows/Welcome/collector processes were
preserved. The owned tunnel/tile were removed and m74 returned with a clean
status check. [Completion, scope and policy receipts](gnome46-completion.json).

Aurora testing-45.20261022.1 / Plasma and KWin 6.7.5 / native Wayland on
f44 passed exact-AppImage deferred Settings, synthetic editor Upload and focused
GTK4 paste. Whole-family C180 peak RSS 39,424,000 B / PSS 32,271,360 B passes
40 MiB. The first static four-format capture/export passed; the initial moving
fixture refused before product recording because its GTK GStreamer sink was missing.
The initial reverse-VNC port refusal and subsequent corrected 45901 forward are
retained without attributing the failure to SELinux. All 96 exported synthetic
files verified before guarded retirement. The completed fresh-overlay followup
installed `gstreamer1-plugins-good-gtk` 1.28.7-2.fc45.x86_64 through normal
`rpm-ostree install --apply-live --idempotent`. A setup observer timed out while
that transaction continued; its log is retained and completion was checked before
recording. Base version stayed testing-45.20261022.1. Moving export scores were
26.855 / 26.830 / 26.877 / 31.630 for MP4/WebM/GIF/WebP; the same-environment
static controls were all zero. Policy and unlocked visible-output checks passed
before, between and after the runs. The earlier C180 retains its original
pre-GTK environment; it was not rerun after fixture installation.

The first common-helper launch on that overlay failed before product work with a
generic DBus command error. The exact failed method was not recorded. Both methods
were later present and unchanged helper behavior passed after readiness, supporting
startup ordering as an inference, not a proven original method failure. The revised
helper adds bounded initial service readiness and exact command diagnostics.
A real guest reboot then exercised login autostart, followed by a full check-only
visible/unlocked preflight. All 102 followup archive members verified on host and
workstation before guarded retirement; only the owned 6084 tile/tunnel were removed.
[Environment, startup, retained attempts and cleanup](aurora-followup-completion.json).

The separately scheduled Fedora RSS diagnostic pair used the same absent
running-Shell extension for both builds, unlike the original extension-active
gate environment. Integrated settled RSS was 41,299,968 B and baseline
42,135,552 B; this is diagnostic attribution evidence, not a replacement gate
pass. The lane retained an Overview precondition refusal and a vanished-child
harness failure before its single baseline retry. It found run-to-run heap
retention variation but no justified product fix. The original RSS failure stays
open. Further extension-active profiling is a recommendation, not an admitted
slot or waiver.

Selected saved frames are indexed with exact source, asset reference, original
private path, digest and scope in [visuals/index.json](visuals/index.json).
They were reviewed; live wall presence alone is not used as saved evidence.


## Completed remaining media and common guest startup validation

The installed Debian13 DEB on d69 completed native Plasma/Wayland MP4, WebM, GIF
and WebP capture with pause/resume, private Review and local export. Moving export
scores were 7.850 / 7.162 / 6.677 / 8.160; all four static scores were zero.
The initial wrapper path did not exist and exited before product recording;
the corrected, digest-retained wrapper was then used. All 102 archive members
verified on the workstation. Removing only putgd restored the exact 3,697 package
names. An initial malformed shell-expanded package query is retained as invalid;
the corrected argv-based comparison was byte-exact. The verified-clean lease
release was 03:29:32Z, followed by `status --check`; signer and collector remained.
[Native binary identity, archive and cleanup](debian-media-completion.json).

The fresh Cinnamon/X11 overlay on f44 tested the common helper only. It did not
run another product acceptance workload. Initial failures exposed a blank first
frame, Cinnamon's demand-activated ScreenSaver service, and failed-launch writes
that had not reached the guest disk. The updated lab code retains every initial
frame attempt while waiting at most 15 seconds for initial visible output; later
check-only boundaries still fail immediately. Cinnamon's actual `GetActive`
result is required instead of requiring an already-owned bus name. The generated
run public key is newline-separated and synced; failed launch attempts a bounded
guest sync before stopping the owned QEMU process.

Recovery preserved the malformed authorized-key line and renamed the exact empty
owned inhibitor unit before repairing them. No existing keys or unrelated files
were removed. Check-only preflight rejected a deliberate idle-delay of 600 seconds.
SSH reboot requested interactive authorization and visible terminal/session attempts
did not change uptime; no password was entered. After authenticated guest sync,
a recorded QMP `system_reset` restarted only the disposable guest. Its new boot ID
and startup receipt show timeout zero, lock/blanking/suspend disabled, the actual
idle/sleep inhibitor, X11 DPMS disabled and an active unlocked Cinnamon session.
The final complete desktop frame was reviewed and the full check-only preflight
passed. This is an owned guest reset test, not a graceful reboot claim.
All 23 exported evidence members verified before guarded overlay retirement;
SSH22445/VNC5945 closed and only its 6085 tile/tunnel were removed.
[All helper attempts, startup and cleanup](cinnamon-preflight-completion.json).

The final tooling head is `c53ce0bd181b521abba0ca3e2e2744d3a0c70d78` in #584;
24 focused guest tests and 32 lab tests pass, with the required
[CI summary](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40263621820)
successful. These tooling changes do not alter the frozen product or performance
harness. GNOME46, Cinnamon and Plasma live policy/startup checks are complete;
Plasma5 has contract coverage but was not a live campaign target. Existing immutable
goldens and physical desktop policies are unchanged. Fresh bootstrap media installs
the future-login helper; launch/start and campaign boundaries verify effective live
state. Older goldens need the helper installed in their owned overlay if later
logins are part of the campaign.

| Target | Native functional / memory scope | Exact native / AppImage four-format moving + static |
| --- | --- | --- |
| m74 Mint22.3 Cinnamon X11 | Integrated functional, physical CPU/RSS/PSS, seeded History and native capture burst pass; History content endpoints partly unassessed | Both pass |
| f44 Fedora44 GNOME Wayland | Functional and C180 pass; post-burst RSS remains FAIL | Both pass |
| k72 Kubuntu26.04 Plasma Wayland | Functional, idle and file burst pass | Both pass; native actual X11 also passes, with the return-inventory procedural gap retained |
| d69 Debian13 Plasma Wayland | Functional, idle and file burst pass | Both pass |
| f44 guest Ubuntu26.04.1 GNOME50.1 Wayland | Native/AppImage functional and scoped C180 pass | Both pass |
| m74 guest Ubuntu24.04.5 GNOME46.0 X11 | Native functional/C180 pass; AppImage functional pass, corrected whole-family RSS FAIL | Both pass |
| f44 guest Aurora45 Plasma6.7.5 Wayland | Exact-AppImage functional/C180 pass | AppImage passes; no native package row claimed |

These entries are targeted checks with the parameters and exclusions above, not
complete package profiles. All 113 retained run results remain indexed, including
refusals and superseded observers. Every acceptance-owned guest is stopped and
retired, physical packages and owned UI processes cleaned, and all desktop/heavy
slots explicitly handed to the authorized screenshot inventory lane after checks.
The separate pilot's geometry and RSS investigation remain separately attributed.

Outstanding acceptance work is concrete: native Fedora's original 40 MiB RSS
failure, GNOME46 AppImage's whole-family RSS failure and accounting scope, witnessed
History open/scroll/search content responses under the approved limits, complete
#378/#380/#381 requirements and the final published/stable exact-asset gates.
No further identical memory repeats, threshold changes or waivers are implied.


## PR review remediation (after campaign completion)

Review of #584 found that the shared fixture runner's entry boundary used apply
mode, potentially repairing drift. It now checks without repair. The complete-frame
visibility test now compares RGB pixels rather than individual channel bytes, and
early refusals retain a receipt once the local run directory is safe to write.
Unsafe directories still fail without writes or guest access. Deterministic account
fixtures remove dependence on an exported `USER` or a non-root test runner.
All 29 focused guest contracts pass with `USER` unset.

These changes belong to the subsequent PR-review phase at code `5f7d1e8c`.
They do not replace the campaign's tested host-tool revisions or alter original
run bytes. A read-only audit of eight retained successful Cinnamon/Aurora QMP
frames finds no uniform-RGB witness among them; this is not a fresh acceptance
run. [Remediation and offline check receipt](review-remediation.json).
All 113 original run-result digests, saved visuals, failure objects and completion
receipts remain retained; no completed watchdog or guest was restarted.

The visual index now names its source root and digest-index section explicitly,
including the two lossless PPM-to-PNG derivatives outside the collected-run tree.
The approval receipt is copied unchanged, and preflight collections are sorted by
their actual timestamps without changing any attempt. The public Debian status
summary omits the unrelated signer's command/home and retains its raw-receipt
hash. Declared non-secret host labels, lab paths and run/process identities remain
for provenance, consistent with the repository's documented lab inventory.
