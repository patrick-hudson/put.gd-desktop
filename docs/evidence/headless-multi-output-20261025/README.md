# Headless multi-output pilot — 2026-10-25/26

This is partial geometry evidence, not release acceptance. The executable route
is documented in [the lab guide](../../headless-multi-output.md). Product fix
[#582](https://github.com/patrick-hudson/put.gd-desktop/pull/582) and tooling
[#583](https://github.com/patrick-hudson/put.gd-desktop/pull/583) target nightly.
[#380](https://github.com/patrick-hudson/put.gd-desktop/issues/380) remains open;
[#381](https://github.com/patrick-hudson/put.gd-desktop/issues/381) is unchanged.

## Exact application identities

| Artifact | Source/build | SHA-256 |
| --- | --- | --- |
| Frozen CI AppImage | CI 40244194828, `6b7949f5cab8ee9d9564d5ebb4f2cee81881cbf8`, `26.1025.1362-dev+g6b7949f5cab8` | `b88350ff4ca89d9bba2c76769fb5a864370828b9c90a0072f58d7f7f8cb7b9f3` |
| KDE fix CI AppImage | CI 40251723369, merge `4610dda66a9d8eb30ff8f2b583ebd2c88fea2e0d`, `26.1026.1364-dev+g4610dda66a9d` | `94a3015b33ba7eb21d26038f93d21e9309ecc9441cd9bd692446984d2ed305af` |

The fix merge has parents frozen nightly `6b7949f5` and PR head `2f5a5d6a`.
Its tree `880b394b51a09211b53105039ed9c7bf8039de8f` equals the product-only PR
head's tree. Both are development CI artifacts, distinct from published nightly
or stable assets. Native-host AppImage results below are not DEB/RPM results.
The [identity summary](identity-summary.json) records representative exact package,
resident PID/start/hash, helper hashes, native-session readback, GTK backend and
actual output geometry for the native Mint/KDE and both GNOME artifact stages.

The private remote C++ build at source `687f17d6` (same product patch plus then-current
lab sources) passed all 110 CTests. The new three-screen widget regression fails
with the old constructor (exit 8), passes with the fix, and was restored/retested
after mutation. These are widget/contracts results, not compositor evidence.
PR #582's CI summary passed on its exact CI merge.

## Observed profiles

| Profile | Native GPU/wall feasibility | App geometry scope |
| --- | --- | --- |
| Debian 13 / Plasma 6.3.6 Wayland, Intel HD 530 | Independent pipes, QHD30/60, scale 1.5, rotation and gaps observed; restored | App launch blocked by the then-current wall gate. Earlier secondary-stream attribution needs a retest with the RFB fix. |
| Kubuntu 26.04 / Plasma 6.6.6 Wayland, Intel i915 | FHD + QHD at 1.5; both heads fresh on combined wall | Frozen build reproduced wrong-head selection; fixed build primary region/paste and MP4 motion/bar pass. Full fractional native-grid/reference comparison remains pending. |
| Mint 22.3 / Cinnamon 6.6.4 X11, HD 530 / Mesa 25.2.8 | FHD + QHD, both fresh; rotated left/up/gap profile fresh | Frozen AppImage integral-density region/window/desktop, independent paste and MP4 static/moving/bar checks pass as scoped below. |
| Fedora 44 / native GNOME Wayland, Intel i915 | FHD + QHD on independent pipes; original first-head stream plus separate native second-head portal stream both fresh | No put.gd process launched in this bounded feasibility slot. |
| Ubuntu 26.04.1 / GNOME 50.1 Wayland, fresh KVM overlay | Two separate virtio scanouts and per-head wall listeners | Frozen AppImage adjacent regions/span/windows/desktop/paste/Escape/right-click/MP4 passed after common v3 preflight; resize/disable during selection passed. Separate fix artifact passed five targeted regression cases. Scaled/rotated stages failed their wall prerequisite before app cases. |

The current fractional oracle limitation is documented in the lab guide. The
remaining matrix includes complete scaled/rotated pixel attribution, actual
focus-loss coverage, topology changes on each required compositor and a supported
retained-negative-coordinate profile. No incomplete cell is waived here.

## Saved visual annotations

The annotated overview boxes each native Mint output and separates the original
and fixed KDE captures. Its labels name each tested build. Annotations are a
separate browser-rendered layout; the original images below retain their exact
saved pixels and hashes. The 110 remote CTests used the same product patch at
`687f17d6`; the exact fix AppImage identity is the CI merge listed above.

![Annotated native Mint geometry and original/fixed KDE capture comparison](annotated-pilot.png)

Mint native rotated arrangement: requested secondary origin `(-1560,-100)`;
XRandR actually placed the rotated QHD head at `(0,0,1440,2560)` and FHD primary
at `(1560,100,1920,1080)`, leaving a 120-unit horizontal gap. This is normalized
left/up evidence, not retained negative coordinates. Distinct palettes identify
the two native outputs; the moving-frame gate passed before the app campaign.

![Mint native rotated heads, with separate palettes and visible gap](mint-native-rotated-wall.png)

Frozen KDE wrong-head finding: the visible outline is on the left FHD output;
the saved selection returned the right output's palette. It reproduced with
both uinput and portal-VNC pointer routes. The Settings window is the isolated
test instance; no credential value is displayed.

![Frozen KDE primary-output outline](kde-native-outline-witness.png)

| Frozen primary selection: wrong output, 241×180 | Fixed primary selection: correct output, 160×120 |
| --- | --- |
| ![Wrong secondary-output palette](kde-before-primary-region.png) | ![Correct primary-output palette](kde-fixed-primary-region.png) |

The fixed primary image also pasted with byte-identical RGBA pixels in the
independent native GTK application. The secondary crop completed after the fix;
its original-grid outward rounding and fractional edge colors require the
separate reference validation, rather than a nominal-DPR assertion.

Fedora native head 0 and head 1 were simultaneously visible on the wall through
separate native portal streams. Their exact stream sizes were 1920×1080 and
2560×1440; changed-pixel counts were 1,036,710 and 1,267,200 respectively. The
synthetic coverage gate was at least 99% on each. These pictures do not show
physical panels or a cable removal.

![Fedora native first head](fedora-native-head0.png)

![Fedora native second head](fedora-native-head1.png)

## Failure context and restoration

GNOME guest `g1/adj3` repeated all nine adjacent-layout cases on original
`6b7949f5` after the common disposable-guest preflight from
[#584](https://github.com/patrick-hudson/put.gd-desktop/pull/584). Helper v3 SHA-256
was `a6322f30d50ba0e5c87360e8c316a4d5b6396bdccfc5b9a90a3aa37ea8594835`.
Apply/check-only receipts verify real desktop configuration homes, policy values,
an actual logind inhibitor, ScreenSaver inactive, unlocked active Wayland session,
native output inventory and full QMP witnesses. The first helper's inhibitor
startup race is retained; the corrected bounded readiness check passed. Historical
`adj2` results remain separate from this corrected stage.

Original-build `resize1` and `loss1` changed the disposable secondary output
while selection was active. Both ended with `display_changed`, no stale artifact
and unchanged text pasted into the independent application. This is software
output reconfiguration, not a physical cable removal. The original two-output
layout was restored, followed by policy and all-head freshness checks.

Separate `g1/fix1` used the exact `4610dda66a9d` CI AppImage. Regions inside each
head and desktop capture had zero pixel mismatches and byte-identical independent
paste; Escape and right-click preserved the sentinel. This five-case regression
does not establish all-desktop acceptance of the new build. Both source/package
receipts are archived. The first final wall check failed at head-0 synthetic
coverage 0.5; an identical-source/layout bounded retry passed both heads with
unchanged thresholds. Both attempts remain in the evidence.

GNOME accepted a rotated secondary output at scale 2 and a separate unrotated
scale-2 left/up layout, but their per-head VNC frame prerequisites failed before
app campaigns. No geometry or recording pass is inferred for either profile.
The original adjacent layout was restored and its fresh frames re-established.

The [Mint](mint-case-summary.json), [KDE](kde-case-summary.json) and
[GNOME](gnome-case-summary.json) summaries preserve individual case outcomes.
Mint's first whole-window/desktop mismatch was visibly its preceding success
toast; the separate rerun disabled notifications only in the isolated settings
and passed the unchanged exact pixel check. The initial failures remain in the
summary. The recording fixture was changed to repeat motion across the output,
retaining an earlier narrow-region control that missed a sweeping bar.

The RFB partial-frame fix `421214b4` preserves unchanged pixels and invalidates
them on resize. The old reader produced black areas on GNOME despite complete
QMP frames; two protocol tests cover this correction. The old results retain
their original helper hashes. A Cinnamon VM rotation also produced a black
**QMP** secondary scanout, so that separate rotation block is not attributed to
the RFB reader. Its actual-focus-change attempt did not steal the keyboard grab
and is not a focus-loss pass.

All native trials restored original EDID/force state and the original single
display. KWin files were byte-compared with saved originals; Mint and Fedora
preserved the original absence of `monitors.xml`. Existing user windows, Windows
VMs, collectors, signer and original VNC services were preserved. Owned input
units/sockets, fixtures, supplemental VNC servers, tunnels and tiles were
removed. Only added diagnostic packages were purged. Physical idle/lock/power
policies and boot configuration were unchanged; no physical logout or reboot
was performed.

Verified-clean releases: Mint `01:17:28Z`, Kubuntu corrected completion
`01:50:58Z`, Fedora `02:09:14Z` on 2026-10-26. Fedora's slot took 16m02s,
including package-provider correction and a failed cleanup frame attempt that
overlapped Overview restoration. The corrected sequential frame check passed,
the cover stopped, and the final clean visual/status preceded handoff to RSS.
The failed attempt is retained. Debian's earlier cleanup correction and
original-service preservation remain in the private coordination receipts.

The Cinnamon VM archive is 17,254,183 bytes, SHA-256
`de157b5606d4fae43092e2e48f613b3e5f4f2defb0eed43c9e35d0d677fff1d3`;
all 647 archived members were verified before guarded overlay retirement.
Its original and corrected post-reset manifests remain preserved. The GNOME
archive is 110,236,106 bytes, SHA-256
`6ab4ee3057b0c5fda703a21debe74b421caaa9d6ce5d1641c1fb403b96100806`;
all 620 archived evidence members were verified on the workstation before guarded
retirement completed at `2026-10-26T03:17:41.980201Z`. Its completion manifest
records a failed/partial campaign, preserving the unproved matrix cells.
The owned QEMU, SSH/VNC listeners, two guest wall tiles and tunnel were removed;
the Debian signer and shared collector remained. A fresh physical desktop visual
and `status --check` preceded explicit physical/quiet-heavy handback to acceptance.

Original evidence and cleanup receipts remain in private coordinated builder
runs. Curated PNGs here are unmodified saved evidence;
[asset-sha256.json](asset-sha256.json) indexes their bytes. No SSH private key,
protected login input, payload credentials or VNC restore token is published.
