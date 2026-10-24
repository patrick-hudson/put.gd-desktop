# Exact development-artifact uninstall acceptance

All **11 targeted functional rows passed** at clean code commit
`d5b561ddb94f8255b8bc754c1382552d983a6fa9`, tree
`797721fa0672581562a8675d0d2010eced7c6bd4`, build
`26.1024.1186-dev+gd5b561ddb94f`. These are actual DEB/RPM and FUSE-mounted
AppImage bytes from [CI run 40068126465](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40068126465).
This verifies the corrected #382 lifecycle on development artifacts; the
published-nightly gate remains after the code and docs PRs merge. All eleven
rows create integration with the updated build.

## Matrix

Each row includes an independent 180-second idle resident memory observation.
Values below are peak RSS / ending PSS in MiB, both below 40 in every row.
`loginctl` session types and the running Qt Wayland or XCB plugin are retained
in each receipt; installed versions alone are not session proof.

| Target | Format | Actual desktop/session | RSS / PSS MiB | Strict map gate | Result |
| --- | --- | --- | --- | --- | --- |
| a382a | appimage | Aurora 45 Plasma 6.7.5 / Wayland (UEFI VM on Fedora) | 31.26 / 24.38 | pass | [receipt](a382a/appimage/summary.json) |
| d69 | appimage | Debian 13 Plasma 6.3.6 / Wayland (physical) | 31.16 / 25.27 | pass | [receipt](d69/appimage/summary.json) |
| d69 | native | Debian 13 Plasma 6.3.6 / Wayland (physical) | 31.08 / 9.10 | pass | [receipt](d69/native/summary.json) |
| f44 | appimage | Fedora 44 GNOME 50.6 / Wayland (physical) | 29.82 / 23.56 | pass | [receipt](f44/appimage/summary.json) |
| f44 | native | Fedora 44 GNOME 50.6 / Wayland (physical) | 35.61 / 21.40 | pre-existing Qt dependency† | [receipt](f44/native/summary.json) |
| k72 | appimage | Kubuntu 26.04 Plasma 6.6.6 / Wayland (physical) | 30.75 / 24.18 | pass | [receipt](k72/appimage/summary.json) |
| k72 | native | Kubuntu 26.04 Plasma 6.6.6 / Wayland (physical) | 32.49 / 9.15 | pass | [receipt](k72/native/summary.json) |
| m74 | appimage | Mint 22.3 Cinnamon 6.6.9 / X11 (physical) | 30.57 / 24.62 | pass | [receipt](m74/appimage/summary.json) |
| m74 | native | Mint 22.3 Cinnamon 6.6.9 / X11 (physical) | 28.90 / 18.26 | pass | [receipt](m74/native/summary.json) |
| u382a | appimage | Ubuntu 26.04 GNOME 50.1 / Wayland (VM on Fedora) | 29.90 / 23.62 | pass | [receipt](u382a/appimage/summary.json) |
| u382a | native | Ubuntu 26.04 GNOME 50.1 / Wayland (VM on Fedora) | 31.77 / 20.59 | pass | [receipt](u382a/native/summary.json) |

† Fedora native retains a raw strict memory-map failure for
`/usr/lib64/libcrypto.so.3.5.9`. This predates this change: the
[1058 native campaign](../../2026-10-23-native-full-acceptance/README.md) item C
already recorded it, and current [readelf/RPM evidence](f44/native/fedora-qt-dependencies.txt)
shows `libQt6Core.so.6` directly requires `libcrypto.so.3`. Functional and memory
budgets pass; the library policy itself is not reported as a strict pass.

The [matrix JSON](matrix.json) gives exact SHA-256, size and CI artifact ID for
every package, plus all four real login session IDs. Each row's `summary.json`
retains the source/build, executable hashes, native package verification,
preflight socket lengths, Qt platform plugin, memory samples, cleanup,
configuration/data hashes and journal result. Published journal text is limited
to startup/managed-watch units; full original receipts remain privately exported,
with their hashes and a verified export index.

