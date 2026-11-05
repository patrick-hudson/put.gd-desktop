# AppImage exact-asset audit rehearsal (#383) — 2026-10-22

This is a rehearsal of the first-stable license, notices, provenance and
dependency audit ([#383](https://github.com/patrick-hudson/put.gd-desktop/issues/383))
on the AppImage of the first CalVer nightly. It is a pass on nightly bytes, not on
release bytes. The release campaign
([#384](https://github.com/patrick-hudson/put.gd-desktop/issues/384)) reruns the
same commands on the stable draft.

| | |
| --- | --- |
| Asset | `putgd-26.1022.968-nightly-x86_64.AppImage` |
| SHA-256 | `dc17d5c6fb92a35514f381051d58f9a0170fad2b40c7e0e328dae64b219f51cf` |
| Bytes | 115,374,584 (runtime 944,632 + SquashFS 114,426,133, zstd, 16 KiB blocks, plus padding) |
| Release | [`v26.1022.968-nightly`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1022.968-nightly), commit `ddc7981e4a325f5c75bee39a57170a89d74685fd`, tree `fc8d9ba4`, count 968 |
| Built by | [Nightly run 39737710388](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39737710388), `workflow_dispatch` on `nightly`, 07:01:53–07:31:01Z, attempt 1, every job green |
| Audited on | m74 (Linux Mint 22.3, Ubuntu 24.04 `noble` base, the AppImage's build distro); minisign cross-check on d69. Nothing ran on the workstation. |
| Audited at | 2026-10-22 17:45–19:30Z, apt indexes from 2026-10-22 |

The DEBs and the RPM were not audited in this pass. The AppImage was the priority.

| File | What it holds |
| --- | --- |
| `results.json` | The result table below, machine-readable, with the asset identity |
| `data/probe.json` | Runtime size (SquashFS offset), SHA-1/SHA-256, `.upd_info`, the runtime's ELF sections |
| `data/image-buildids.json` | GNU build ID of each of the image's 249 ELF files |
| `data/match.json` | Which image ELFs match which current noble package, and each package's binary and source version |
| `data/sizes.json` | The size breakdown below |
| `data/ubuntu-source-index.json` | Exact-version Ubuntu source files and sizes for the 94 bundled source packages |
| `data/ubuntu-open-cves.json` | The Ubuntu CVE tracker's open noble CVEs for those packages |
| `tools/` | The scripts used: `minisign_verify.py`, `appimage_probe.py`, `runtime_cmp.py`, `buildids.py`, `match.py`, `sizes.py`, `ubuntu_cves.py` |

## Result

| # | Item | Result | Evidence / gap |
| --- | --- | --- | --- |
| P1 | `SHA256SUMS` lists the image, `.zsync`, both source assets and `release-builds.json`, and they verify | **pass** | `sha256sum --check --strict --ignore-missing`: 5/5 OK. All 15 entries equal GitHub's asset `digest`, and the `pkg.put.gd/releases/v26.1022.968-nightly/SHA256SUMS` copy is byte-identical. |
| P2 | `SHA256SUMS.minisig` verifies with the pinned nightly key | **pass** | Key `71440FA5E439AEBB` (`packaging/linux/repo/keys/putgd-nightly.minisign.pub`). Both minisign 0.12 on d69 and the independent `tools/minisign_verify.py` (Ed25519 over BLAKE2b-512) verify it. Trusted comment: `put.gd v26.1022.968-nightly nightly`. |
| P3 | The image matches its CI run and `release-builds.json` | **pass** | Tag → `ddc7981e` = the run's `head_sha`; the receipt's `build` has the same commit, tree and count, with `dirty: false`. The receipt `artifact.sha256` is the image's SHA-256. All 759 receipt `inventory` entries (741 files, 18 symlinks) match the extracted SquashFS byte for byte, and no file is unlisted. `putgd`/`putgd-cli --version-json` from the extracted tree report `ddc7981e`, `26.1022.968-nightly`, not dirty. The GNU build IDs of all 10 put.gd ELFs appear in the `debugSymbols` receipt. |
| P4 | The runtime is the pinned type2-runtime | **pass** | `tools/runtime_cmp.py`: the first 944,632 bytes equal `runtime-x86_64` 20251108 (`2fca8b44…`, lock digest) apart from the writable `.upd_info`, `.digest_md5`, `.sha256_sig` and `.sig_key` sections. The image carries no embedded signature (`.sha256_sig` is all zeros); trust comes from P2. |
| P5 | Update information and zsync agree with CalVer | **pass** | `.upd_info` = `zsync\|https://app.put.gd/dl/nightly/appimage.zsync` (nightly channel). The `.zsync` header has `Filename: putgd-26.1022.968-nightly-x86_64.AppImage`, `Length: 115374584`, `SHA-1: 9feee5d8…` (= the image), and `URL: https://pkg.put.gd/releases/v26.1022.968-nightly/…`, which serves the identical bytes (SHA-256 checked). `app.put.gd/dl/nightly/appimage.zsync` is byte-identical to the release's `.zsync`. `update/v1/nightly.json` names this tag, size and SHA-256. |
| N1 | Every bundled library has a licence text or copyright notice | **pass, with gaps** | All 124 Ubuntu binary packages have their `copyright` file under `bundled-licenses/distro/`, plus `/usr/share/common-licenses`. FFmpeg has COPYING.GPLv2/GPLv3/LGPLv2.1/LGPLv3 and LICENSE.md, QtKeychain has COPYING, and there are the AWS SDK/CRT/s2n Apache-2.0 texts, the runtime's MIT and libfuse notice, and the Proton notices. **Gap:** the Qt modules' and LayerShellQt's own `LICENSES/LGPL-3.0-only.txt` (and the other SPDX texts) are dropped by the collector's name filter → [#417](https://github.com/patrick-hudson/put.gd-desktop/issues/417). |
| N2 | The notices describe the AppImage accurately | **fail** | `runtime-notices.txt` is the native-package text ("No compiler or Qt runtime is bundled"). Nothing lists component, version and licence. `THIRD_PARTY_NOTICES.md` doesn't mention the AppImage, and the AppStream `project_license` (`MIT AND Apache-2.0 AND GPL-3.0-only`) omits GPL-2.0-or-later → [#417](https://github.com/patrick-hudson/put.gd-desktop/issues/417). |
| N3 | GPL corresponding source for the declared GPL parts | **pass** | `-appimage-media-source.tar.gz` (`34946156…`) is bound to the image SHA-256. Its FFmpeg tar is `git archive` of `3a0867c2` = upstream tag `n7.1.5`, and `SOURCES.json` hashes match. The x264 `2:0.164.3108+git31e19f9-1`, x265 `3.5-2build1` and gst-plugins-ugly1.0 `1.24.1-1build1` `.dsc`/orig/debian files match `SOURCES.json` and the receipt's `gplComponents`. The bundled `libx264`, `libx265` and `libgstx264` match those Ubuntu builds by build ID. |
| N4 | Source for everything else the GPL/LGPL terms reach | **fail (decision)** | No source and no written offer for Qt 6.8.3 (LGPL-3.0-only), LayerShellQt, the 94 Ubuntu source packages (GStreamer, GLib, GTK, libpulse, GnuTLS, …), or the runtime's static libfuse. Complete source would be about 290 MB: 237,030,319 bytes of exact-version Ubuntu sources plus about 52 MB of Qt module tarballs → [#416](https://github.com/patrick-hudson/put.gd-desktop/issues/416) (needs Patrick). |
| N5 | LGPL relinkability | **pass** | Every LGPL library is a separate shared object with an `$ORIGIN` RUNPATH. Nothing in put.gd checks their hashes (only the Proton helper's own SHA-512), and the programs run from an extracted AppDir (`--appimage-extract`). The one exception, static libfuse in the runtime, is upstream's binary and is covered by N4. |
| N6 | Codecs with patent or licensing caveats | **flagged for Patrick** | H.264 (x264, `x264enc`, FFmpeg `libx264` and the `h264` decoder), H.265 (x265, FFmpeg `libx265`) and AAC (`voaacenc`, FFmpeg `aac`) → [#419](https://github.com/patrick-hudson/put.gd-desktop/issues/419). Not decided here. |
| C1 | No debug or test files | **pass** | No `.debug_info` or `.debug_line` section in any of the 249 ELFs. No `.a`, `.la`, `.h`, `.pc`, `.cmake`, `.o` or `.pyc` files. The only test-like file is `libgstvideotestsrc.so`, listed in `media.json` for the lab diagnostics. |
| C2 | No secrets | **pass** | Grep for AWS key IDs, PEM private keys, GitHub/Slack/OpenAI tokens and `aws_secret_access_key` found only PEM-parser header strings (Qt Network, GnuTLS, GIO, libssh, the Qt OpenSSL plugin) and a Unicode name fragment in libunistring. There are no key or cert files, and no `/home/patrick` or other personal paths. |
| C3 | No absolute build paths | **pass, with one gap** | One absolute RUNPATH: `liblayer-shell.so` → `/home/runner/work/…` → [#421](https://github.com/patrick-hudson/put.gd-desktop/issues/421). 40 files contain CI-runner path strings (asserts, FFmpeg's configure line, the guarded build-tree lookup in `S3Module.cpp:78`); for information only. |
| C4 | Size reasonable, with a breakdown | **pass (see below)** | 110 MiB download; the Proton helper is 36 % of it. |
| D1 | Bundled Ubuntu packages are current | **pass** | All 153 distro ELFs match, by GNU build ID, the current noble-updates/noble-security build of their package (124 binary, 94 source packages) → `data/match.json`. |
| D2 | Bundled versions with known CVEs | **fail (decision)** | Qt 6.8.3 and the code it embeds have many later CVEs → [#418](https://github.com/patrick-hudson/put.gd-desktop/issues/418) (needs Patrick). FFmpeg, the AWS SDK/CRT and the Ubuntu packages are current; see the inventory. A stable AppImage freezes the Ubuntu snapshot → [#420](https://github.com/patrick-hudson/put.gd-desktop/issues/420). |
| X1 | #369: Proton `THIRD-PARTY-NOTICES` includes `@openpgp/web-stream-tools` | **pass on this asset** | Both 0.3.1 and 0.1.3 are listed in `usr/libexec/putgd/proton/THIRD-PARTY-NOTICES.txt` (identical to the `usr/share/doc` copy). The npm pin that #369 asks for is still open. |
| X2 | Curated `-source.tar.gz` (spot check) | **pass** | `473559ff…`: 431 members, no `work/`, `evidence/`, `plans/` or `tests/`, no credentials (same grep as C2), no personal paths. CI's `sourceBuild` receipt: passed, 359 s, all 8 programs identity-matched, Proton helper rebuilt to the bundled SHA-256 `e93b3b52…`. Not rebuilt again here. |

Not covered in this pass: the DEBs and the RPM; the Proton per-release
compatibility check and the concatenation of the 2.48 GB Proton source parts
(#383 items 3 and 5; CI's `protonSource` receipt binds them to catalog
`6d5d4798…`); and a second, independent rebuild of the curated source.

## Size breakdown

The extracted AppDir is 305.8 MB. Each group below was packed again on its own
with `mksquashfs -comp zstd -b 16384`. The whole tree packs to 114,429,952
bytes, against 114,426,133 in the image (0.003 % apart). The groups sum to a
little more than the whole because SquashFS can't share fragments across groups.

| Group (asset `dc17d5c6…`) | Files | Unpacked | In the image | Share |
| --- | ---: | ---: | ---: | ---: |
| Proton helper (`putgd-proton-runtime`, Bun-compiled, and its notices) | 30 | 89.6 MB | 41.8 MB | 36 % |
| Qt 6.8.3 (libraries, ICU 73.2, plugins, translations), QtKeychain, LayerShellQt | 81 | 83.7 MB | 32.6 MB | 29 % |
| Other Ubuntu libraries (GTK3/GLib stack, GnuTLS, OpenSSL, curl, xcb, …) | 98 | 43.6 MB | 16.0 MB | 14 % |
| Recording (GStreamer and plugins, FFmpeg 7.1.5, x264, x265, libvpx, libwebp, Opus, AAC, …) | 61 | 44.4 MB | 13.2 MB | 12 % |
| put.gd's own programs, S3 module, desktop files, icons, GNOME extension | 26 | 16.9 MB | 6.9 MB | 6 % |
| AWS SDK for C++ 1.11.907, CRT, s2n-tls | 30 | 23.5 MB | 3.4 MB | 3 % |
| Licences and notices (`usr/share/doc`) | 415 | 4.1 MB | 0.5 MB | 0.5 % |
| type2-runtime | 1 | — | 0.9 MB | 1 % |

Largest single files: the Proton helper (82.9 MB unpacked), `libicudata.so.73`
(32.0 MB), `libx265.so.199` (16.5 MB), `libQt6Gui` (11.1 MB) and `libgtk-3`
(8.4 MB). About 16 MB of the extracted tree are copies (six Qt plugins in
`usr/lib`, the AWS libraries twice, the Proton notices twice). SquashFS
deduplicates them, so they hardly add to the download
([#421](https://github.com/patrick-hudson/put.gd-desktop/issues/421)). The
experimental Proton helper is the one lever large enough to change the download
noticeably. Whether Proton ships in stable is already decision 6 in the #44 plan.

## Dependency inventory (dated 2026-10-22)

Upstream pins:

| Component | Bundled version | Upstream now | Known CVEs affecting it | Action |
| --- | --- | --- | --- | --- |
| Qt (official binaries) | 6.8.3 (qtbase `c07c2d5a`, qtwayland `8ab0d5b4`, qtsvg `c75099a7`, qtimageformats `3b6b1fb9`) | Open-source binaries 6.10.3, 6.11.2, 6.12.0; 6.8.4 and 6.8.5 are source-only (6.8.5 posted 2026-10-16); the online repo stops at `qt6_683` | 23 per [Qt's list](https://wiki.qt.io/List_of_known_vulnerabilities_in_Qt_products), for example CVE-2025-10728 (SVG, CVSS 9.4), CVE-2025-10729, CVE-2026-6210, CVE-2026-8168 (SVG), CVE-2025-5683 (ICNS), CVE-2025-5992 (ICC), CVE-2025-14575 (TLS CA path) | [#418](https://github.com/patrick-hudson/put.gd-desktop/issues/418), needs Patrick |
| libpng in QtGui | 1.6.47 | 1.6.5x | Later libpng releases fixed CVE-2025-64505, -64506, -64720, -65018 (1.6.51) and CVE-2025-66293 (1.6.52) | with #418 |
| libtiff in `libqtiff` | 4.7.0 | 4.7.1+ | CVE-2025-9900 (write-what-where on a crafted TIFF height; fixed upstream in 4.7.1) | with #418 (or drop the plugin) |
| SQLite in `libqsqlite` | 3.49.1 | 3.50.x+ | CVE-2025-6965 (fixed 3.50.2), CVE-2025-7709 (FTS5, 3.50.3); only reachable through put.gd's own local history database | with #418 |
| libjpeg-turbo in `libqjpeg` | 3.0.3 | 3.1.x | none found in this quick check | with #418 |
| ICU (shipped with Qt) | 73.2 | 77+ | CVE-2025-5222 is in the `genrb` tool, which isn't shipped | with #418 |
| FFmpeg | 7.1.5, `3a0867c2` = tag `n7.1.5` | 7.1.5 is the newest 7.1 point release (2026-06-20); 8.1.2 is current | Later distro advisories fix demuxers and decoders that `--disable-everything` leaves out (TIFF, VobSub, NVDEC, S/PDIF, RTP/ASF); none was found in the enabled `matroska`, `mov`, `gif`, `h264`, `aac`, `libvpx_vp9`, `opus` or `mjpeg` components | recheck on release day |
| AWS SDK for C++ | 1.11.907 (`a9f0ffcc`) | 1.11.915 | s2n-tls CVE-2026-16317/-16318 are fixed in v1.7.6, and the pinned s2n `853d1943` is 16 commits past it; aws-c-http CVE-2026-12043 is fixed in v0.11.0, and the pin `8aefd899` *is* v0.11.0 | none; recheck |
| QtKeychain | `8eac9889` | — | none known | — |
| LayerShellQt / ECM | 6.4.6 `b1317ba1` / 6.14.0 `a5f0366e` | — | none known | — |
| type2-runtime | 20251108 `dd6cebed` | — | none known | — |
| Proton helper | Bun 1.4.2, SDK `js@0.22.2`, crypto 2.0.1 | — | not checked here (#383 item 5) | dated Proton check |
| appimagetool / linuxdeploy / zsyncmake | 1.9.1 / 1-alpha-20251107-1 / 0.6.2 | build tools only, not shipped | — | — |

Ubuntu 24.04 packages: all 94 source packages are at the current
noble-updates/noble-security version (D1). The Ubuntu CVE tracker still lists
these CVEs as open (`needed`, `deferred` or `pending`) for noble at those
versions (`data/ubuntu-open-cves.json`):

27 open CVEs across 12 source packages: none high or critical, 21 medium and
6 low. 11 are `needed` (no fix published yet) and 16 are `deferred`.

| Source package | Open on noble (priority, status) |
| --- | --- |
| pcre2 10.42-4ubuntu2.1 | 7 medium, needed (CVE-2026-103111, -86145, -89156, -89157, -89158, -89160, -89161) |
| gnutls28 3.8.3-1.1ubuntu3.6 | 5 medium, deferred (CVE-2026-67693, -88647, -95184, -95209, -95210) |
| libsndfile 1.2.2-1ubuntu5.24.04.1 | CVE-2026-37555 medium needed; CVE-2025-52194, CVE-2026-88372 medium deferred; CVE-2024-50613 low deferred |
| gst-plugins-base1.0 1.24.2-1ubuntu0.5 | CVE-2026-85150 medium, needed |
| x264 2:0.164.3108+git31e19f9-1 | CVE-2025-25467 medium, deferred |
| glib2.0, cyrus-sasl2, libxrender, pixman | one medium deferred each (CVE-2026-86469, CVE-2026-107161, CVE-2026-88807, CVE-2023-37769) |
| elfutils, cairo, systemd | low only (CVE-2025-1352, -1376; CVE-2017-7475, CVE-2018-18064; CVE-2026-40228) |

Ubuntu hasn't fixed these yet, so the DEB users are in the same position. A
rebuild only helps once Ubuntu publishes a fix, which is #420's monitoring
job. The tracker API was queried per package with `version=noble` and
status `needed`, `deferred` or `pending`. All 282 queries succeeded.

The full list of source packages and versions is in `data/match.json`
(`packages`), and their Sources-index sizes are in `data/ubuntu-source-index.json`.

## Decisions for Patrick

1. **Complete source** for Qt, the LGPL libraries and the runtime: ship about 290 MB of source, add a written offer, mirror by version on pkg.put.gd, or accept the current position ([#416](https://github.com/patrick-hudson/put.gd-desktop/issues/416)).
2. **Qt 6.8.3's CVEs:** bump to 6.11.2/6.12.0 binaries, build 6.8.5 from source, ship with mitigations (drop the `icns`/`tiff`/`svg` image plugins), or accept ([#418](https://github.com/patrick-hudson/put.gd-desktop/issues/418)).
3. **Codec patents:** H.264, H.265 and AAC in a stable download ([#419](https://github.com/patrick-hudson/put.gd-desktop/issues/419)).
4. **Re-release policy** for a stable AppImage when Ubuntu fixes a bundled package ([#420](https://github.com/patrick-hudson/put.gd-desktop/issues/420)).

[#417](https://github.com/patrick-hudson/put.gd-desktop/issues/417) (notices) is
a fix with no decision needed, apart from the source location that #416 settles.
[#421](https://github.com/patrick-hudson/put.gd-desktop/issues/421) is hygiene.

## Repeating it on the stable draft

On a builder with Ubuntu 24.04 apt sources (m74), in a new directory. The
scripts are in `tools/` here and need only the standard library, plus
`python3-cryptography` for the minisign check.

```sh
TAG=vYY.MDD.N; V=${TAG#v}; B=https://github.com/patrick-hudson/put.gd-desktop/releases/download/$TAG
mkdir dl && cd dl
for f in SHA256SUMS release-builds.json putgd-$V-x86_64.AppImage putgd-$V-x86_64.AppImage.zsync putgd-$V-appimage-media-source.tar.gz putgd-$V-source.tar.gz; do curl -fsSLO $B/$f; done
curl -fsSLO https://pkg.put.gd/releases/$TAG/SHA256SUMS.minisig
sha256sum --check --strict --ignore-missing SHA256SUMS                                     # P1
python3 -I ../tools/minisign_verify.py "$(tail -1 KEY.pub)" SHA256SUMS SHA256SUMS.minisig   # P2 (stable key)
cd .. && python3 -I tools/appimage_probe.py dl/putgd-$V-x86_64.AppImage                     # offset, sha1, .upd_info
python3 -I tools/runtime_cmp.py dl/putgd-$V-x86_64.AppImage runtime-x86_64                  # P4
unsquashfs -q -n -o OFFSET -d root dl/putgd-$V-x86_64.AppImage                              # then P3: inventory vs receipt
python3 -I tools/buildids.py root > image-buildids.json
xargs -a distro-packages.txt apt-get download                                               # in debs/, from the receipt's distroLicensePackages
python3 -I tools/match.py image-buildids.json debs > match.json                             # D1
python3 -I tools/sizes.py root match.json sizes > sizes.json                                # C4
python3 -I tools/ubuntu_cves.py ubuntu-open-cves.json SOURCE...                             # D2 (slow API, about 1 h)
```

The P3 inventory comparison, the C1–C3 greps and the N3 media-source hash
check are the inline commands summarised in the table. Turning all of this
into a `tools/releases/` checker is part of #383's "Done looks like".
