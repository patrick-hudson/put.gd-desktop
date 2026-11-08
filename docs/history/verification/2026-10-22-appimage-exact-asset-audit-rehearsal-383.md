# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## AppImage exact-asset audit rehearsal (#383) — 2026-10-22

Asset: `putgd-26.1022.968-nightly-x86_64.AppImage`, SHA-256
`dc17d5c6fb92a35514f381051d58f9a0170fad2b40c7e0e328dae64b219f51cf`, from
[Nightly run 39737710388](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39737710388)
on `ddc7981e`. Audited on m74 (Ubuntu 24.04 base). Evidence and repeatable
scripts: [`records/2026-10-22-appimage-license-audit/`](https://github.com/patrick-hudson/put.gd-evidence/tree/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-22-appimage-license-audit). This is a nightly
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
  ([evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-23-appimage-qt612-418/README.md)).

Not covered: the Proton per-release check. The DEBs and the RPM are in the
section above.
