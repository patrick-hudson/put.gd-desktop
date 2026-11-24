# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## DEB/RPM exact-asset audit rehearsal (#383) — 2026-10-22

Assets: the ubuntu24.04, ubuntu26.04 and debian13 DEBs and the fedora44 RPM of
`v26.1022.1017-nightly` (`3582b30a`, after #424 and #428), from
[Nightly run 39864143584](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39864143584).
The README lists each package's SHA-256. Audited on f44, with lint in rootless
containers, and identities on m74, k72 and d69. Evidence and repeatable scripts:
[`records/2026-10-22-native-license-audit/`](https://github.com/patrick-hudson/put.gd-evidence/tree/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-22-native-license-audit). This is a nightly pass; it
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
