# Native packaging notices, metadata and hardening (#540)

[Code PR #540](https://github.com/patrick-hudson/put.gd-desktop/pull/540) addresses
#369, #461, #470 and #475. This record covers package content, ELF properties and
isolated installer behavior. It is not graphical, memory, fresh-agent or stable
release acceptance.

## Exact development build

Both physical builders tested clean source commit
`3993e8d49d0a83b7593e78c77d8701528cd2f878`, tree
`82db82a93bb1809c50bd4e61ed644508cb5f2c88`, version
`26.1024.1150-dev+g3993e8d49d0a`. The commit was transferred in a private Git bundle
and tested before the branch update was pushed. Neither source identity nor
historic receipts have been relabeled with this documentation commit.

- Fedora 44: `patrick@192.168.68.70`, hostname `fedora`.
  RPM SHA-256: `6937ba121fe8dbce729e7a1422abda7f844486a842183df5ae728335e34ad665`.
- Debian 13: `patrick@192.168.68.69`, hostname `debian`.
  DEB SHA-256: `86c6a36b94527dd5e6256d10001b0236131cdc9bc9ad3b8b1748a65f5d70e09d`.

The two `native-audit.json` files retain software/module identities, package
metadata, dependency resolution and the complete file/mode/digest inventories.
The builds and package bytes remain under
`~/work/pgpkg-20261024/build/package-validated-candidate/` on each builder.
Configure/build logs and the exact contract reports remain directly usable under
`~/work/pgpkg-20261024/work/validated-candidate/`. SDK and Qt remain dynamically
linked as recorded in each native audit; native Qt is supplied by the distro.

## Results

All 35 release/packaging/CI contract suites passed independently on each builder
(`f44/contracts.json`, `d69/contracts.json`). Build identity and bundled Proton
CTest contracts passed on both (`ctest.log`). Preset builds targeted all product
programs plus those contract executables, with `CMAKE_BUILD_TYPE=Release`,
`PUTGD_BUNDLE_PROTON=ON`, `PUTGD_CRASH_TRIGGER=OFF` and the host's package target.

The native artifact audits passed. All eight product ELF executables must have
`Type: DYN`, `GNU_RELRO` and immediate binding (`BIND_NOW`/`NOW`); the audit rejects
non-PIE or partial RELRO. `putgd-launcher` is a shell script. The upstream Bun
executable is outside this compiler change and remains tracked in #471.

Debian's official `python3-debian` strict copyright parser accepted the full
DEP-5 file. An actual isolated `dpkg` install used
`--path-exclude=/usr/share/doc/*` and
`--path-include=/usr/share/doc/*/copyright`. The complete 24,070,583-byte copyright
file survived, byte for byte; the ordinary changelog was excluded.
`d69/minimal-install.json` records the result and package/source identities.
The retained file conservatively reproduces the installed Bun notice/provenance
corpus as well as the other licence texts; its size is intentional and is not a
claim that every source notice describes linked code.

The RPM's licence and notice files are `%license`. An actual RPM 6.0.2 install
with `--excludedocs` retained all 54 notice files, byte for byte, while excluding
`changelog.gz`. This installer check ran in a disposable Fedora 44 container on
the physical Fedora builder, with scripts/triggers and dependency resolution
excluded from this file-retention check. Native identity/ELF/dependency audits
ran directly on the host. `f44/nodocs.json` and `nodocs.log` retain the result.
The container invocation used `--rm`; no VM or golden image was used or changed.

Before the metadata changes, Linux helper provisioning on Fedora with npm 9.2.0,
10.9.8 and 11.16.0 produced the same helper SHA-256
`e93b3b520d20a19e37569c7a97a1680f47bc43fcb5acaf6a5d69fa660e0a34b0`
and notice SHA-256
`df283c96217224794ea11dc95e89dcc0a66f7f805be4afdcbde944d90eb392a2`.
All 13 redistribution files were bound by the manifest, including ICU 78.3's
exact notice in both installed copies. The direct dependency pin changes the
installed inventory guarantee without changing the helper or notice bytes.
The original provisioning logs and `npm-matrix.json` remain at
`~/work/putgd-packaging-20261024/` on Fedora.

## Original failures and correction

The repeated CI contract failure on earlier #540 heads was deterministic:
`tests/ci/test_flake_tools.py` requires `include(cmake/TestPolicy.cmake)` to be the
last line. The added metadata include followed it. The final correction moves
metadata registration before that existing policy; the test is unchanged.
`contracts-failure-ce9.txt` preserves the failure on `ce9bbcb8`.

Stricter private artifact validation also found that whitespace-only lines in
the large Bun notice corpus became empty DEP-5 paragraphs. The strict parser
reported `Syntax or Parse error on or near line 270332`. The generator now emits
the required dot continuation for all blank/whitespace-only lines. The corrected
whole-file parse and minimized install passed before the final push. The earlier
`ce9bbcb8` packages remain under `build/package-clean/` on both builders.

The first physical-host RPM installer harness failed during `rpm --root ...
--initdb`, before reading the package, with an `EACCES` transaction-lock error.
`f44/host-installroot-failure.log` preserves it. This is not an installer payload
result. The normal RPM installation in the isolated Fedora 44 container provided
the successful `--excludedocs` result above; no host security policy was disabled.
Additional diagnostics remain in the host's private evidence directory.

## GitHub CI and remaining gates

[CI run 40052002741](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40052002741)
passed the required `CI` summary for PR head `3993e8d4`. Linux 24.04/26.04,
ASan/UBSan, Windows portability, all four native package jobs, AppImage, debug
symbols, SBOM and PipeWire checks passed. Release collection and cache pruning
were intentionally skipped; those skipped jobs are not counted as passed tests.
`ci-run.json` retains the run identity. This CI result is separate from the two
private-host package-byte receipts above.

No PR merge or stable publication was performed by this work. A stable draft
still needs its own source/asset identities, current vulnerability scan,
installation/resource checks and the required native Cinnamon/GNOME/KDE evidence.
The complete AppImage source task (#416/#541), deeper #511 assessment and #419
codec distribution record are separate work; this record does not close them.

## Reproduction

Start from the source commit above on the matching builder, provision the pinned
SDK and helper through the normal build contract, and run:

```sh
python3 tools/ci/contracts.py release --output work/qa/contracts
cmake --preset dev -DCMAKE_BUILD_TYPE=Release -DPUTGD_BUNDLE_PROTON=ON \
  -DPUTGD_CRASH_TRIGGER=OFF -DPUTGD_PACKAGE_TARGET=debian13
cmake --build --preset dev --parallel 4 --target putgd putgd-cli putgd-ui \
  putgd-capture putgd-s3-worker putgd-b2-worker putgd-kde-region putgd-record \
  test_build_contract test_proton
ctest --preset dev -R '^(build|proton)_contract$'
python3 tools/releases/native.py --build build/dev --format DEB \
  --output "$PWD/build/package-qa" --require-proton \
  --proton-manifest build/dependencies/proton-runtime/manifest.json \
  --package-target debian13
```

Use `fedora44`, `RPM` and the provisioned SDK's `lib64` directory on Fedora.
Output directories must be new and remain in ignored `build/` or `work/` paths
so the source identity stays clean. The original installer commands and logs are
preserved beside the corresponding result JSON; these tests install only into
owned test roots or disposable containers and do not replace the desktop's app.