## What each row exercised

1. Start the installed package or actual AppImage without provider credentials,
   enable Launch at login, quit the resident, then perform a real graphical
   logout/login. Verify the exact build auto-starts and its Settings checkbox is
   enabled; sample resident memory for 180 seconds.
2. Quit the resident and disarm the watch to model an ended user manager. SSH
   keeps the test account's manager alive, so this step is deliberate. Perform a
   real logout, assert no active graphical session, remove the package/final
   image, then perform a real login. Require the managed entry/script absent,
   no resident and no startup journal error. Configuration and user-data hashes
   must be unchanged, with the saved preference still true.
3. Reinstall and manually start. Verify the retained preference re-creates startup
   integration. Quit the resident, leave the real user-manager watch armed and
   remove the installation. Require cleanup while the account is active,
   unchanged settings/data and another clean real login. The watch has a
   two-second settling grace for atomic updates; observed times in the JSON
   start when the removal command returns, not when the executable is unlinked.
   Zero means cleanup was already complete when the package command returned.
4. Reinstall/start, set `launchAtLogin:false` through the production CLI's
   expected-revision settings patch, verify `enabled:false, applied:true` and the
   unchecked Settings checkbox, quit/remove and verify another clean real login.
   The MCP helper forwards the same operation but was not independently rerun.
5. Remove only owned test profiles, restore pre-existing log placement, verify no
   owned resident/entry/script remains and release each physical desktop lock.

All eleven rows passed the enabled-login and post-removal startup-journal gate.
All rows have nine reviewed complete desktop frames, indexed by `visual-review.json`.
The [22 annotated pictures](pictures.json) show enabled Settings after login and
the later post-removal login. Captions name exact build, target, session and UTC
time. Original pixels/hashes are retained privately; orange boxes and caption
bands are separate annotations. File/process absence comes from receipts, not
from an empty desktop picture. Mint's stock Welcome, KDE remote-control and
Debian IBus notices are unrelated and remain visible in the evidence.

## Lab failures retained and corrected

Aurora 45 uses Plasma Login Manager. The first qdbus logout did not end that
session. Stopping only the display manager then left old `ksmserver` and
`plasmashell` user services, giving a blank later session despite logind saying
active/unlocked. Those attempts and blank frames are **invalid acceptance**.
The corrected lab route stops `plasmalogin` and the owned Plasma workspace/
graphical-session user targets before restarting the display manager. It yielded
a visible fresh desktop, then repeated the enabled login, memory and complete
remaining lifecycle. Product source and the acceptance gates did not change.
[Correction receipt](a382a/appimage/harness-correction3.json),
[explicit accepted retry selection](a382a/appimage/receipt-selection.json),
[corrected remote driver](lab/i382-aurora3.py) and [resume driver](lab/resume_aurora3.py)
are retained alongside earlier drivers. The second correction also emitted an
extra JSON record; the third silenced that diagnostic stdout without changing
logout behavior. No blank frame or earlier product build is reused as a pass.

Debian's final lock release initially detected a newly running unrelated signer
service. Its cgroup was verified, it was left running, and release succeeded
using the lock tool's explicit `--not-mine` exclusion. Both release receipts and
[ownership proof](d69/appimage/non-owned-signer-process.json) are retained.

## CI and artifact provenance

The [full branch run](branch-ci-state.json) and [PR integration run, attempt 2](pr-ci-state.json)
passed their required `CI` summary, tests, ASan/UBSan, Windows compile, native
packages, AppImage, debug-symbols, PipeWire, SBOM/scans and source/notices gates.
The integration commit has the same application/packaging/release-tool sources
plus unrelated nightly lab/docs changes; see [source identity](source-identity.json).
Its version is `26.1024.1191-dev+gccbd7d4ac576`; its bytes are not substituted for
the 1186 desktop-tested artifacts.

