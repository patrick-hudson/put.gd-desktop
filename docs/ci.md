# CI coverage and runtime

The Build workflow checks every pull request, default-branch push and manual run.
Branch pushes with an open PR no longer create a second identical matrix. New
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

Each Linux job also verifies the independent Release configuration without
QtTest, lab fixtures or benchmark dependencies. Saved live-S3 numeric receipts
are replayed once on Ubuntu 24.04; this is platform-independent historical
validation, not a fresh cloud or desktop run. Lab identity/storage and process
observer safety contracts retain their structured reports.

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
