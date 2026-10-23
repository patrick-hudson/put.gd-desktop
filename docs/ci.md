# CI coverage and runtime

Five workflows cover every event. Jobs that gate one another run in the same
workflow run and are linked with `needs:`; nothing waits on another workflow
through the Actions API.

| Workflow | Runs on | Does |
| --- | --- | --- |
| `ci.yml` | every pull request, pushes to `nightly`, manual runs; called by nightly and release | contracts, Linux tests, clang ASan/UBSan core tests, Windows compile, packages, the AppImage's pipewiresrc on PipeWire 1.6; on `nightly` pushes, cache pruning |
| `package.yml` | called by `ci.yml` | four native packages, AppImage, their SBOMs and vulnerability scan, and with a tag the collected `release-assets` |
| `nightly.yml` | 05:23 UTC daily and manual | skip unchanged source, or tag, test, package and publish a nightly, then notify put.gd-site; re-scan the latest stable release's SBOMs |
| `release.yml` | pushes to `main` and manual | cut a candidate (or stable on request), test, package and stage a draft |
| `renovate.yml` | 04:17 UTC Mondays and manual; pull requests that change it | open dependency-update pull requests against `nightly`; on pull requests, a read-only lookup ([dependency updates](#dependency-updates)) |

The single required status check is the `CI` job. It fails when any job the
change selected did not succeed. A cancelled or failed job is never a pass; a
job skipped because the change cannot affect it is.

## Job selection

`tools/ci/changes.py` classifies a pull request's changed paths. Pushes, the
schedule, manual runs and release callers always select everything.

| Changed paths | contracts | linux, portable | packages |
| --- | --- | --- | --- |
| `docs/` (except below), `README.md`, `AGENTS.md`, `CLAUDE.md` | yes | no | no |
| Lab, MCP and maintenance Python; `tests/ci`, `tests/releases`, `tests/maintenance`; Python and YAML directly under `tests/desktop` and `tests/performance`; `.github/renovate.json`, `.github/dependabot.yml` | yes | no | no |
| Anything else, including `src/`, C++ tests, schemas and new directories | yes | yes | no |
| `packaging/`, `tools/releases/`, `tools/proton/`, `tools/ci/`, `cmake/`, `integrations/`, `.github/workflows/`, root CMake files, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `.gitignore`, `docs/build.md`, `docs/proton-drive.md` | yes | yes | yes |

Unknown paths select the build by default. The last row lists every input to the
package builders and the curated source archive.

A pull request selects the union of its rows, so a documentation edit pushed to a
code pull request re-runs that pull request's whole build. Keep documentation in
its own pull request (see `AGENTS.md`): a documentation-only pull request runs
`contracts` alone, unless it touches `docs/build.md` or `docs/proton-drive.md`.

The **sanitizers** job is selected separately: by `src/core/`, `src/app/`, the C++
sources under `tests/unit/`, `tests/integration/` and `tests/fixtures/`, and by
`cmake/`, `tools/ci/`, `CMakeLists.txt`, `CMakePresets.json` and `ci.yml`. UI,
platform, storage and packaging changes don't select it.

The **pipewire** job is selected by every packaging row above, by `src/record/` and
by `tests/desktop/pipewire/`. That directory's scripts are read by this job alone,
so a change to them doesn't select `linux` and `portable`.
`tests/ci/test_changes.py` pins these rules.

## Jobs

**contracts** runs on a plain `ubuntu-24.04` runner in about a minute. It runs
every hosted Python suite once through `tools/ci/contracts.py`:

- `release`: `tests/ci`, `tests/releases` and the Python suites in `tests/unit`.
- `proton`: the helper source and redistribution suites in `tools/proton`.
- `lab`: every `tests/desktop/test_*.py`, `tests/maintenance` and the UI-process
  benchmark contract. QEMU disk utilities are installed so overlay tests run.
- `observer`: the cold-family observer suite inside an owned, 45-second subtree.
- `evidence`: the saved source114 live-S3 numeric replay, which is historical
  validation rather than a fresh cloud or desktop run.

Each group writes a JSON report and log to the `contracts` artifact. The job
also runs `actionlint` 1.7.12, pinned by SHA-256, on every workflow.

**linux** builds the Debug `dev` preset with the bundled Proton helper on Ubuntu
24.04 (minimum distro Qt API) and Ubuntu 26.04 (LayerShellQt). It runs the full
registered CTest suite as an unprivileged user, with private temporary paths and
saved socket preflight, JUnit, test logs and build identity. Waits and test timeouts scale by 3
and a failed integration test is retried once and reported (see
[flaky tests](#flaky-tests)). Hosted Xvfb and
offscreen contracts do not establish compositor support or physical memory
acceptance. The Release-preset compile happens in the package builders, not here.

**sanitizers** builds the `clang-asan` preset (the `asan` preset's
`-fsanitize=address,undefined`, compiled with clang) in an Ubuntu 26.04 container and
runs the core contracts and short unit tests the `clang-asan` test preset selects:
settings, operations, history, storage locations, payload, archive, upload naming,
recording model, storage, config reload, local IPC and the small parser and
platform-model tests. The test preset makes every UBSan report fatal; any ASan
report already is. Waits and test timeouts scale by 5 there, and the same
[retry policy](#flaky-tests) applies. The job is not in the required `CI` summary yet (issue
[#90](https://github.com/patrick-hudson/put.gd-desktop/issues/90)); add it there once
it has run green on `nightly` for a while.

Distro Qt is not instrumented, so ASan can't check loads made inside it. The bug
[#76](https://github.com/patrick-hudson/put.gd-desktop/pull/76) fixed kept a
`QJsonValueRef` to a destroyed temporary `QJsonObject`, and only exported QtCore
functions read through it. It passed both GCC and clang ASan against distro Qt; both
compilers do poison the temporary, but no instrumented code read it. The job
therefore builds QtCore itself with ASan (`tools/ci/provision-qt-asan.sh`: the
pinned qtbase archive matching the distro version, Core only, the distro's
absolute install paths so the distro plugins still load) and caches it. The
application still compiles against the distro headers; the tests run with
`LD_LIBRARY_PATH` pointing at the instrumented `libQt6Core.so.6`, and distro QtGui,
QtSql, QtTest and the plugins bind to it. When Ubuntu 26.04 moves to another Qt
release the recipe stops with a version mismatch and fails the job; update its
version and SHA-256 together. The cache key names the distro Qt version, the clang
version and the recipe's hash, so any of them changing rebuilds QtCore. The
instrumented QtCore is a CI-only test library: no package, AppImage or release build
uses it, and release builds stay on distro Qt. `json_ref_lifetime_contract` still
rejects the #76 pattern in source.

**portable** compiles the shared production code on Windows with tests disabled,
using the official Qt installation and pinned QtKeychain/AWS SDK builds. The S3
wire parser links Qt Network there because it uses QHostAddress. macOS is not
built in CI.

**packages** calls `package.yml`. It builds and audits the Ubuntu 24.04,
Ubuntu 26.04 and Debian 13 DEBs, the Fedora 44 RPM and the AppImage, and compiles the curated source
export on Ubuntu 24.04. Its `sbom` job writes a CycloneDX SBOM of every package and
scans them with Grype ([SBOMs and vulnerability scans](release.md#sboms-and-vulnerability-scans)).
With a release tag, `collect` verifies and assembles the
public asset set. See [releases](release.md).

**pipewire** checks the AppImage's bundled GStreamer `pipewiresrc` against a PipeWire
1.6 daemon (issue #144). The AppImage bundles Ubuntu 24.04's PipeWire 1.0.5 plugin,
while hosts run their own daemon, and PipeWire 1.5.81 and later refuse a link whose
buffer-count ranges don't overlap. That mismatch broke recording on Plasma 6.6 (#137)
and nothing in CI could see it, since the runners have PipeWire 1.0.5. The job runs
`tests/desktop/pipewire/ci_check.sh` in a pinned `ubuntu:26.04` container. It:

- compiles the branch's own `pipewireSourceDescription()` (`recorder_properties.cpp`)
  to get the properties the recorder sets, such as `min-buffers=1`;
- links the image's `pipewiresrc`, with those properties, to `run_buffers.sh`'s
  KWin-like (2-4 buffers) and Mutter-like sources on a private daemon with no
  session manager, devices or D-Bus, and requires frames at the sink;
- requires a control with `min-buffers=8` to be refused, so a daemon that doesn't
  enforce the rule can't pass silently.

It takes the AppImage from this run's `packages` job when that ran. Otherwise only
`src/record` or the check changed, so it downloads the latest nightly's AppImage and
checks it against that release's `SHA256SUMS`. It takes about two minutes, most of
it installing the container's packages. A branch that drops `min-buffers=1` fails
it with the #137 error, `error alloc buffers: Invalid argument (-22)`. The job is
part of the required `CI` summary when selected.

## Dependency updates

Dependabot reads only manifests it knows. Most of put.gd's pins are versions in
JSON locks and CMake, each with a commit or SHA-256 beside it. Self-hosted
[Renovate](https://docs.renovatebot.com/) watches them (#420): `renovate.yml`
runs it weekly from `.github/renovate.json`, and opens update pull requests
against `nightly`. Nothing merges itself; each update goes through CI and the
usual acceptance like any other change.

| Pins | File | Source of new versions | Pull requests |
| --- | --- | --- | --- |
| Qt binaries for the AppImage and the five modules' licence sources | `packaging/appimage/tools.lock.json` | `qt/*` tags | one group; patch releases open at once, a new minor or major waits for approval |
| LayerShellQt, extra-cmake-modules | `packaging/appimage/tools.lock.json` | `KDE/*` tags | one group |
| QtKeychain (AppImage) | `packaging/appimage/tools.lock.json` | tags | one each |
| FFmpeg (AppImage recorder) | `packaging/appimage/tools.lock.json` | `n*` tags | patch releases at once; a new minor or major waits for approval (its sonames change) |
| linuxdeploy, its Qt plugin, appimagetool, the type2 runtime | `packaging/appimage/tools.lock.json` | GitHub releases | one group |
| Syft, Grype | `packaging/sbom/tools.lock.json` | GitHub releases | one group |
| AWS SDK for C++ | `cmake/dependency-lock.json`, `cmake/Dependencies.cmake` | tags | one group |
| Proton helper: npm packages, Bun, the SDK | `tools/proton/package.json` and its lock, `tools/proton/runtime-lock.json` | npm | one group, and only after approval |
| MCP helper | `tools/mcp/pyproject.toml` | PyPI | one group |

ICU is the build that ships inside Qt's binaries, so it moves with Qt and is not
watched by itself. The flatpak manifest is a research spike and not watched.
Windows pins (the `portable` job's Qt, `tools/ci/provision-portable-deps.sh`)
belong to the Windows rewrite and are not managed here.

**Versions only, then a refresh.** Renovate changes the version fields. Its
post-upgrade task, `tools/maintenance/refresh_pins.py`, then re-derives
everything that goes with them: each tool's download URL and SHA-256 (checked
against upstream's checksum file where one is published), each source pin's tag
commit, and the AWS SDK's commit with all fifteen CRT submodule pins, read from
tree-only fetches. It never runs what it downloads. That leaves one manual step:
the digest of the `zsyncmake` inside appimagetool, which the appimagetool pull
request's body explains; until it is updated the AppImage job fails with
"Bundled zsyncmake digest mismatch". Run the script by hand after editing a
version yourself: `python3 tools/maintenance/refresh_pins.py`. The AppImage job
reads its Qt version from the lock, so a Qt update needs no workflow change.

**Approval.** Updates that need a decision wait on the dependency dashboard, the
issue titled "Dependency updates (Renovate)": Qt and FFmpeg minor and major
releases, and every Proton helper update. A Proton helper update also needs its
notices and corresponding-source catalog rebuilt (docs/proton-drive.md), which
Renovate cannot do. Tick an update on the dashboard and the next run opens it. At
most five update pull requests are open at once.

**Token.** The job uses the workflow's own `GITHUB_TOKEN` with
`contents: write`, `pull-requests: write` and `issues: write`; there is no
personal access token or app. That has three costs:

- The repository must allow it: Settings, Actions, General, "Allow GitHub
  Actions to create and approve pull requests". The `nightly` ruleset requires no
  approving review, so the approval half grants nothing extra.
- Pull requests opened with the workflow token don't start workflows by
  themselves. A maintainer approves each one's workflows ("Approve and run
  workflows" on the pull request) and CI then runs as usual.
- The token cannot change `.github/workflows`, so Renovate leaves the workflows
  alone (`enabledManagers` leaves out `github-actions`). Dependabot keeps their
  action SHAs current instead (below). The container digests, actionlint and
  the Renovate image inside the workflows are not actions, so neither bot
  updates them; they stay manual
  ([#507](https://github.com/patrick-hudson/put.gd-desktop/issues/507)).

Patrick chose this setup on 2026-10-23: the workflow token for Renovate, and
Dependabot for the workflow pins.

**Workflow actions.** `.github/dependabot.yml` has Dependabot update the
`uses: owner/action@<sha> # vX` pins in `.github/workflows` weekly (Mondays
04:47 UTC), against `nightly`, in one grouped pull request labelled
`dependencies` and `area: ci`. `ilammy/msvc-dev-cmd` is Windows-only and left
to the Windows rewrite. Dependabot's pull requests start CI on their own, with a
read-only token; the flaky-test tracker then logs a warning instead of
updating its issues.

**Checking a change to this setup.** A pull request that changes
`.github/renovate.json`, `renovate.yml` or the two maintenance scripts runs the
`lookup` job instead: `renovate-config-validator --strict`, then Renovate's
local platform in `lookup` mode over the pull request's own checkout, read-only
(`contents: read`). Its job summary lists every detected dependency, its current
version and the update Renovate would propose; the full JSON log is the
`renovate-lookup` artifact. `tests/ci/test_renovate_config.py` checks that every
regex manager still matches each pin it manages.

**Running it by hand.** `gh workflow run renovate.yml -f dry-run=full` logs the
branches and pull requests Renovate would create without creating them; with no
input it runs for real. Each run's summary has the same dependency table and
its log is an artifact. The Renovate image is pinned by version and digest in
`renovate.yml`.

## Notifying put.gd-site

The download site ([patrick-hudson/put.gd-site](https://github.com/patrick-hudson/put.gd-site))
builds its links from this repository's releases. Its deploy workflow
(`.github/workflows/deploy.yml` there) runs every six hours, and also on a
`repository_dispatch` of type `release-published`, when it regenerates
`releases.json` within about a minute.

`nightly.yml`'s `publish` job sends that event after `publish.py --nightly` has
published the prerelease. The `Notify put.gd-site` step posts
`repos/patrick-hudson/put.gd-site/dispatches` with the payload `tag` (the nightly
tag) and `prerelease: true`.

- The workflow token cannot dispatch to another repository, so the step uses the
  `SITE_DISPATCH_TOKEN` repository secret: a fine-grained personal access token
  for `patrick-hudson/put.gd-site` only, with Contents: read and write. Store it
  under Settings, Secrets and variables, Actions.
- Without the secret, the step logs that it isn't notifying the site and exits 0.
- A failed dispatch is a warning, not a failure. The release is already public,
  and rerunning `publish` would stop at the already-published check. The site
  catches up on its schedule, or when its deploy workflow is run by hand:
  `gh workflow run deploy.yml -R patrick-hudson/put.gd-site`.

`release.yml` only stages drafts and sends nothing. A candidate or stable release
is published by hand with `publish.py --publish`, which sends the same event with
the operator's own `gh` login (see [releases](release.md#candidate-and-stable)).
`tests/ci/test_release_channels.py` pins the nightly step and forbids every other
`/dispatches` call in the workflows: workflows never start one another.

### Sanitizers job runtime

Measured on the four-vCPU hosted runner on 2026-10-19 (Ubuntu 26.04 container,
clang 21.1.8, distro Qt 6.10.2):

| Run | Caches | Qt build | App build | CTest (23 tests) | Job |
| --- | --- | ---: | ---: | ---: | ---: |
| [39267375812](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39267375812) attempt 1, PR #107 | none | 7:35 | 9:42 | 70.5 s, passed | 23:10 |
| [39267375812](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39267375812) attempt 2, PR #107 | SDK, QtCore | cached | 4:42 | 65.3 s, passed | 7:33 |
| [39266994848](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39266994848) attempt 3, #76 reverted | SDK, QtCore, ccache | cached | 0:46 | 27.5 s, 6 failed | 2:49 |

Package installation takes about 1:15 of every run, and a cold SDK build 2:40 to
3:05. The two uncached application builds differ by five minutes on the same
source, which is runner variance. Pull requests don't save a compiler cache, so
until a `nightly` push has saved one, PR runs take the second row's time; after
that, the third row's (about 3:30 with a passing suite).

Caches: the instrumented QtCore is 29 MiB compressed, the SDK prefix 4 MiB and the
compiler cache 97 MiB. The QtCore key is
`qt-asan-core-26.04-qt<distro Qt package version>-clang<version>-<recipe hash>`, so a
new distro Qt or clang package, or an edit to the recipe, rebuilds it once (about
7:35). Runner image updates don't change it, since the job runs in its own
container.

The run with #76 reverted (attempt 2 of
[39266994848](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39266994848),
throwaway branch since deleted) failed `settings_contract`, `operations_contract`,
`recording_model_contract`, `storage_contract_integration`,
`config_reload_integration` and `storage_locations_integration`, each with
`stack-use-after-scope` read by `QJsonValueRef::toObject()` from
`Settings::validate` in `'ref.tmp' (line 158)`. Attempt 1 of that run failed only
because `download.qt.io` didn't resolve once; the recipe now retries downloads.

## Speed measures

- **SDK prefix caches.** Each builder restores an exact installed SDK prefix.
  Keys hash the dependency lock, the SDK recipes (`tools/ci/sdk-source.sh`,
  `tools/releases/provision-sdk.sh`, the job's own provisioning script), the
  cache implementation, compiler output, installed distro packages, runner
  image, architecture and workspace path. Workflow edits do not invalidate them.
  No partial restore key or cross-OS archive is used. Installed files, modes and
  symlinks are checked against the sealed inventory before compilation; a
  mismatch fails the job. Fork pull requests restore but never save.
- **No SDK checkout on Linux test cache hits.** The Linux test prefix also
  carries the SDK `LICENSE` files that packaging configuration reads, so a hit
  needs no clone. Package builders still clone and verify the pinned SDK source
  and all submodules every run, because packages ship those licenses and the
  source export rebuilds against that tree.
- **ccache for the Linux test and sanitizer builds.** Compiler outputs are cached
  per job. Only pushes to `nightly` save one, trimmed to the entries that build
  used; every other run restores the newest `nightly` cache and does not save.
  See [cache budget](#cache-budget). The build-identity source changes every
  commit and is always recompiled. Package builders compile from scratch.
- **Parallel builds.** Application and SDK builds use every runner CPU.
- **Parallel tests, longest first.** CTest runs twelve tests at once and restores
  the previous run's durations, so the longest tests start first. See
  [hosted CTest schedule](#hosted-ctest-schedule).
- **One contracts job.** Python suites that previously ran in eight jobs run once.

Cache hits are dependency reuse, never application acceptance evidence.
Application binaries, build identities, test results, credentials and VM runs
are outside every cache.

## Cache budget

The repository's Actions cache limit is 10 GB; past it, GitHub evicts the least
recently used entries, which can be a warm QtCore or SDK prefix. On 2026-10-19 the
cache held 9.78 GB in 101 entries and passed the limit (10.62 GB) when #107's
`nightly` push saved. 9.0 GB of it was 22 `ccache-linux-*` entries of 350 to 435 MB:
every `nightly` push, including runs cancelled by the next push, saved a new
`ccache-linux-<distro>-<run>-<attempt>` entry, although a prefix restore only ever
reads the newest one on a ref. Three rules keep the total bounded:

- **One saver per compiler cache.** Only a push to `nightly` saves ccache; pull
  requests, manual runs and the nightly and release callers restore the newest
  `nightly` entry. A cancelled run doesn't save.
- **Trimmed and capped.** The build step records its start time, and afterwards
  `ccache --evict-older-than` drops every entry the build neither hit nor stored,
  so the saved cache is one build's working set, not every object since the first
  save. A ccache hit refreshes the entry's time, so the working set survives.
  `CCACHE_MAXSIZE` caps each job in case the working set grows.
- **Pruning after every `nightly` push.** The `prune caches` job in `ci.yml` runs
  `tools/ci/cache_prune.py` after the Linux and sanitizer jobs, with the run's
  `GITHUB_TOKEN` (`actions: write`, `pull-requests: read`; the nightly and release
  callers grant them, and the job runs only on `nightly` pushes). It deletes, by id:
  every entry of a rolling family (`ccache-*` and `ctest-cost-*` by
  `<run>-<attempt>` suffix, `qt-asan-core-<distro>-`, `putgd-pkgs-v1-*` by ISO
  week, and `grype-db-v6-*` by database) older than the newest on the
  same ref, and every cache of a closed pull request or deleted branch. On `main`,
  `nightly` and `v*` tags it deletes only those superseded entries. Merging a
  pull request pushes to `nightly`, so its caches go with the next run. Exact-key
  caches (SDK prefixes, the Qt installer's) are left to GitHub's seven-day idle
  eviction. `--dry-run` lists the deletions without making them. The job is not
  in the required `CI` summary.

Budget per job, measured 2026-10-19 (compressed archive sizes; the ceiling is
`CCACHE_MAXSIZE`, which bounds what a working set can grow to):

| Cache | Saved by | Entries kept | Size | Ceiling |
| --- | --- | ---: | ---: | ---: |
| `ccache-linux-24.04-*` | `nightly` push | 1 | 405 MB | 1 GB |
| `ccache-linux-26.04-*` | `nightly` push | 1 | 435 MB | 1 GB |
| `ccache-clang-asan-26.04-*` | `nightly` push | 1 | 102 MB | 400 MB |
| `qt-asan-core-26.04-*` | first miss on a ref | 1 per ref | 31 MB | recipe-bound |
| `install-qt-action-*` (Windows, AppImage) | first miss | 1 each | 650 MB | Qt version |
| `putgd-sdk-v1-*` (9 environments) | first miss on a ref | 1 per environment | about 60 MB | dependency lock |
| `ctest-cost-linux-*` | every non-fork run | 1 per ref | under 2 kB | |
| `grype-db-v6-<sha256>` ([vulnerability database](release.md#sboms-and-vulnerability-scans)) | first `sbom` job to see a new database, per ref | 1 per ref | about 195 MB | one database |
| `putgd-pkgs-v1-*` ([package snapshots](#package-installs)) | first run of an ISO week, or new dependency scripts, per ref | 1 per package target | 408-544 MB (Fedora 544, Ubuntu 24.04 408, Ubuntu 26.04 422, Debian 13 421 on 2026-10-22) | dependency lists |

The four package snapshots add 1.8 GB on `nightly` to the steady state below. A new week's
snapshot supersedes the previous week's at the next prune, and a pull request
that changes the dependency scripts saves its own four until it closes.

Steady state after a `nightly` push is about 1.7 GB (the ccache sizes above
still include objects from earlier commits, so the trimmed caches will be
somewhat smaller). Between a push's saves and its prune job, three superseded
ccache entries add about 0.94 GB. With every ccache at its ceiling the steady
state would be 3.1 GB and the peak 5.5 GB. Open pull requests add only small
SDK, QtCore and CTest entries until they close. Hit rates don't change: runs
already restored only the newest `nightly` entry, and that is the one kept.

## Reproduce and investigate

Use normal presets. To reproduce the hosted test schedule locally, select a new
short private TMPDIR, configure with `-DPUTGD_TEST_TIMEOUT_SCALE=3`, then run
`tools/ci/ctest_preflight.py` and, with `PUTGD_TEST_TIME_SCALE=3`,
`python3 tools/ci/ctest_retry.py --ledger LEDGER --junit PATH -- --preset dev
--parallel 12 --resource-spec-file "$PWD/tools/ci/ctest-resources.json"`.
Plain `ctest` works too, without the retry and ledger. Retain the exact command,
registered tests, environment/version identity, JUnit and LastTest.log for each
attempt. Preserve failures before retrying; never fix a long path by weakening
socket checks or silently excluding a test.

Run any contract group with `python3 tools/ci/contracts.py GROUP`; the reports
land in `build/contracts`. Download the per-job artifacts for hosted failures,
and compare full commit/build identity and test names first. A workflow
scheduling or cache change requires hosted validation of both Linux
distributions and the Windows compile before it is reported green.

## Tests retained after the audit

No invalid or redundant test was established in the audit. Long waits protect
real contracts: worker ownership beyond its old 30-second idle expiry, the
15-second client/CLI deadline, incomplete IPC frame disconnection, allocator
cleanup admission, cancellation and subprocess reaping. CLI, client, provider
and SDK-worker checks cover different failure boundaries. Full source114 KDE
CTest evidence totaled 363.86s; 27 sub-second tests contributed only 6.52s,
while 14 tests over ten seconds contributed 271.86s.

The collision-policy fixture waits for a terminal operation with an explicit
15-second budget (scaled like every wait since #351). Its former five-second polling helper silently returned a
`running` operation: the controlled 32-HEAD exhaustion row completed at 5,056 ms
on the local release work run. The no-PUT, exact 32-HEAD, no-copy and final
`collision_exhausted` assertions remain unchanged; this is a test synchronization
bound, not a relaxed production timeout or memory benchmark.

## Hosted CTest schedule

Issue [#62](https://github.com/patrick-hudson/put.gd-desktop/issues/62) cut
hosted Linux CTest from about five minutes to about 80 seconds on each
distro. No test was removed, split or weakened, and every timing assertion above
is unchanged; two tests gained missing synchronization waits (below). Socket
preflight, the `putgd-ci` user and JUnit retention are as before.

- **Durations.** Each distro restores the newest
  `Testing/Temporary/CTestCostData.txt` before CTest and saves it afterwards,
  failed runs included. CTest then starts earlier failures first and the rest
  longest first. Same-repository pull requests save entries too and restore
  their own newest one before the newest `nightly` one. A new test has no
  duration until it has run once.
- **Slots.** CTest runs twelve tests at once on the four-vCPU runner (eight
  until #351; see [parallelism](#parallelism)). Most of the suite is waiting:
  `tools/ci/ctest_cpu_profile.py`, one test at a time on k72, measured 261 CPU
  seconds in 822 summed seconds for 102 tests; `gui_tray` used 3 s of CPU in a
  48 s run.
- **CPU pool.** CPU is the one resource every test shares. With eight slots and
  no limit, the CPU-bound medium tests ran together once the long idle ones
  finished: on 24.04 `s3_delete_integration` took 22 s instead of 8 s and
  `collision_policy_integration` 18 s instead of 9 s, closer to the explicit
  15-second collision bound. Every test that keeps at least half a core busy
  for two seconds or more in that profile declares `RESOURCE_GROUPS`, one `cpu`
  slot per core it uses, in `cmake/TestPolicy.cmake`: 13 tests with one slot
  and `export_av_sync_contract` (1.7 cores) with two. `tools/ci/ctest-resources.json`
  gives the runner three `cpu` slots, so at most three cores' worth of them run
  at once while idle tests use the other slots. Without a resource spec, as in
  a plain local `ctest`, the property has no effect.
- **Shared resources.** The audit found one: Xvfb display numbers. Concurrent
  `xvfb-run -a` calls that start from the same display race for the X server's
  per-display lock, and the loser sleeps two seconds per attempt;
  `x11_native_snapshot` once spent 6.25 s starting Xvfb for a 4 ms test. Each
  Xvfb test now starts its search at its own display (`putgd_xvfb_display`, 200
  upwards) and `-a` still moves past displays that other processes hold.
  Nothing else is shared between tests: buses come from `dbus-run-session`;
  sockets, configuration, data and lock files live in per-test temporary
  directories or `XDG_*` overrides; S3 fixtures bind port 0; process scans match
  only the test's own children; Proton account locks use per-test references;
  `version_contract` builds under its own `Testing/version-contract-*` root. No
  `RESOURCE_LOCK` is needed. A new test that uses a fixed path, port, display or
  bus name must declare one; a new CPU-bound test should join the `cpu` group.
- **Races the schedule exposed.** `native_shortcut_rejection_integration`
  pressed Ctrl+1 as soon as the synthetic watcher received
  `RegisterStatusNotifierItem`. The resident marks its tray available only after
  that call's reply and a follow-up `IsStatusNotifierHostRegistered` query, and
  it drops failure notifications while the tray is unavailable; the 2 s
  duplicate-feedback guard then hides a retry. When the resident's D-Bus replies
  lagged behind the X key event, no notification ever arrived. It failed once at
  twelve slots and once at eight. The test now also waits for the resident's own
  `trayAvailable` diagnostic, as `native_background_startup_integration`
  already does. Pinned to four local cores beside eight busy loops, the old test
  failed 15 of 80 runs and the new one 0 of 120. In `gui_history_view`,
  `uploadProgressRendersBeforeCompletionDuringDelayedRecentReplies` checked the
  "Reading the clipboard" subtitle once, right after the row turned active; the
  `acquiring` state can reach the model one update later. The check now retries
  like its neighbours; the acquisition stays held open, so no later state can
  satisfy it. Under the same load it failed 21 of 40 runs before and 0 of 80
  after. It failed once in local suite runs and never on hosted runners.
- **Floor.** `native_background_startup_integration` takes about 70 s: nine data
  rows in one process, each waiting for a product startup bound. It starts first
  and bounds the wall time.

CTest wall time ("Total Test time (real)") in seconds, all 72 tests unless noted:

| Run | Commit | Slots | Durations | 24.04 | 26.04 |
| --- | --- | ---: | --- | ---: | ---: |
| [39165764037](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39165764037), push to `nightly` (before, with #69) | `70722c3` | 2 | none | 311.90 | 308.66 |
| [39174290324](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39174290324), push to `nightly` (before, 73 tests) | `4d3b6d5` | 2 | none | 313.69 | 308.37 |
| [39167182473](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39167182473), PR #70 | `65caac1` | 8 | none | 99.96 | 105.24 |
| [39168198731](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39168198731), own Xvfb displays | `4142fae` | 8 | restored | 87.59 | 76.32 |
| [39169471820](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39169471820), trial | `1ae2fc5` | 12 | restored | 71.15, 1 failed | 70.69 |
| [39170267410](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39170267410) attempt 1 | `904dcda` | 8 | restored | 81.77 | 75.89 |
| [39170267410](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39170267410) attempt 2 | `904dcda` | 8 | restored | 76.35 | 96.43, 1 failed |
| [39172087415](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39172087415) attempts 1–4, tray race fixed | `68aa356` | 8 | restored | 78.28–84.26, 4 passed | 77.79–83.61, 4 passed |
| [39175234634](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39175234634) attempts 1–4, CPU pool, 73 tests | `54a0a95` | 8 | restored | 77.61–85.43, 4 passed | 77.84–85.45, 4 passed |

Every hosted run from `68aa356` on passed: 16 job runs on the two distros.
The Test step fell from 296–314 s at two slots to 78–86 s, and warm Linux jobs took
3:51 to 6:14 instead of about eight minutes; package installation accounts for
most of the spread. At eight slots the CPU pool left the wall time unchanged
(77.6–85.5 s against 77.8–84.3 s without it), because the long idle tests still
hold every slot for the first 55 s, but it shortened the CPU-bound tests:
`collision_policy_integration` averaged 9.9 s instead of 11.6 s. Twelve slots
reached the 70 s floor before the tray race was fixed; with the pool they are
the next step to try.

### What grew since the 57-test baseline

Between run 38292139041 (2026-10-12, 57 tests, 159 s wall) and nightly push
39161438843 (2026-10-18, 72 tests, 295 s wall), the summed Ubuntu 24.04 test
time rose from 313.8 s to 585.7 s. With `--parallel 2` the wall time was half
the summed time. The growth arrived with new tests and new cases in existing
binaries:

| Source | Added seconds |
| --- | ---: |
| 16 new tests, chiefly `native_background_startup_integration` (69.1 s), `edit_operations_integration` (8.1 s), `proton_transport` (6.9 s), `recording_helper_integration` (5.2 s) and `proton_contract` (4.4 s) | 101.5 |
| `gui_tray`: 8 to 28 test functions, 23.5 s of explicit waits | 46.9 |
| `gui_settings`: 41 to 52 test functions | 41.6 |
| `gui_application`: 8 to 25 test functions | 24.0 |
| `operations_contract`: 41 to 72 test functions | 16.8 |
| `gui_history_view`: 9 to 23 test functions | 9.4 |
| `s3_worker_supervision`, `native_tray_integration`, `s3_integration` and smaller changes | 32.4 |
| `gui_operations_view` folded into other views | -0.8 |

Ubuntu 26.04 matches within a few seconds (340.2 s to 614.6 s summed).

## Flaky tests

From 2026-10-14 to 2026-10-22, 52 of 840 Linux and sanitizer test jobs failed.
From 2026-10-16, 22 failures were timing and ordering flakes on code that was
otherwise green (the same commit passed on a re-run or on the other
distribution), 2 more are open and are not wait problems (#352, #353), and 6
were real bugs that failed everywhere and were fixed in their own pull
requests. Thirteen tests flaked. Each earlier fix (#184, #187, #272, #275,
#277) patched one test. Most shared one cause:

- Tests waited with fixed budgets: 1770 `QTRY_*` and `qWait` uses, every
  `QTRY_*` defaulting to 5 s, and 246 bare `qWait` calls, most of them a
  sleep followed by an assertion.
- The hosted runner has four vCPUs and runs eight tests at once, with SQLite,
  worker threads, child processes and Xvfb. Under that load, a step that takes
  0.2 s alone sometimes takes more than 5 s.

![Every failed Linux and sanitizer test job from 2026-10-16 to 2026-10-22, classified](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/396b54e2d55ef5e4e4dafdf0f66c26824f48b574/docs/evidence/2026-10-22-flaky-tests/ci-failure-history.png)

The rest were tests that assumed which of two operations would take an upload
first, a sleep standing in for a wait, and an X request read before another
connection's ungrab reached the server. The fix has four parts, introduced by
#351.

### Scaled waits

Every wait in a C++ test goes through `tests/fixtures/Waits.h`
(namespace `putgd::test`):

| Use | For |
| --- | --- |
| `PUTGD_TRY_VERIFY(expr)`, `PUTGD_TRY_COMPARE(a, b)` (and `_WITH_TIMEOUT`, `PUTGD_TRY_VERIFY2`) | wait until it holds, then assert it |
| `PUTGD_VERIFY_STEADY(expr, ms)`, `PUTGD_COMPARE_STEADY(a, b, ms)` | it holds, and still holds `ms` later ("arrives once, nothing follows") |
| `waitFor(predicate, ms)`, `waitForSignal(spy, count, ms)`, `waitForWindowExposed(w)`, `waitForWindowActive(w)` | helpers, callbacks and windows |
| `settle(ms)` | fixed, unscaled time: to let a product timer run, or before a check that nothing happened |

A budget scales with `PUTGD_TEST_TIME_SCALE`: 1 when unset (local runs), 3 in
the `linux` job and 5 in `sanitizers` (instrumented code runs about three
times slower). A passing wait returns as soon as its condition holds, so a
larger budget doesn't slow a passing test; it only bounds how long a test
waits for something that will happen. Product timing contracts are not
budgets and never scale. The B2 and S3 fixture servers scale their own
barriers the same way.

`tools/ci/check_test_waits.py` (run by `tests/ci/test_flake_tools.py` in the
`contracts` job) rejects `QTest::qWait`, `qWaitFor`, `QTRY_*`,
`QSignalSpy::wait`, `qWaitForWindowExposed/Active`, and a `settle()` followed
directly by a plain `QVERIFY`/`QCOMPARE` (a sleep standing in for a wait).
Exceptions go in `tools/ci/test-waits-allowlist.json` with a reason and an
exact count per file; today only the manual theme-switch benchmark is listed.

Prefer a wait on the real completion signal or callback over polling where
the code exposes one. For example, `proton_contract` now waits for each
`inspectSharing` reply before asking again, instead of queueing a new request
every few milliseconds, and the tray tests wait for the operation's release
before copying its result (copying earlier is refused with `operation_busy`).

The migration turned each `qWait(N); QCOMPARE(...)` into
`settle(N); PUTGD_TRY_COMPARE(...)`: the N ms still elapse, so a check that
nothing more arrives keeps its window, and a late arrival passes within the
budget. Assertions with side effects stayed plain. `waitFor` polls like
`QTRY_*` (every 50 ms, running the event loop in between) and ends only when
both the scaled budget and its steps have run out, so it is never stricter
than the loop it replaced. About 50 other `settle()` calls are fixed time on
purpose; [#354](https://github.com/patrick-hudson/put.gd-desktop/issues/354)
tracks replacing those that still stand in for a completion.

### Labels and one reported retry

`cmake/TestPolicy.cmake` labels every test. A `unit` test runs synchronously in
its own process (no event loop waits, threads, child processes, sockets, buses
or displays), so its failure is deterministic. Every other test is
`integration`, including new tests until someone lists them as `unit`. The
label follows the behaviour, not the name: `operations_contract` and
`proton_contract` are integration tests.

`tools/ci/ctest_retry.py` runs CTest once, then reruns the failed integration
tests once with the same arguments:

- every test passed: the job passes;
- every failure was an integration test that passed its retry: the job passes,
  with a workflow warning per test;
- a unit test failed, an integration test failed twice, or CTest failed
  without a failed test to show: the job fails.

Nothing passes silently. `tools/ci/flake_ledger.py` writes every test that
failed, retried or not, to the job summary, and records each pass on a retry
in that test's [`flaky-test`](https://github.com/patrick-hudson/put.gd-desktop/issues?q=label%3Aflaky-test)
issue: one issue per test, titled `Flaky test: NAME`, with a comment per
occurrence (run link, job, commit, first failure lines). A recurrence reopens
a closed issue. The `linux` and `sanitizers` jobs need `issues: write`, so
`nightly.yml` and `release.yml` grant it to their `ci` call. Fork pull
requests get a read-only token; their flaky tests still appear in the job
summary, with a warning that the tracker wasn't updated.

The ledger (`flake-ledger.json`) and the retry's JUnit
(`ctest-results-retry.xml`) are kept in the `ctest-linux-*` and
`ctest-clang-asan` artifacts. Nightly publication is unchanged: `publish` still
needs the whole `ci.yml` call to succeed, so a test that fails twice blocks it
and a test that passed its retry doesn't. Test `TIMEOUT`s are hang guards and
scale like the waits: `-DPUTGD_TEST_TIMEOUT_SCALE` (3 on `linux`, 5 on
`sanitizers`) multiplies every explicit one at configure time.

The policy was demonstrated on #351 with temporary tests, since reverted: a
test that failed its first run passed its retry, appeared in the job summary
and opened a tracker issue
([run 39710070705](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39710070705),
[#355](https://github.com/patrick-hudson/put.gd-desktop/issues/355)); a `unit`
test that failed once failed the job without a retry
([run 39711215419](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39711215419)).

### Builder stress lane

`tools/ci/stress_suite.py` runs CTest repeatedly on a builder under CPU
contention and writes a ledger in the same format, with per-test run and
failure counts:

```bash
python3 tools/ci/stress_suite.py --source ~/work/put.gd-x --out ~/work/flake-stress/RUN \
    --iterations 20 --hogs 4 --hog-cpus 0-1 --test-cpus 0-1 --time-scale 3 \
    -- --preset dev -L integration --parallel 12 \
    --resource-spec-file "$HOME/work/put.gd-x/tools/ci/ctest-resources.json"
```

`--hogs` starts busy loops (pinned with `--hog-cpus`), `--test-cpus` pins CTest
and every test to a CPU subset, and `--time-scale` sets `PUTGD_TEST_TIME_SCALE`.
It uses a private short `TMPDIR` and `XDG_STATE_HOME`, stops only the
processes it started, by PID, and removes only its own temporary directory.
Run it on a builder, one suite per host at a time, after checking
`tools/lab/desktop_lock.py status` for a quiet-slot lock.

Results, `tools/ci/stress_suite.py` on the builders, 2026-10-22. The known set
is the twelve tests that flaked in CI. Pinning (`--test-cpus`, `--hog-cpus`)
is what reproduces a starved runner: with six unpinned busy loops the nightly
passed 8 of 8 runs.

![Runs with a failed test under CPU contention, nightly 33c768e against #351](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/396b54e2d55ef5e4e4dafdf0f66c26824f48b574/docs/evidence/2026-10-22-flaky-tests/stress-before-after.png)

| Host | Load | Tests | Nightly `33c768e` | #351 |
| --- | --- | --- | --- | --- |
| d69 (Debian 13, Qt 6.8) | 4 busy loops and the tests on 2 of 4 CPUs | known set | 6 of 8 runs failed (`operations_contract`, 6) | 0 of 8 (`4984e80`) |
| m74 (Mint 22.3, Qt 6.4) | same | known set | 1 of 8 (`operations_contract`) | 0 of 10 (`7ffcf42`) |
| d69 | 3 busy loops and the tests on 1 of 4 CPUs | known set | 6 of 6 (`gui_history_view` 5 s budget, `operations_contract`) | 0 of 6 (`ea703c6`, timeouts scaled) |
| k72 (Kubuntu 26.04, Qt 6.10) | 4 busy loops | the 74 `integration` tests, 12 at once | | 0 of 8 (`c6918fe`) |

The lane also found four races the scaled waits alone did not fix, each fixed in
#351: `onDemandPreviewsAreBounded` slept 1.5 s for the after-operation posters
(it now waits for the new `previews` diagnostic), three operations tests
assumed the first operation takes the shared upload lock, and an X11 test read
grabs before the resident's ungrab reached the server. The ledgers are in
`docs/evidence/2026-10-22-flaky-tests/stress/`.

### Weekly run on k72

Since 2026-10-21 the lane runs every week on k72 (Kubuntu 26.04, four cores
like the hosted runner), so a new flaky test shows up before it turns CI red
([#360](https://github.com/patrick-hudson/put.gd-desktop/issues/360), approved
by Patrick). `tools/ci/stress_weekly.py` runs as the systemd user service
`putgd-stress-weekly.service`, which `putgd-stress-weekly.timer` starts on
Sundays at 03:00 local time. `Persistent=false` means a missed week isn't caught
up. Each run does this:

1. **It checks first and skips the week** with the reason logged when:
   - the k72 desktop lock (`tools/lab/desktop_lock.py`) is held for quiet, A/V
     or performance work (its purpose names quiet, recording, A/V, audio,
     video, perf, latency, benchmark, memory, timing or profiling);
   - another weekly run holds `~/.local/state/putgd-stress/weekly.lock`;
   - a `ctest` or `stress_suite.py` process (any owner, matched by its exact
     name) is still running, or the load is still above the CPU count, after
     an hour of waiting. It never stops those processes.

   It repeats these checks after the build, before the tests start.
2. **It fetches `origin/nightly`** into its own tree, `~/work/put.gd-stress`,
   with `build/sdk` and `build/dependencies` linked to `~/work/put.gd/build/`.
   It configures with `-DPUTGD_TEST_TIMEOUT_SCALE=3` and builds at
   `nice -n 10 -j3`. It removes the previous `build/dev` first only when the
   disk lacks the reserve from [lab-storage.md](lab-storage.md): 10 GiB plus
   the build.
3. **It runs the tests:** `stress_suite.py --iterations 10 --hogs 4
   --time-scale 3 -- --preset dev -L integration --parallel 12
   --resource-spec-file tools/ci/ctest-resources.json`.
4. **It writes the results** to `~/work/flake-stress/weekly-DATE/` on k72:
   `ledger.json`, `summary.md`, each iteration's JUnit and log, `build.log`,
   `weekly.log` and `weekly.json` (status, commit, timings and the tracker
   outcome). It keeps eight weeks of these directories and deletes only older
   ones that it created itself.
5. **It reports to the tracker:** `flake_ledger.py issues` adds a comment to
   the `flaky-test` issue of every test that failed a run, opening or
   reopening the issue as CI does. The comment gives the failure count, the
   commit and the first failure line. This uses a fine-grained token at
   `~/.config/putgd-stress/github-token` (see
   [remote-builders.md](remote-builders.md#weekly-stress-lane-on-k72)).
   Without the token, the run logs why and keeps its local results.

To see a week's result, read `summary.md` in the newest `weekly-*` directory,
or run `journalctl --user -u putgd-stress-weekly` on k72. A test that fails
only in this lane runs under more contention than the hosted runner. Treat it
like a CI flake: find the wait that assumes a fast machine.

The first runs, on 2026-10-21, were started by hand
(`docs/evidence/2026-10-22-stress-weekly/`):

- A fake quiet A/V lock in a test copy skipped the week and logged why.
- The first full run timed out five native and X11 tests in every iteration.
  They had inherited the Plasma session from the systemd user manager (see
  [remote-builders.md](remote-builders.md#weekly-stress-lane-on-k72)). Since
  that fix, the children get a headless environment.
- The rerun built incrementally in 31 s, then ran the 74 integration tests ten
  times in about 118 s each, 20 minutes in all.
  `capture_process_integration` failed 1 of 10 runs, now
  [#398](https://github.com/patrick-hudson/put.gd-desktop/issues/398).

![The weekly stress lane on k72: the timer, a skipped week, a full run and the missing-token path, code 3ae60a5](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/33f03566b9594d130c6a91186f0fcf916892072b/docs/evidence/2026-10-22-stress-weekly/stress-weekly-k72.png)

### Parallelism

`tools/ci/ctest_cpu_profile.py` runs each test alone and reports its wall and
CPU time; `cmake/TestPolicy.cmake`'s CPU pool comes from its k72 profile
(`docs/evidence/2026-10-22-flaky-tests/k72-cpu-profile.json`). With that pool,
the full suite on k72 (four cores, like the runner) took:

![Full CTest wall time against tests run at once on k72](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/396b54e2d55ef5e4e4dafdf0f66c26824f48b574/docs/evidence/2026-10-22-flaky-tests/parallelism.png)

| `--parallel` | CPU pool | k72 wall (median of 3) | Failures |
| ---: | --- | ---: | --- |
| 8 | before: 8 tests | 106.8 s | none |
| 6 | profiled, 14 tests | 137.5 s | none |
| 8 | profiled | 113.2 s | none |
| 10 | profiled | 97.4 s | none |
| 12 | profiled | 83.8 s | one `x11_isolated` grab race, fixed in #351; then 10 of 10 runs passed, 83.3-84.1 s |

The longest test takes about 75 s, so twelve at once is close to the floor.
The load average was 4-5 on four cores. On the hosted runners the CTest step
went from 107.4 s (24.04) and 97.8 s (26.04) at eight
([run 39704821072](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39704821072))
to 76.6 s and 95.0 s at twelve
([run 39712963621](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39712963621)).
A retry costs only the failed tests' own time. The repository is public, so
hosted minutes are free; the change saves about 15-30 s per Linux job.

## Package installs

On 2026-10-22 a Fedora mirror sync turned every pull request's
`packages / native (fedora44)` job red for about 40 minutes
([#368](https://github.com/patrick-hudson/put.gd-desktop/issues/368)). The
`updates` metadata listed mesa 26.2.4 before any mirror served it. dnf tried
every mirror, `dl.fedoraproject.org` included, and got 404 or a broken TLS
connection. Retrying the same install a few minutes later, or falling back to
the master mirror, would not have helped.

![Before: run 39717118907, the fedora44 job fails when no mirror serves mesa 26.2.4](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/174b27a826aa34532dee6e22c0ddda70682e4a3e/docs/evidence/2026-10-22-package-installs-368/368-before.png)

Package jobs install through `tools/ci/install-packages.sh` (dnf or apt-get).
`tools/releases/install-native-deps.sh` calls it, so do the AppImage, weston
and dwz steps. It works in three layers:

- **Bounded attempts.** Up to 4 online attempts (`PUTGD_PKG_ATTEMPTS`), waiting
  30, 60 and 120 s between them (`PUTGD_PKG_BACKOFF`). Every retry downloads
  fresh metadata: `dnf --refresh`, or `apt-get update --error-on=any`, which
  fails on any index it couldn't fetch instead of installing from stale lists.
  dnf retries each download 10 times across mirrors (`retries=10`) and apt 3
  times. If a package name doesn't exist, the install fails at once with no
  retry.
- **Package snapshot.** The native package jobs restore
  `/var/cache/putgd-packages` from `actions/cache`, under the key
  `putgd-pkgs-v1-<target>-<hash of both scripts>-<ISO week>`. It holds the
  package metadata and exactly the packages of the last successful install.
  Online attempts start from a copy of it, so packages already in it aren't
  downloaded again. After an online success, the copy, pruned to the installed
  packages, replaces it. The cache is saved on that week's first run and when
  the dependency scripts change, never after a fallback. If all online attempts
  fail, the install runs once more offline from the snapshot
  (`dnf --cacheonly`, `apt-get --no-download`). That builds against a package
  set at most a week old instead of failing the job. A dependency list that
  changed since the snapshot can't be satisfied offline, and the step fails.
- **Reporting.** Each failed attempt, a retry that passed and a snapshot
  install are warning annotations and rows of a "Package install retries"
  table in the job summary. A clean install writes nothing. The pre-checkout
  bootstrap (git only) retries 3 times inline and reports in a table of the same form.

![After: run 39729960271 (throwaway e8f9443b on aca7858f), retries, snapshot installs and a red all-fail job](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/174b27a826aa34532dee6e22c0ddda70682e4a3e/docs/evidence/2026-10-22-package-installs-368/368-after.png)

The pictures come from #388. Before: run 39717118907, PR #357's head
`fd9e255`. After: run 39729960271 on a throwaway commit (`e8f9443b`, PR head
`aca7858f` plus a demo-only workflow change that wrapped `dnf` and `apt-get`
to inject mirror faults). There, a fault on the first attempt passed on its
retry, four failed attempts installed from the snapshot and built the package,
and a fault on every attempt and on the snapshot failed the job. Job summaries
and logs are visible only to signed-in viewers, so the after picture lists the
same lines from the check-runs API. The files are in
`docs/evidence/2026-10-22-package-installs-368/`. To repeat it, push such a
wrapper to a pull request branch, never to `nightly`.

Not covered yet: the `linux`, `sanitizers` and `contracts` jobs in `ci.yml`
still use bare `apt-get`
([#389](https://github.com/patrick-hudson/put.gd-desktop/issues/389)). `npm ci`
in the Proton helper step runs with `npm_config_fetch_retries=5`. Its separate
exit-152 failure
([#332](https://github.com/patrick-hudson/put.gd-desktop/issues/332)) is a
CPU-time kill, not a network error.

## Proton Linux runtime

Linux test and package jobs provision the pinned official JS helper before
configuring with `PUTGD_BUNDLE_PROTON=ON`. Provisioning runs helper tests,
compiled one-request smoke checks and the large stdout-drain fixture. Native and
AppImage audits require the exact provisioned manifest and source/notices files;
collection also verifies the corresponding-source transport. The Linux x86-64
lock is not used by the Windows compile. Hosted contracts do not replace
authenticated packaged desktop and resource acceptance.

## Rollout measurements (October 2026)

Hosted runs from the first day of the redesigned pipeline. "Cold" means the
cache keys had just changed; "warm" restored the caches the `nightly` push run
saved. Wall time runs from the trigger to the last job.

| Run | Event | Caches | Wall |
| --- | --- | --- | ---: |
| [39091022143](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39091022143) | PR #61, every path class | cold | 16:15 |
| [39092148362](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39092148362) | push to `nightly` | cold | 14:16 |
| [39093024865](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39093024865) | promotion PR #63 | warm | 8:21 |
| [39093014897](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39093014897) | manual nightly, published `v0.1.1-dev.309` | warm | 15:49 |
| [39093989128](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39093989128) | manual nightly, unchanged source | none needed | 0:20 |
| [39093626492](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/39093626492) | push to `main`, cut `v0.1.1-rc.1` | warm | 13:43 to draft |

Warm Linux test jobs took about eight minutes. Of that, 1:17 to 2:03 is
package installation, under 30 seconds is the ccache build, and about five
minutes is CTest. Issue
[#62](https://github.com/patrick-hudson/put.gd-desktop/issues/62) cut that to
under 90 seconds; see [hosted CTest schedule](#hosted-ctest-schedule).

| Linux 24.04 step | Cold | Warm |
| --- | ---: | ---: |
| Build (Debug, Proton bundled) | 4:41 | 0:24 |
| CTest (67 tests) | 5:06 | 5:02 |
| SDK provisioning | 2:48 | skipped |

Windows dropped from 7:42 cold to 3:21 warm. The published nightly spent 2:20
collecting and 3:13 publishing after its builds. The unchanged-source nightly
ran only `plan` and skipped every other job.

The first nightly also showed the Fedora package builder rebuilding the SDK on
a cache it should have hit: `rpm -qa` lists packages in database order, so the
key changed between runs with identical packages. Pull request
[#64](https://github.com/patrick-hudson/put.gd-desktop/pull/64) sorts the
inventory before hashing.

## Previous pipeline measurements

These describe the pipeline before October 2026's redesign: `build.yml` on every
pull request and branch push, `release.yml` for packages, and a coordinator that
dispatched one into the other. They are retained for comparison only.

From 2026-10-10 to 2026-10-17 it started 175 runs for 17 merges. Pull request
Build runs averaged 15.0 minutes, branch-push Build runs 14.4 minutes and
dispatched nightly package runs 17.7 minutes, including a 16-minute wait for a
separate Build run. On a warm cache, Ubuntu 24.04 spent 1:22 installing
packages, 1:24 cloning the SDK source, 9:37 building and testing, and 2:37 on a
second Release compile.

Hosted Build run `38292139041` (source117, `a91a7ce`) restored and validated the
exact SDK prefix on every platform. One matched cold/warm pair, in seconds:

| Job | Cold | Warm |
| --- | ---: | ---: |
| Ubuntu 24.04 | 861 | 633 |
| Ubuntu 26.04 | 918 | 766 |
| Windows 2022 | 426 | 334 |
| macOS 14 | 344 | 261 |

Warm Linux CTest times were 159.22 and 173.27 seconds. Earlier run 38201680155
took 15m21s/16m38s on Linux, with SDK builds of 183s/213s and separate
production builds of 136s/146s.
