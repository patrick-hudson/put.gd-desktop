# Integrated Linux development acceptance, 2026-10-25

**Campaign in progress. No full-profile or stable acceptance is claimed.**
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
`history-thresholds-approved.json` and the quote is in `COORDINATION.md`. Approval
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
limits, missing declared distro/package rows and final stable exact-byte gates
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
40 MiB. Static four-format capture/export passes; the moving fixture refused
before product recording because its host GTK GStreamer sink was missing.
The initial reverse-VNC port refusal and subsequent corrected 45901 forward are
retained without attributing the failure to SELinux. All 96 exported synthetic
files verified before guarded retirement. A fresh overlay for the queued moving
fixture followup is still in progress; the missing moving row is not a pass.

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