The PR's first SBOM job failed before scanning, while extracting the uploaded
AppImage. [Original failure log](pr-integration-sbom-failure.log) is retained;
its cause is unknown and its failed runner bytes were not retained. The same
uploaded artifact matched its build receipt SHA-256, extracted successfully on
Mint, and the retried CI job passed. [Extraction receipt](pr-integration-extract.json).
This was not a reported vulnerability finding.

[AppImage metadata](artifacts/appimage-metadata/appimage-build.json) identifies
the exact tested image and its null development update channel. Its
[complete-source receipt](artifacts/appimage-metadata/appimage-complete-source.json)
binds the source archive to those image bytes; CI checked the archive, but the
674 MB source archive was not downloaded again for desktop testing. Actual
AppImage execution/FUSE, native Qt plugin and image hashes are verified in all
six rows. Same-channel replacement is covered only by the bounded executable
fixtures in the earlier checks. Published-nightly replacement remains a gate.

## Legacy-account rollout limitation

Code inspection establishes a remaining compatibility limit: old native entries
point directly at the executable and cannot run cleanup after that executable
is gone. Earlier AppImage scripts also lack this cleanup. The new version
migrates managed integration when it runs in an account; focused tests cover
that migration. This campaign does not establish automatic cleanup for an old
entry in an account that never ran the updated version. Such accounts need an
updated-app run or explicit disable before uninstall. The no-home-traversal
rule is preserved. #382 remains open for this rollout limitation as well as
merge/published-nightly verification; these current-entry tests are not an
unqualified all-account backward-compatibility pass.

## Reproduction and cleanup

The frozen [campaign](lab/campaign.py), [remote driver](lab/i382.py),
[host SSH controller](lab/control.py), [guest controller](lab/guest_control.py),
[complete-frame VNC reader](lab/full_rfb.py), helpers, artifact fetch/stage tools
and original source hashes are retained under `lab/`. Paths, host pins, run IDs,
ports and protected blank-login references must be configured for new owned
runs. Do not run these historical commands against reused/disposed profiles.
From a matching source checkout, stage them under a new short private output
root and stage only the pinned CI artifact after verifying its build/audit SHA.
The original command shape was:

```sh
python3 work/i382n/control.py f44,d69,k72,m74 stage
python3 work/i382n/guest_control.py u382a stage
python3 work/i382n/campaign.py f44 native /home/patrick/work/p382n/EXACT.rpm
python3 work/i382n/campaign.py u382a appimage /home/lab/work/p382n/EXACT.AppImage
```

Run independent Cinnamon/GNOME/KDE rows concurrently, with a physical lock per
host and visible wall tiles. Prepare clean synthetic profiles first, preflight
all resolved sockets, inspect package transactions, and use the corrected Aurora
logout driver. Its replay is `resume_aurora3.py` after the recorded clean stop;
a new campaign should select `i382-aurora3.py` for every Aurora remote call.
The Ubuntu console helper validates the blank masked field before reading a
protected owned/private local login input, types it only in memory and saves
no post-input login frame. Provision fresh references for a new guest.

All raw good/failed pictures and results remain in the private code-worktree
`work/i382n/evidence/`. Guest-owned synthetic test directories and all guest
pictures/receipts were exported to Fedora under `~/work/a382/work/exports/`;
every archive member was SHA-256 verified before guarded VM retirement.
[Retirement receipts](retirement/) preserve export/completion/manifest hashes
and exact removed overlays; original goldens remain untouched. The test VMs,
only their two tunnels and only their two wall tiles were removed. The four
physical locks are free; the protected Mint Windows VM and unrelated Debian
signer were left intact. [Final cleanup receipt](lab-final-cleanup.json).

This targeted campaign does not perform the full native A–Q or AppImage A–K
profiles, capture/recording/upload, live S3, physical performance, mixed displays,
full fresh-agent parity, a dated Proton review or exact stable-draft acceptance.
Those release gates remain with #378–#385 as individually scoped. #382 remains
open for merge and published-nightly evidence; no release was performed.
