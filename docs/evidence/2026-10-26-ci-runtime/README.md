# CI runtime and installer download changes

Code: [PR #593](https://github.com/patrick-hudson/put.gd-desktop/pull/593),
commit `de87c6627af2397f2d05901f519d04026e362acb`, based on nightly
`d7e806fd1849157eed4b1d53b7dc698ed4522bac`. The user requested faster CI/nightlies
and installers as the primary downloads, and approved retaining essential support
metadata with separately linked corresponding sources.

## Observed baseline

[Nightly run 40275032192](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40275032192)
ran from 2026-10-26 05:35:22 UTC to 06:29:55 UTC (54m33s), at the baseline commit.

| Job | Elapsed minutes |
| --- | ---: |
| AppImage | 23.75 |
| Ubuntu 24.04 package, including source recompilation | 13.05 |
| SBOM | 2.10 |
| Collection | 7.88 |
| PipeWire, after collection | 4.12 |
| Publication | 15.78 |

The source build/test jobs took 4.5–5.6 minutes alongside packaging; they were
not this run's critical path. The preceding
[push CI run](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40268485275)
took 29m12s, including a 20.87-minute AppImage job and 5.3-minute debug aggregation.
These are individual observations, not an average or maximum.

`v26.1026.1388-nightly` published about 4.8 GB across 21 uploaded assets. The five
installers total about 335 MB. The duplicate combined ZIP is 1.02 GB, debug
archive 269 MB, Proton sources 2.48 GB, and AppImage source archive 674 MB.
The new main-release inventory is approximately 337 MB at those baseline sizes;
the source archive moves to a linked release and remains a publication gate.
No existing assets or tags were changed.

## Validation performed

Tests ran over pinned SSH on Debian 13, `patrick@192.168.68.69`, under
`/home/patrick/work/ci-fast-20261026`. Before testing, load was 0.00 and about
29 GiB of memory and 88 GiB of disk were available. This was command-line tool
validation, not a graphical desktop run or a package build.

- All 38 suites in `python3 tools/ci/contracts.py release` passed.
- Both suites in `python3 tools/ci/contracts.py proton` passed.
- The expanded `tests/ci/test_cache_prune.py` passed separately (11 tests).
- SHA-256-pinned actionlint 1.7.12 passed on all four modified workflows.
- Read-only live validation of permanent Proton source assets passed: public
  status, sizes and GitHub SHA-256 digests match the current catalog.
- The exact-commit reuse lookup returned push run 40268485275, attempt 1, and
  recorded its contracts, Linux, Windows, sanitizer and CI job IDs.
- The download site's actual `tools/releases.py` selector was inspected; source
  tags end in `-files`, so its terminal-nightly matcher excludes them.
- A per-file SHA-256 manifest verified the remote test inputs against the code
  commit. Local evidence is retained privately at
  `/home/patrick/work/put.gd/work/ci-fast-20261026/`; remote reports and the source
  manifest remain under the test directory's `build/`.

New contracts cover installer inventory/source separation, source release
creation and partial-upload resume, conflicting or absent source rejection,
source tag identity, remote asset digests and races, and exact source-CI
reuse/fallback. Existing legacy release-layout tests still pass.

Initial failures were retained. The first transfer omitted Git metadata needed
by the Renovate and source-archive tests; a shallow baseline fetch and index
reset corrected the test environment. Workflow assertions describing the old
aggregate-debug job and sequential PipeWire job were updated to test the new
required jobs. These failures were not treated as passes.

## Remaining verification

The new hosted PR run must validate full package assembly and the new artifact
handoffs. The first tagged run must exercise real source-companion publication,
and a subsequent warm-cache nightly must establish the actual elapsed-time
improvement. No measured speedup is claimed yet. Stable package-specific native
desktop, memory and live-service acceptance remains required by the release
runbook; none was performed or established by these tool contracts.
