# DEB/RPM exact-asset audit rehearsal (#383) — 2026-10-22

This is the DEB/RPM half of the first-stable license, notices, provenance and
dependency audit
([#383](https://github.com/patrick-hudson/put.gd-desktop/issues/383)). The
AppImage half is in [2026-10-22-appimage-license-audit](../2026-10-22-appimage-license-audit/README.md).
It is a pass on nightly bytes, not on release bytes. The release campaign
([#384](https://github.com/patrick-hudson/put.gd-desktop/issues/384)) reruns the
same scripts on the stable draft.

| | |
| --- | --- |
| Release | [`v26.1022.1017-nightly`](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1022.1017-nightly), commit `3582b30af88bb8158c49b92864c6eb3c48038b21` (nightly HEAD, after #424 and #428), tree `e5265b28`, count 1017 |
| Built by | [Nightly run 39864143584](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39864143584), `workflow_dispatch` on `nightly`, 22:19:51–22:47:26Z, attempt 1, every job green |
| Audited on | f44 (Fedora 44): download, extraction, receipts, repos and the RPM's identity. Lint ran in throwaway rootless podman containers (Debian 13, Ubuntu 24.04 and 26.04 lintian; Fedora 44 rpmlint). minisign ran on d69. Each DEB's identity ran on its own distribution: m74 (Mint 22.3, noble), k72 (Ubuntu 26.04.1), d69 (Debian 13). Nothing was installed and nothing ran on the workstation. |
| Audited at | 2026-10-22 22:50–23:40Z. The repos were checked at 22:51Z `InRelease`. A rehearsal on `v26.1022.996-nightly` (20:40Z) found the same results. |

| Asset | SHA-256 | Bytes |
| --- | --- | ---: |
| `putgd-26.1022.1017-nightly-ubuntu24.04-x86_64.deb` | `f28ab4aebae03d501f007dcfe20a05df6ec701c32e1127b91fd18dce4c0e15a5` | 54,187,906 |
| `putgd-26.1022.1017-nightly-ubuntu26.04-x86_64.deb` | `6f89cd7b025fcc87a7a37459a3f227b490cb88b9282b55c921c88842f996464a` | 54,529,896 |
| `putgd-26.1022.1017-nightly-debian13-x86_64.deb` | `1241c317f153ab1b986441cd27f4700faed05490643ef534be968498507080c9` | 54,069,902 |
| `putgd-26.1022.1017-nightly-fedora44-x86_64.rpm` | `4f5a5c6355b1f138abf11ed1ded25213f5001996e61fd5d39f6882cbb1fb0a42` (repo copy, signed: `f51fcb12f723a70cf2e617fb1632b6163cfb871611b9a0a5d146adf012f8cf2a`) | 40,528,036 |

| File | What it holds |
| --- | --- |
| `results.json` | The result table below, machine-readable, with the asset identities |
| `data/packages.json` | Per package: receipt cross-checks, inventory match, RUNPATHs, debug sections, build paths, docs, control files |
| `data/elf.json` | Each ELF's NEEDED, RUNPATH/RPATH, SONAME, debug sections and GNU build ID |
| `data/repo.json` | The apt suites and the dnf repository: signatures with each key, `Packages`/`primary` entries, pool and RPM digests, DEP-11 and dnf AppStream catalogs |
| `data/identity.txt` | `putgd`/`putgd-cli --version-json` and `build-info.json` from each package on its own distribution |
| `data/minisign.txt` | `SHA256SUMS.minisig` verification |
| `data/lint/`, `data/lint-tags.json` | lintian, rpmlint, `appstreamcli validate` and `desktop-file-validate` output and tag counts |
| `data/pr468-lint/` | The same lint on [#468](https://github.com/patrick-hudson/put.gd-desktop/pull/468)'s CI packages (`26.1022.1019~dev+g6aa43c50`) |
| `data/fedora44-rpm-header.txt`, `data/fedora44-rpm-files.txt` | RPM header, scriptlets, Requires/Recommends, and per-file flags |
| `data/npm-audit.json`, `data/osv-npm.json` | Known-vulnerability lookups for the Proton helper's npm lock |
| `tools/` | The scripts: `fetch.sh`, `extract.sh`, `inspect_tree.py`, `repo.py`, `lint.sh`, `identity.sh`, `summarize.py`, `osv_npm.py` |

## Result

✅ pass · ⚠️ pass with a gap (issue linked) · ❌ fail · 📋 decision for Patrick

| # | Item | ubuntu24.04 | ubuntu26.04 | debian13 | fedora44 | Evidence / gap |
| --- | --- | :-: | :-: | :-: | :-: | --- |
| P1 | `SHA256SUMS` lists the package and it verifies | ✅ | ✅ | ✅ | ✅ | `sha256sum --check --strict --ignore-missing`. The `pkg.put.gd/releases/<tag>/SHA256SUMS` copy is byte-identical to GitHub's. |
| P2 | `SHA256SUMS.minisig` verifies with the pinned nightly key | ✅ | ✅ | ✅ | ✅ | Key `71440FA5E439AEBB`. minisign 0.12 and the independent Ed25519/BLAKE2b verifier from the AppImage audit both pass; trusted comment `put.gd v26.1022.1017-nightly nightly`. |
| P3 | The package matches its CI run and `release-builds.json` | ✅ | ✅ | ✅ | ✅ | Tag → `3582b30a` = the run's `head_sha`. Each receipt's `package.sha256` and size match the asset. The receipt `manifest` equals the packaged `build-info.json`, and `putgd`/`putgd-cli --version-json` on the package's own distribution print the same identity (`dirty: false`). Every receipt `inventory` entry matches the extracted tree byte for byte, with nothing unlisted: 105/105 per DEB, 107/107 for the RPM. All 9 put.gd ELF build IDs are in the `debugSymbols` receipt. |
| P4 | The repo copy matches the release asset | ✅ | ✅ | ✅ | ✅ | apt: `Packages` lists `26.1022.1017~nightly` only, and the pool DEB is byte-identical to the asset. dnf: `primary` lists 1017, 996 and 983. The repo RPM is the asset plus the signer's signature: equal `SHA256HEADER` `3d817b96…` and payload SHA-256 `3a1138ba…`. |
| P5 | Repo signatures verify against the keys the packages ship | ✅ | ✅ | ✅ | ✅ | `InRelease` and `Release.gpg` verify with `gpgv` against `/usr/share/keyrings/putgd-archive-keyring.gpg`, against the key inline in `putgd.sources` `Signed-By`, and against `packaging/linux/repo/keys/putgd-nightly.gpg`. All three are `4032E28D…5536B371`. The stable key `C0962EFA…` fails, as it should (negative control). The repo RPM verifies (`rpmkeys -K`, RSA/SHA512) with `RPM-GPG-KEY-putgd-nightly` and `RPM-GPG-KEY-putgd`, and `NOKEY` with the stable key. dnf repodata is unsigned by design (`repo_gpgcheck=0`, decided in #293 for dnf5daemon). |
| P6 | AppStream consistent with the packages | ✅ | ✅ | ✅ | ✅ | `appstreamcli validate --pedantic` passes; the only note is `cid-contains-uppercase-letter`. The DEP-11 catalog of each suite and the dnf `appstream` repodata have the same id, `Package`/`pkgname` `putgd`, licence and release version as the installed metainfo. The catalogs and icon tarballs are covered by `InRelease` or `repomd.xml` checksums. A nightly's release reads as `stable` → [#471](https://github.com/patrick-hudson/put.gd-desktop/issues/471). |
| N1 | Licence texts and notices in `/usr/share/doc/putgd` | ⚠️ | ⚠️ | ⚠️ | ⚠️ | Present: put.gd's `LICENSE` (MIT), the native `runtime-notices.txt` (the right text for native packages, per #417), 14 AWS SDK/CRT/s2n `LICENSE` files, three Wayland protocol XMLs, and `proton/` (helper licences, notices, sources catalog). **Gap:** the SDK's and CRT's Apache-2.0 `NOTICE` files and the notices of cJSON (MIT), TinyXML-2 (Zlib), libcbor (MIT) and xxHash (BSD-2-Clause), which are compiled into `libaws-cpp-sdk-core`, `libaws-c-common` and `libaws-checksums`, weren't shipped. The AppImage has the same gap. Fixed in [#468](https://github.com/patrick-hudson/put.gd-desktop/pull/468). |
| N2 | Debian `copyright` (DEP-5) / RPM `%license` | ❌ | ❌ | ❌ | ❌ | No `/usr/share/doc/putgd/copyright` and no `changelog` (lintian errors). Minimised Ubuntu images keep only `copyright` from `/usr/share/doc`, so they drop every put.gd notice. The RPM marks all 33 doc files `%doc`, none `%license`, so `--nodocs`/`tsflags=nodocs` drops them → [#470](https://github.com/patrick-hudson/put.gd-desktop/issues/470). |
| N3 | Declared licence (RPM `License:`, AppStream) | 📋→✅ | 📋→✅ | 📋→✅ | 📋→✅ | `MIT AND Apache-2.0 AND GPL-3.0-only` omits code the packages ship: OpenPGP.js (LGPL-3.0+), Bun's JavaScriptCore/TinyCC (LGPL-2.x), bcryptjs (BSD-3-Clause), the zkde-screencast bindings in `putgd-record` (LGPL-2.1-or-later), and cJSON/libcbor/TinyXML-2/xxHash. The RPM spelled `GPL-3.0`, a deprecated SPDX id; #468 makes it `GPL-3.0-or-later`, which upstream allows → [#469](https://github.com/patrick-hudson/put.gd-desktop/issues/469). Decided: main licences plus a pointer to `runtime-notices.txt`, done in #468 (see Decisions). |
| N4 | Proton helper notices, #369 consistency | ✅ | ✅ | ✅ | ✅ | `proton/THIRD-PARTY-NOTICES.txt` is `df283c96…`, the same as the committed file and the `usr/libexec` copy. It lists 26 packages: 10 `bundled-js` (all with licence text), 1 `embedded-runtime` (Bun, covered by `BUN-THIRD-PARTY-NOTICES.txt.gz`), 9 `installed-not-bundled` and 6 `build-only`. `@openpgp/web-stream-tools` 0.3.1 and 0.1.3 are both there. The release jobs use npm 9.2.0 (Ubuntu/Debian `npm` package) and 10.9.8 (Fedora `nodejs22-npm`), and all install 26 packages. The pin that #369 asks for is still open. |
| N5 | Corresponding source for the GPL/LGPL parts | 📋→✅ | 📋→✅ | 📋→✅ | 📋→✅ | Distro libraries (Qt, GStreamer, FFmpeg) come through Depends/Requires. The helper's GPL-3.0 crypto and LGPL OpenPGP.js/JSC source is on the GitHub release (`putgd-proton-corresponding-source.tar.gz.part1/2`, catalog `6d5d4798…`). The shipped `SOURCES.json` gives file names and hashes only and says `"distributionReady": false`, and apt/dnf users download from pkg.put.gd → [#416](https://github.com/patrick-hudson/put.gd-desktop/issues/416#issuecomment-6352661019). Decided: #468 puts the release page in `runtime-notices.txt` and `proton/SOURCE-LOCATION.txt`. |
| N6 | Every bundled component has its licence | ⚠️ | ⚠️ | ⚠️ | ⚠️ | Bundled are put.gd (MIT), the AWS SDK 1.11.907/CRT/s2n libraries (Apache-2.0, plus the embedded code in N1) and the Proton helper (N4). There's no Qt, compiler runtime, codec or aws-lc: `libcrypto.so.3`, `libcurl.so.4` and `libz.so.1` come from the distro. Same gap as N1, fixed by #468. |
| C1 | No stray debug or test files | ⚠️ | ⚠️ | ⚠️ | ⚠️ | No `.a`, `.la`, `.o`, `.h`, `.pc`, `.cmake`, `.pyc`, test directories or fixtures, and none of the release audit's forbidden names. All 25 ELF files keep `.symtab` (lintian `unstripped-binary-or-object`). The Fedora `libs2n.so.1.0.0` also carries full DWARF → [#471](https://github.com/patrick-hudson/put.gd-desktop/issues/471). |
| C2 | No secrets | ✅ | ✅ | ✅ | ✅ | The AWS/GitHub/Slack/OpenAI/Google key patterns, PEM private-key headers and `aws_secret_access_key`: no hits in any file. No key or certificate files apart from the public repo keys, and no `/home/patrick` paths. |
| C3 | No absolute build paths, sane RUNPATHs | ⚠️ | ⚠️ | ⚠️ | ⚠️ | The only RUNPATH/RPATH is `$ORIGIN`: RUNPATH on the 15 private AWS libraries, DT_RPATH on `libputgd-s3.so` on purpose. Programs have none. 18 ELF files contain `/__w/put.gd-desktop/…` strings (s2n asserts, CRT `__FILE__`, the guarded build-tree lookup in `putgd`, `putgd-ui` and `putgd-s3-worker`) → #471. |
| C3b | Hardening of put.gd's programs | ✅ | ✅ | ✅ | ❌ | DEB programs are PIE (`DYN`). All eight Fedora programs are non-PIE (`EXEC`), with partial RELRO only → [#475](https://github.com/patrick-hudson/put.gd-desktop/issues/475). |
| C4 | Control/spec metadata | ⚠️ | ⚠️ | ⚠️ | ⚠️ | Depends/Requires name only distro packages, and shlibdeps/RPM's automatic requires match `elfDependencies`. The DEB's one conffile is `/etc/apt/sources.list.d/putgd.sources`, with no maintainer scripts; dpkg handles purge. RPM: `%ghost %config(noreplace)` on `/etc/yum.repos.d/putgd.repo`, `%post` copies it in on the first install only, and `%postun` removes it on erase. **CPack defaults leaked:** Vendor `Humanity`, the RPM description `This is an installer created using CPack…`, an empty DEB extended description, Section `devel`, a Maintainer without an address, and the RPM owning `/usr/libexec`, `/usr/share/applications`, `/usr/share/metainfo` and the hicolor tree. All fixed in #468. |
| C5 | Both channels' repo files (#428) | ✅ | ✅ | ✅ | ✅ | DEB: `/usr/share/putgd/repo/putgd-{nightly,stable}.sources` (suites `nightly`/`stable`) beside the installed nightly `putgd.sources`. RPM: `/usr/share/putgd/repo/putgd{,-nightly,-stable}.repo`, each naming its own key, and `/etc/pki/rpm-gpg/RPM-GPG-KEY-putgd{,-nightly,-stable}`. The fingerprints match `docs/package-repositories.md`. |
| L1 | lintian / rpmlint triaged | ⚠️ | ⚠️ | ⚠️ | ⚠️ | See the triage table below. #468 fixes `malformed-contact`, `extended-description-is-empty`, `invalid-license`, `description-line-too-long` and `standard-dir-owned-by-package` (checked on its CI packages, `data/pr468-lint`). #470 tracks `no-copyright-file` and `no-changelog`, #471 the rest. |
| D1 | Bundled pieces with known CVEs | ✅ | ✅ | ✅ | ✅ | Proton helper: `npm audit --package-lock-only` and OSV return 0 advisories for all 36 lock entries. Bun 1.4.2 has no published GHSA (1.4.4 is current). AWS SDK 1.11.907: as in the AppImage audit, the s2n and aws-c-http pins are past their fixed CVEs. Distro libraries are the distribution's job. |
| D2 | Codecs (for #419) | ✅ | ✅ | ✅ | ✅ | No codec is bundled. `putgd-record` links the distro's GStreamer and libav* and picks an encoder at run time. DEB: Depends `gstreamer1.0-plugins-bad` (`openh264enc`, `voaacenc`); Recommends `-ugly` (x264), `-libav` and `-vaapi`. RPM: Requires `gstreamer1-plugins-bad-free`; Recommends `gstreamer1-plugin-openh264` and `openh264` (Cisco's build from `fedora-cisco-openh264`) → [#419 comment](https://github.com/patrick-hudson/put.gd-desktop/issues/419#issuecomment-6352660745). Report only. |

Not covered: Qt and the other distro libraries' notices and CVEs, which come
through Depends/Requires and are the distribution's job; the Proton per-release
compatibility check and the concatenation of the 2.48 GB Proton source parts
(#383 items 3 and 5, which the AppImage audit also left to CI's `protonSource`
receipt); and an install, upgrade or purge on a live system, which
[#293](https://github.com/patrick-hudson/put.gd-desktop/issues/293)/[#386](https://github.com/patrick-hudson/put.gd-desktop/issues/386)
acceptance covers.

## lintian and rpmlint triage

lintian 2.117.0ubuntu1.5 (noble), 2.129.0ubuntu2.1 (26.04) and 2.122.0 (trixie), with
`--pedantic --display-info --display-experimental`. rpmlint 2.8.0 (Fedora 44)
reported 5 errors and 68 warnings. The three DEBs got the same tags; the counts
are per package.

| Tag | Count | Verdict |
| --- | ---: | --- |
| E `no-copyright-file`, E `no-changelog` | 1, 1 | Real → #470 |
| E `malformed-contact`, E `extended-description-is-empty` | 1, 1 | Real, fixed in #468 |
| E `unstripped-binary-or-object` (lintian), W same (rpmlint) | 25, 25 | Real, minor (`.symtab` kept; debug info is split by CI) → #471 |
| E `embedded-library libjpeg` in `putgd-proton-runtime` | 1 | Upstream Bun binary; information only (#471) |
| E `package-installs-apt-sources` | 1 | By design: the tagged DEB adds its signed channel (#293, `docs/package-repositories.md`) |
| W `hardening-no-pie`, W `hardening-no-relro` (`putgd-proton-runtime`) | 1, 1 | Bun's upstream binary; information only (#471) |
| rpmlint W `position-independent-executable-suggested` | 9 | **Real:** all eight Fedora programs are `EXEC` (non-PIE), while the DEB builds are `DYN`, because Ubuntu/Debian GCC defaults to `-pie` and the CMake build doesn't ask for it → [#475](https://github.com/patrick-hudson/put.gd-desktop/issues/475) |
| W `no-manual-page` / `no-manual-page-for-binary` | 9 | Minor → #471 |
| I `hardening-no-bindnow`, `hardening-no-fortify-functions` | 16–24, 1–17 | Information only (the SDK's own flags) |
| I `extra-license-file`, `package-contains-documentation-outside-usr-share-doc` | 10, 5 | Intended: the helper's audit needs its notices beside it in `/usr/libexec/putgd/proton` |
| I `desktop-entry-lacks-keywords-entry`; X `spelling-error-in-binary`, `duplicate-files` | 1; 35–37, 3 | Cosmetic. The duplicates are identical Apache-2.0 texts |
| P `openpgp-file-has-implementation-specific-extension` (`.gpg` keyring) | 1 | apt's documented `Signed-By` keyring name; keep |
| rpmlint E `description-line-too-long`, W `invalid-license GPL-3.0`, E `standard-dir-owned-by-package /usr/share/icons` | 1, 1, 1 | Real, fixed in #468 |
| rpmlint E `explicit-lib-dependency glib2`, `libsecret` | 2 | Intentional (the `gio` tool and the dlopen'd libsecret) → #471 |
| rpmlint E `files-duplicated-waste`, W `files-duplicate` | 1, 6 | Identical licence texts and the autostart copy of `putgd.desktop` → #471 |
| rpmlint W `conffile-without-noreplace-flag` (the three `/etc/pki/rpm-gpg` keys) | 3 | Should be plain files so a key rotation replaces them → #471 |
| rpmlint W `devel-file-in-non-devel-package` (unversioned `libaws-*.so`) | 12 | The private SDK loads them by these names; keep |
| rpmlint W `dangerous-command-in-%post install`, `-%postun rm` | 1, 1 | The reviewed repo-file scriptlets; keep |
| rpmlint W `non-coherent-filename` | 1 | Release asset naming; keep |
| `desktop-file-validate` hint: two main categories in `putgd.desktop` | 1 | Cosmetic |

On #468's CI packages, lintian is left with `no-copyright-file`, `no-changelog`,
`unstripped-binary-or-object` and `embedded-library` (errors). rpmlint is left with
3 errors (`explicit-lib-dependency` ×2, `files-duplicated-waste`) and no
`invalid-license`, description or standard-directory findings. Those packages
are untagged dev builds, so they carry no repo file and no
`package-installs-apt-sources`.

## Dependency inventory (dated 2026-10-22)

Bundled in the native packages:

| Component | Version | Known CVEs | Action |
| --- | --- | --- | --- |
| AWS SDK for C++ (with aws-crt-cpp, aws-c-*, s2n-tls) | 1.11.907 `a9f0ffcc`; s2n `853d1943`, aws-c-http `8aefd899` = v0.11.0 | s2n CVE-2026-16317/-16318 and aws-c-http CVE-2026-12043 are fixed in the pins. Upstream is at 1.11.915. | Recheck on release day |
| cJSON 1.7.19, libcbor 0.13.0, TinyXML-2, xxHash | as vendored by the SDK pin | none found for these versions | Follow the SDK |
| Proton helper `putgd-proton-runtime` `e93b3b52…` | Drive SDK `js@0.22.2`, crypto 2.0.1 (`956a713c`), `@protontech/openpgp` 6.3.1, Bun 1.4.2 | `npm audit` and OSV: 0 for all 36 lock entries; no Bun GHSA | Bun 1.4.4 is current; bump with the next helper rebuild. The dated Proton check is #383 item 5. |

Distro libraries (Qt 6, QtKeychain, LayerShellQt, GStreamer, FFmpeg, OpenSSL,
curl, …) come from Depends/Requires and get the distribution's security updates.
Nothing here needs a put.gd release.

## Issues and PRs

| | What | Labels |
| --- | --- | --- |
| [#468](https://github.com/patrick-hudson/put.gd-desktop/pull/468) | Code PR: AWS NOTICE and embedded-code notices (native and AppImage), RPM licence id, CPack metadata, RPM directory ownership | — |
| [#469](https://github.com/patrick-hudson/put.gd-desktop/issues/469) | The declared licence omits LGPL, BSD and Zlib code the packages ship. Decided: main licences plus the notices table (fixed by #468) | P1, release-blocker |
| [#470](https://github.com/patrick-hudson/put.gd-desktop/issues/470) | No Debian `copyright`/`changelog`; RPM licence texts are `%doc`, not `%license` | P2 |
| [#471](https://github.com/patrick-hudson/put.gd-desktop/issues/471) | Hygiene: symbol tables, Fedora libs2n DWARF, CI paths, smaller lint findings | P3 |
| [#475](https://github.com/patrick-hudson/put.gd-desktop/issues/475) | The Fedora RPM's programs are non-PIE | P2 |
| [#416](https://github.com/patrick-hudson/put.gd-desktop/issues/416#issuecomment-6352661019), [#419](https://github.com/patrick-hudson/put.gd-desktop/issues/419#issuecomment-6352660745) | The native side of the source-offer and codec decisions | needs-patrick |

## Decisions

Patrick decided on 2026-10-22 (relayed by the coordinator):

> #469: name the main licences and point to the full notices; Proton crypto package as GPL-3.0-or-later if its upstream allows, else -only. Proton source: add a URL in the notices to the GitHub release source. Maintainer: packages@put.gd is fine.

Implemented in [#468](https://github.com/patrick-hudson/put.gd-desktop/pull/468), commit `7c3e7aba`:

1. **Licence expression** ([#469](https://github.com/patrick-hudson/put.gd-desktop/issues/469#issuecomment-6353477447)).
   - The RPM `License:` and AppStream are `MIT AND Apache-2.0 AND GPL-3.0-or-later`.
   - The DEB/RPM description points to `runtime-notices.txt`, which opens with a table of every licence of the shipped code and where its text is.
   - The AppImage's `components.json` names the same main licences per component and lists the rest as embedded.
2. **Proton crypto is GPL-3.0-or-later.** Upstream allows it: WebPackages' README at `956a713c` grants "either version 3 of the License, or (at your option) any later version". Its `LICENSE` is the GPLv3 text pinned in `runtime-lock.json` (`1b3782cc…`), and the crypto sources have no narrower header.
3. **Proton source** ([#416](https://github.com/patrick-hudson/put.gd-desktop/issues/416#issuecomment-6353477700)).
   - `runtime-notices.txt` and a new `proton/SOURCE-LOCATION.txt` name the exact release page with the source parts, the catalog and put.gd's source.
   - `SOURCES.json` stays as is: it's hash-pinned in the helper manifest and the published catalog.
4. **Codecs:** no change for DEB/RPM ([#419](https://github.com/patrick-hudson/put.gd-desktop/issues/419#issuecomment-6352660745)).
5. **Maintainer:** `packages@put.gd`.

The AppImage decisions in #416 (Qt/LGPL source), #418 and #419 are separate and still open.

## Rerun

On a builder with podman (Fedora 44 here) and the stock distro images:

```sh
bash tools/fetch.sh TAG DIR                  # GitHub assets + pkg.put.gd SHA256SUMS(.minisig), checksums
bash tools/extract.sh TAG DIR packaging/linux/repo/keys
python3 -I tools/inspect_tree.py DIR         # receipts, ELF, stray/secret/build-path scan
python3 -I tools/repo.py TAG DIR DIR/keys    # apt/dnf copies, InRelease/Release.gpg/RPM signatures, AppStream
bash tools/lint.sh TAG DIR                   # lintian (each DEB's own release), rpmlint, appstreamcli
python3 -I tools/summarize.py DIR OUT
bash tools/identity.sh PACKAGE               # on a host of that package's distribution
minisign -Vm SHA256SUMS -x SHA256SUMS.minisig -p packaging/linux/repo/keys/putgd-nightly.minisign.pub
```

`lint.sh` expects images `localhost/putgd-audit-lintian-{debian-trixie,ubuntu-24.04,ubuntu-26.04}`
(lintian and appstream on the stock image) and `localhost/putgd-audit-rpmlint`
(`rpmlint rpm-build appstream desktop-file-utils` on `fedora:44`). Run `repo.py`
while the tag is still the newest in the apt suites, because apt keeps only the
newest version.
