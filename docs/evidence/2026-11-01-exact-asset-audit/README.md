# Exact-asset license, source and CVE rehearsal — 2026-11-01

Rerun of the [2026-10-31 rehearsal](../2026-10-31-exact-asset-audit/README.md)
on the newest published nightly at the start of this pass,
**v26.1101.3698-nightly**, commit `38fa2ac634d475733e2ebfe70c2871da6425e31b`,
tree `82f7582dce6e`, count 3698. It is the comparison run after the two findings
that blocked 3384 were fixed: Bun 1.4.4 relink ([#1054](https://github.com/patrick-hudson/put.gd-desktop/issues/1054)/[#1071](https://github.com/patrick-hudson/put.gd-desktop/issues/1071))
and the scoped HarfBuzz disposition ([#1055](https://github.com/patrick-hudson/put.gd-desktop/issues/1055)/[#1065](https://github.com/patrick-hudson/put.gd-desktop/pull/1065)).

**Result: the stable-policy audit passes on these nightly bytes with zero errors
and zero blocking findings.** This is a pass on published nightly bytes, not on
release bytes. [#384](https://github.com/patrick-hudson/put.gd-desktop/issues/384)
reruns the same driver on the exact stable draft. No legal, live-service or
desktop acceptance is claimed. [#383](https://github.com/patrick-hudson/put.gd-desktop/issues/383)
and [#511](https://github.com/patrick-hudson/put.gd-desktop/issues/511) track
this rehearsal.

## Identity and scope

| | |
| --- | --- |
| Release | [`v26.1101.3698-nightly`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1101.3698-nightly), producing run [41308870029](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/41308870029) |
| Assets | 15 installer/SBOM/support assets (`SHA256SUMS` `30cccc40…`), four linked source assets from `sources-v26.1101.3698-nightly-files`, two Proton transport parts (1,300,000,000 + 1,094,441,755 bytes) from `proton-sources-0.23.0-crypto2.2.0-bun1.4.4` |
| Driver | `tools/releases/audit_assets.py` at nightly `7c59a53cc3e412cf3387e9f602947f57ea82e0a8` (script SHA-256 `c11813c09a88…`), plus `check_source_build.py` |
| Host | physical **d69 / debian** (Debian 13), headless over pinned SSH under `~/work/audit-3698/`; no desktop session, keychain, Proton account or user destination. Nothing ran on the workstation. |
| Time (UTC) | downloads and first pass 2026-11-01 23:53–23:58Z; rebuild 23:57–00:07Z; final bound pass 2026-11-02 00:40–00:44Z. Dated 2026-11-01, the campaign day; d69's clock runs at UTC−6, so its file times read six hours earlier. |
| Scanner | Syft v1.55.0, Grype v0.121.0 (`packaging/sbom/tools.lock.json`); database `v6.1.10` built `2026-11-01T06:20:12Z`, archive SHA-256 `d0e1af9e6491440e1a671d0a0aafc32c5c7cb5cda787dcec3482799ef298f31c` ([database.json](database.json)) |

| File | What it holds |
| --- | --- |
| [audit.json](audit.json) | The final driver result: every check, inventories, notices, dependencies, scan and the bound rebuild receipt |
| [database.json](database.json) | Grype database identity |
| [vulnerability-summary.md](vulnerability-summary.md), [vulnerability-report.json](vulnerability-report.json) | Stable-policy classification of the regenerated SBOMs |
| [published-vulnerability-summary.md](published-vulnerability-summary.md) | CI's nightly-policy scan of the published SBOMs (artifact `vulnerability-report` of run 41308870029), for comparison |
| [regenerated-sbom-receipts.json](regenerated-sbom-receipts.json) | Hashes of the five SBOMs regenerated on d69 |
| [source-rebuild-results.json](source-rebuild-results.json), [source-rebuild-parameters.json](source-rebuild-parameters.json) | The separate exact-source rebuild |
| [maintenance.json](maintenance.json) | The dated dependency and Proton upstream/security/migration review (#383 item 5) |
| [manual-checks.json](manual-checks.json) | Signature, mirror, feed and zsync checks the driver does not perform |
| [raw-evidence-index.txt](raw-evidence-index.txt) | SHA-256 of every raw file kept privately on d69 |
| [EVIDENCE-SHA256SUMS](EVIDENCE-SHA256SUMS) | Checksums of this packet |

## Results

The final driver run exited **0**: `passed: true`, `errors: []`. The first pass
(before the rebuild) reported exactly one error, the expected
`No exact-source production rebuild receipt` for a nightly, and nothing else.

| # | Item | Result | Evidence |
| --- | --- | --- | --- |
| P1 | Checksums and identity: all 15 assets verify, GitHub digests match, tag commit/tree/count/date match `release-builds.json` and the four pinned build-input files at that commit | **pass** | `checks.assets`, `checks.remoteAssets`, `checks.identity`, `checks.buildInputs` |
| P2 | `SHA256SUMS.minisig` (published at pkg.put.gd) verifies with the pinned nightly key; feed `.minisig` too | **pass** | [manual-checks.json](manual-checks.json) |
| P3 | pkg.put.gd `SHA256SUMS` copy, `update/v1/nightly.json` and `app.put.gd/dl/nightly/appimage.zsync` agree with the release | **pass** | [manual-checks.json](manual-checks.json) |
| N1 | Native DEB/RPM: inventories, modes and root ownership, nine programs, private library directory, `libputgd-s3.so` runtime module identity, metadata and notices (`runtime-notices.txt`, DEP-5 `copyright`, AWS/component snapshots, `proton/THIRD-PARTY-NOTICES.txt`) | **pass** on all four | `checks.packages.<target>.audit` |
| N2 | AppImage: 139 inventoried licence components (the SBOM counts 169 components because it also lists the AWS CRT libraries and the helper's npm packages one by one), 658 files, licence texts present for every component, AppStream licence expression matches, no native notice, no linuxdeploy leftovers, no second copies, 226 `$ORIGIN`-relative search paths, `--version-json` identities of the compiled programs | **pass** | `checks.packages.appimage.audit` |
| N3 | Curated source export: 484 files, hashes and modes verified against the tagged Git blobs, identity `38fa2ac6`/3698, Proton catalog bound to `tools/proton/notices/SOURCES.json` | **pass** | `checks.sourceExport` |
| N4 | Complete AppImage corresponding source (759,191,721 bytes) closes over the runtime, AWS SDK, FFmpeg, ICU, LayerShellQt, mimalloc, Qt modules, QtKeychain, the Proton helper and every bundled Ubuntu source package | **pass** | `checks.completeSource` |
| N5 | Proton corresponding source: both transport parts concatenate to the catalog hash, six inner packets verify, all five helper copies are `9fd4d72f…` and match the catalog, runtime manifest `35b17e01…`, `@openpgp/web-stream-tools` notice present (#369) | **pass** | `checks.protonSource`, `checks.packages.<target>.proton` |
| S1 | Separate exact-source rebuild on d69 (normal `release` preset, distro Qt 6.8.2, pinned AWS SDK 1.11.907): **passed in 590 s**, all eight programs identity-matched, the rebuilt Proton helper reproduces the published `9fd4d72f…` byte for byte | **pass** | [source-rebuild-results.json](source-rebuild-results.json) |
| D1 | Regenerated SBOMs: the AppImage SBOM regenerated on d69 is byte-identical to the published one (`fbf462abb770dda3…`); the four native SBOMs regenerate with equal component sets | **pass** | [regenerated-sbom-receipts.json](regenerated-sbom-receipts.json), [raw-evidence-index.txt](raw-evidence-index.txt) |
| D2 | Stable-policy scan with a fresh database: **180 findings, 31 accepted, 0 blocking**; every disposition's component/version/library fingerprint and absent-tool scope still matches these bytes | **pass** | [vulnerability-summary.md](vulnerability-summary.md), `checks.vulnerabilities`, `checks.dependencies` |
| D3 | Official Bun advisories against the embedded runtime 1.4.4 | **pass**, none | `checks.upstreamAdvisories: []` |
| X1 | Dated Proton/dependency maintenance review (#383 item 5) | **done**, no action before stable | [maintenance.json](maintenance.json) |

### Vulnerabilities

All 180 scanner findings are in the AppImage; each native SBOM has zero Grype
findings. The 33 Critical/High rows, which the stable gate blocks on when a fix
exists:

- **29 FFmpeg 7.1.5** findings and **1 ICU 73.2** finding are accepted by the
  scoped dispositions from #538/#539 until **2027-01-21**, unchanged.
- **CVE-2026-113679 HarfBuzz 14.5.0** (Qt's embedded copy) is accepted by #1065's
  scope (absent WASM shaper, keyed on the `libQt6Gui` fingerprint) until
  **2027-01-29**; [#1070](https://github.com/patrick-hudson/put.gd-desktop/issues/1070) tracks the revisit.
- **CVE-2025-3887** appears twice, for Ubuntu's `gstreamer1.0-plugins-bad` and
  `libgstreamer-plugins-bad1.0-0` 1.24.2-1ubuntu4. Both rows are reported, not
  blocking. Ubuntu's fix, `1.24.2-1ubuntu4+esm1`, is published only through
  Ubuntu Pro ESM Apps, not in the public noble archive the AppImage bundles from,
  so Grype records the finding as `not-fixed` and the fixable-only gate does not
  trip. The affected H.265 parser has no identified product input path (#543).

Compared with the 2026-10-31 run on 3384 (180 findings, 30 accepted, **1
blocking**, plus three embedded Bun advisories): the blocking HarfBuzz finding
is now dispositioned, and the Bun advisories are gone because the helper runs on
1.4.4. The published nightly-policy scan ([published-vulnerability-summary.md](published-vulnerability-summary.md))
reports the same 33 Critical/High rows with the same statuses, so published and
regenerated SBOMs agree.

Nothing here adds a waiver or weakens the policy. Raw findings are retained.

### Dated maintenance review

[maintenance.json](maintenance.json) records pins against upstream on
2026-11-01: AWS SDK 1.11.907 (upstream 1.11.921, no advisory after the pin),
Qt 6.12.0 (current), FFmpeg 7.1.5 (unchanged scope), Bun 1.4.4 (newest release;
the three October advisories are patched in it), `@protontech/drive-sdk` 0.23.0
and `@protontech/crypto` 2.2.0 (both npm latest). Proton's README still targets
its cryptographic migration for end of 2026/early 2027 with final timing pending.
The Live Proton cell of the nightly table is green on all five desktops for this
nightly; browser authentication/refresh, protected/unprotected download equality,
sharing history and journal compatibility were not rerun here.

## Reproduction and retained evidence

```sh
# on d69, repo at nightly 7c59a53cc, GH_TOKEN in the environment
python3 repo/tools/releases/audit_assets.py --tag v26.1101.3698-nightly --download \
  --directory ~/work/audit-3698/assets --output ~/work/audit-3698/exact-audit
python3 repo/tools/releases/check_source_build.py \
  --receipt ~/work/audit-3698/exact-audit/sources/source-archive.json \
  --sdk-prefix ~/work/put.gd/build/sdk \
  --sdk-source ~/work/put.gd/build/dependencies/aws-sdk-cpp \
  --output ~/work/audit-3698/rebuild --bundle-proton
python3 repo/tools/releases/audit_assets.py --tag v26.1101.3698-nightly \
  --directory ~/work/audit-3698/assets --output ~/work/audit-3698/exact-audit-rebuilt \
  --reuse-downloads ~/work/audit-3698/exact-audit \
  --source-build-result ~/work/audit-3698/rebuild/results.json
```

Two things went wrong on the way and are recorded rather than hidden: the first
bound rerun (`audit2.log`) failed at once because `gh` had no token in its
environment; `rpm` prints `cannot open Packages database in /var/lib/rpm` on
Debian while querying the RPM, which is noise and did not affect the result.

Raw downloads, the linked source assets, both Proton transport parts, the five
regenerated SBOMs, the five raw Grype results, the rebuild receipts and log and
all three driver logs stay private on d69 under `~/work/audit-3698/`;
[raw-evidence-index.txt](raw-evidence-index.txt) holds each file's SHA-256. The
1.8 GB rebuild tree (`rebuild/putgd-26.1101.3698-nightly-source/`) is retained
without per-file hashes; its outputs are bound by the receipt. The receipts and
`audit.json` name d69's own paths under `/home/patrick/work/`, which are the
builder's working directories, the same as in the 2026-10-31 packet; no
credential, payload or workstation path is in this packet.

On release day, repeat the three commands on the stable draft from a fresh
checkout of the stable tag itself (so the policy, dispositions and tool lock are
the ones the tag carries, not nightly's), into new output directories, and
compare `checks.vulnerabilities`, `checks.upstreamAdvisories` and the
dispositions' fingerprints with this record. The Release workflow's own run on
the uploaded draft does the same from the tag's checkout.
