# CI coverage and runtime

Four workflows cover every event. Jobs that gate one another run in the same
workflow run and are linked with `needs:`; nothing waits on another workflow
through the Actions API.

| Workflow | Runs on | Does |
| --- | --- | --- |
| `ci.yml` | every pull request, pushes to `nightly`, manual runs; called by nightly and release | contracts, Linux tests, clang ASan/UBSan core tests, Windows compile, packages; on `nightly` pushes, cache pruning |
| `package.yml` | called by `ci.yml` | four native packages, AppImage, and with a tag the collected `release-assets` |
| `nightly.yml` | 05:23 UTC daily and manual | skip unchanged source, or tag, test, package and publish a nightly, then notify put.gd-site |
| `release.yml` | pushes to `main` and manual | cut a candidate (or stable on request), test, package and stage a draft |

The single required status check is the `CI` job. It fails when any job the
change selected did not succeed. A cancelled or failed job is never a pass; a
job skipped because the change cannot affect it is.

## Job selection

`tools/ci/changes.py` classifies a pull request's changed paths. Pushes, the
schedule, manual runs and release callers always select everything.

| Changed paths | contracts | linux, portable | packages |
| --- | --- | --- | --- |
| `docs/` (except below), `README.md`, `AGENTS.md`, `CLAUDE.md` | yes | no | no |
| Lab, MCP and maintenance Python; `tests/ci`, `tests/releases`, `tests/maintenance`; Python and YAML directly under `tests/desktop` and `tests/performance` | yes | no | no |
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
platform, storage and packaging changes don't select it. `tests/ci/test_changes.py`
pins these rules.

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
saved socket preflight, JUnit, test logs and build identity. Hosted Xvfb and
offscreen contracts do not establish compositor support or physical memory
acceptance. The Release-preset compile happens in the package builders, not here.

**sanitizers** builds the `clang-asan` preset (the `asan` preset's
`-fsanitize=address,undefined`, compiled with clang) in an Ubuntu 26.04 container and
runs the core contracts and short unit tests the `clang-asan` test preset selects:
settings, operations, history, storage locations, payload, archive, upload naming,
recording model, storage, config reload, local IPC and the small parser and
platform-model tests. The test preset makes every UBSan report fatal; any ASan
report already is. The job is not in the required `CI` summary yet (issue
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
export on Ubuntu 24.04. With a release tag, `collect` verifies and assembles the
public asset set. See [releases](release.md).

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
- **Parallel tests, longest first.** CTest runs eight tests at once and restores
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
  `<run>-<attempt>` suffix, `qt-asan-core-<distro>-`) older than the newest on the
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

Steady state after a `nightly` push is about 1.7 GB (the ccache sizes above
still include objects from earlier commits, so the trimmed caches will be
somewhat smaller). Between a push's saves and its prune job, three superseded
ccache entries add about 0.94 GB. With every ccache at its ceiling the steady
state would be 3.1 GB and the peak 5.5 GB. Open pull requests add only small
SDK, QtCore and CTest entries until they close. Hit rates don't change: runs
already restored only the newest `nightly` entry, and that is the one kept.

## Reproduce and investigate

Use normal presets. To reproduce the hosted test schedule locally, select a new
short private TMPDIR, then run `tools/ci/ctest_preflight.py` and
`ctest --preset dev --parallel 8 --resource-spec-file tools/ci/ctest-resources.json
--output-junit PATH`. Retain the exact command,
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
15-second limit. Its former five-second polling helper silently returned a
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
- **Slots.** CTest runs eight tests at once on the four-vCPU runner.
  About three quarters of the suite is waiting: profiled locally eight at a
  time, the 72 tests used about 234 CPU seconds in about 900 summed seconds;
  `gui_tray` used 3 s of CPU in a 69 s run.
- **CPU pool.** CPU is the one resource every test shares. With eight slots and
  no limit, the CPU-bound medium tests ran together once the long idle ones
  finished: on 24.04 `s3_delete_integration` took 22 s instead of 8 s and
  `collision_policy_integration` 18 s instead of 9 s, closer to the explicit
  15-second collision bound. Seven CPU-bound tests (`collision_policy_integration`,
  `edit_operations_integration`, `editor_contract`, `history_contract`,
  `s3_delete_integration`, `s3_integration`, `version_contract`) declare
  `RESOURCE_GROUPS "cpu:1"`, and `tools/ci/ctest-resources.json` gives the
  runner three `cpu` slots, so at most three of them run at once while idle
  tests use the other slots. Without a resource spec, as in a plain local
  `ctest`, the property has no effect.
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
