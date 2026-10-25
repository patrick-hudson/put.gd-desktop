# Reproduction commands and exact boundaries

All SSH commands use the existing d69 host pin and workstation-only SSH key as
specified in `docs/remote-builders.md`. No key is transferred. The dedicated
host root is `/home/patrick/work/putgd-ci393`; its isolated `src` checkout is
created from an exported Git bundle. The original checkout is never edited.

Baseline source: `50053b01f1bbd7f3776a53513bb8c43151ce0de8`.
Candidate: `5683c71f4b99f2c64203b67836653f0d41b23baa`.

```sh
python3 tools/lab/desktop_lock.py status d69
# On d69 before each heavy phase:
uptime
nproc
free -g
df -h /home

# Native baseline/final, in the isolated matching source checkout:
cmake --preset dev -DPUTGD_TEST_TIMEOUT_SCALE=3
nice -n 10 cmake --build --preset dev --target test_operations --parallel 2
nice -n 10 python3 /path/to/baseline_repeat.py SOURCE NEW_BASELINE_OUTPUT
nice -n 10 python3 /path/to/fixed_repeat.py SOURCE NEW_FIXED_OUTPUT

# Signed rootless APT probe, after checking out the candidate:
nice -n 10 python3 mirror_probe.py SOURCE NEW_PROBE_OUTPUT
python3 tests/ci/test_hosted_apt_mirrors.py
python3 tests/ci/test_install_packages.py
```

The diagnostic applies `diagnostic.patch` to the baseline C++ test, builds that
same target and preflights
`/tmp/pg393d/test_operations-XXXXXX/runtime/putgd/session.sock`. It runs one busy
loop and `test_operations resumableUploadJournalSurvivesForRetry`, both nice 10
and pinned to CPU0, with `PUTGD_TEST_TIME_SCALE=3`, private `/tmp/pg393d`, absent
`DISPLAY`, `WAYLAND_DISPLAY`, `DBUS_SESSION_BUS_ADDRESS`, and a 40-second outer
`timeout`. Only the recorded hog PID is terminated. Its failed wait/assertions
and dirty build identity are intentional evidence, not a clean candidate.

The two disposable containers use the same digests as `package.yml`:

```text
ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
ubuntu:26.04@sha256:f144425ff09be612d6d9ad965196e9cdc23dae1f42110a8a11a3e9a8198759f7
```

Each is created with `--cpus=2 --memory=8g`, a dedicated volume `/lab` mapped to
this task's host root, and read-only `/sdk` and `/dependencies` mounts of the
builder's existing official SDK prefix/source. These provider-neutral operation
tests do not load S3; reusing the prefix for CMake discovery is not AWS runtime
or package ABI acceptance. Native tests use distro Qt 6.8.2. Container contexts
record their own distro Qt, compiler, clean source tree and binary SHA-256.

Inside each container:

```sh
nice -n 10 bash /lab/src/tools/releases/install-native-deps.sh
nice -n 10 bash /lab/src/tools/ci/install-packages.sh --no-install-recommends \
  libxcb-xtest0-dev xvfb xauth dbus-daemon
# Clone clean final source to /lab/u24 or /lab/u26, and link build/sdk -> /sdk,
# build/dependencies -> /dependencies. Build and tests run as UID/GID 1000.
cmake --preset dev -DPUTGD_TEST_TIMEOUT_SCALE=3
nice -n 10 cmake --build --preset dev --target test_operations --parallel 2
# Ubuntu26 builds with --parallel 1 and taskset -c 1 while Ubuntu24 stress runs.
```

The stress replay script records private environment, preflight, OS/Qt/compiler,
Git source/tree/status, build manifest, test hash and dynamic dependencies before
invoking the unmodified `tools/ci/stress_suite.py`. It runs one suite at a time,
10 iterations, one hog and CTest pinned to CPU0, nice 10, time scale 3, selecting
only the complete `operations_contract` with CTest parallelism 1 and a 360-second
iteration timeout. The fresh short private paths are checked before launch.
The stress helper adds an eight-character TMPDIR suffix, so its conservative
preflight template uses eight `X` characters. Its receipts record the actual private path,
and its ledger records load and per-iteration outcomes; the exit code alone is not treated as a pass.

Raw repeats, failures and build outputs stay on the builder. No graphical
session, cloud credential, production bucket, VM base or signing key is involved.
