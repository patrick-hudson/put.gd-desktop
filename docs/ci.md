# CI coverage and runtime

The Build workflow checks every pull request, `main`/`nightly` push and manual run.
Ordinary feature-branch pushes with an open PR do not create a second matrix.
Promotion PRs still validate their merge source independently of integration pushes. New
runs cancel obsolete runs for the same PR/ref; cancellation is not a passing result.

## Coverage kept

Ubuntu 24.04 exercises the minimum supported distro Qt API; Ubuntu 26.04 also
builds the current LayerShellQt integration. Both run the full registered CTest
suite as an unprivileged user with private temporary paths and saved socket
preflight, JUnit, test logs and build identity. Hosted Xvfb/offscreen contracts do
not establish compositor support or physical memory acceptance.

Windows and macOS compile the shared production code with tests disabled.
Their official Qt installation and pinned QtKeychain/AWS SDK builds remain
separate from Linux distro dependencies. The S3 wire parser links Qt Network on
these platforms because it uses QHostAddress; Linux uses its native parser and
keeps Qt Network out of the resident link requirements.

For ordinary runs, each Linux job also verifies the independent Release
configuration without QtTest, lab fixtures or benchmark dependencies.
Package-coordinated runs use the audited native package builds for that
configuration instead of compiling the same Linux production targets twice;
Debug tests and portable checks remain mandatory. Saved live-S3 numeric receipts
are replayed once on Ubuntu 24.04; this is platform-independent historical
validation, not a fresh cloud or desktop run. Lab identity/storage and process
observer safety contracts retain their structured reports.

## Pipeline coordination

A plain `main` push runs general CI; packaging runs on relevant PRs, release tags
and explicit tagged dispatches. Fewer jobs after a plain merge reflect these
triggers, not deleted tests. Nightly's thin coordinator explicitly dispatches the
package workflow for a newly changed source, using its exact tag and workflow recipe.

The exact-source Build producer and four package jobs run in parallel. Publication
requires both; a skipped publication job on a PR is expected. Producer verification
checks repository, workflow ID/path, full source, latest attempt and every named
required job. Existing successful push/manual Build evidence is reused. PR merge builds are
not exact-source evidence, even when Actions reports the branch head SHA. Missing evidence triggers
one exact-tag dispatch rather than another moving-branch build. Main, candidate,
stable and nightly paths share the coordination rules and speed improvements.

Two generic Linux production compiles are omitted only when package builds cover
them. Qt/distribution ABI, compiler, flags and tagged identities remain separate;
there is no application-binary cache or debug-to-release binary substitution.
Record actual hosted timings before claiming savings. The nightly dispatcher uses
explicit Bash pipefail semantics so preserving a failure log cannot mask its exit.

## Speed changes

- Exact installed SDK-prefix caches avoid recompilation on a matching environment.
  Keys include dependency lock, provisioning workflow, cache implementation,
  compiler output, distro packages, runner image, architecture and workspace path.
  Windows includes MSVC/Windows SDK identity; macOS includes installed Homebrew
  package versions. No partial restore key or cross-OS archive is used.
- Source commit and recursive SDK submodules are verified on every run, including
  cache hits. Cached installed files, modes and symlinks are independently checked
  against their saved inventory before compilation. A mismatch fails the job.
  Application binaries, build identities, test results, credentials and VM runs
  are outside the cache. Fork PRs can restore scoped caches but do not save them.
- The existing official Qt installer uses its version-specific cache.
- CTest runs at bounded parallelism of two. Tests use private temporary trees,
  independent D-Bus sessions, dynamic ports and isolated Xvfb displays. Native
  desktop input and physical memory benchmarks remain exclusive outside CI.

## Tests retained after the audit

No invalid or redundant test was established in the audit. Long waits protect
real contracts: worker ownership beyond its old 30-second idle expiry, the
15-second client/CLI deadline, incomplete IPC frame disconnection, allocator
cleanup admission, cancellation and subprocess reaping. CLI, client, provider
and SDK-worker checks cover different failure boundaries.

A previous passing hosted run (38201680155) took 15m21s/16m38s on the Linux jobs.
Their SDK builds took 183s/213s, and the separate production builds 136s/146s.
Windows/macOS dependency builds took 334s/290s. These are measured old-run costs,
not promised savings. Keep cold-cache and warm-cache results separately when
comparing subsequent runs. Full source114 KDE CTest evidence totaled 363.86s;
27 sub-second tests contributed only 6.52s, while 14 tests over ten seconds
contributed 271.86s. Removing cheap coverage would barely affect runtime.

## Reproduce and investigate

Use normal presets. To reproduce the hosted schedule locally, select a new short
private TMPDIR, enumerate CTest JSON and preflight resolved socket paths before
starting tests. Then run `ctest --preset dev --parallel 2 --output-junit PATH`.
Retain the exact command, registered tests, environment/version identity, JUnit
and LastTest.log for each attempt. Preserve failures before retrying; never fix a
long path by weakening socket checks or silently excluding a test.

Download the per-job Actions artifacts for failures. Compare full commit/build
identity and test names first. Cache hits are dependency reuse, never application
acceptance evidence. A workflow scheduling or cache change requires hosted
validation on both Linux distributions and both portable compilers before it is
reported green.

## Verified cache restoration (source117)

Hosted Build run `38292139041` passed all four jobs at
`a91a7ce1dc2a1ef8cefa1ed34c5abac9b4a1bb8a`. Its second attempt restored and
validated the exact SDK prefix on every platform, reverified AWS 1.11.907 and
all 15 pinned submodules, and skipped SDK provisioning. Windows/macOS also
restored Qt. Both Linux jobs retained 57 tests. One matched cold/warm pair:

| Job | Cold seconds | Warm seconds |
| --- | ---: | ---: |
| Ubuntu 24.04 | 861 | 633 |
| Ubuntu 26.04 | 918 | 766 |
| Windows 2022 | 426 | 334 |
| macOS 14 | 344 | 261 |

Warm Linux CTest times were 159.22 and 173.27 seconds. These are observed hosted
job durations, including their normal setup/artifact work, not promises about
future runner performance or new graphical acceptance. The run's retained
JUnit, cache/environment manifests and logs provide the detailed record.

The collision-policy fixture waits for a terminal operation with an explicit
15-second limit. Its former five-second polling helper silently returned a
`running` operation: the controlled 32-HEAD exhaustion row completed at 5,056 ms
on the local release work run. The no-PUT, exact 32-HEAD, no-copy and final
`collision_exhausted` assertions remain unchanged; this is a test synchronization
bound, not a relaxed production timeout or memory benchmark. Numeric terminal
timings accompany the fixture's normal retained test output.

## Proton Linux runtime

Linux jobs provision the pinned official JS helper before configuring either
preset with `PUTGD_BUNDLE_PROTON=ON`. Provisioning runs helper tests, compiled
one-request smoke checks and the large stdout-drain fixture. Native/AppImage
release audits require the exact provisioned manifest and source/notices files;
publication also verifies the corresponding-source transport. The Linux x86-64
lock is not used by portable Windows/macOS compilation. Hosted contracts do not
replace authenticated packaged desktop and resource acceptance.
