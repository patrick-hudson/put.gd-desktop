# Build

Linux baseline: GCC 13, CMake 3.24+, Ninja, Qt 6.4.2+, QtKeychain 0.14.2+.
Install normal distribution development packages (Ubuntu/Mint):

```sh
sudo apt-get update
sudo apt-get install build-essential git cmake ninja-build pkg-config \
  qt6-base-dev qt6-base-dev-tools libqt6sql6-sqlite qt6-wayland-dev \
  qtkeychain-qt6-dev libxcb1-dev libxcb-keysyms1-dev libxcb-util-dev \
  libxcb-image0-dev libxcb-randr0-dev libxcb-xfixes0-dev libxcb-shape0-dev libxcb-xtest0-dev xvfb xauth dbus-daemon \
  libxkbcommon-dev libxkbcommon-x11-dev libcurl4-openssl-dev libssl-dev \
  zlib1g-dev libwayland-dev wayland-protocols
```

Build the pinned AWS SDK with its own official CMake project. The app configuration
never downloads dependencies. SDK libraries install under `build/sdk`; this is a normal
CMake dependency install prefix, not a replacement compiler or Qt distribution.

```sh
git clone --depth 1 --branch 1.11.907 --recurse-submodules --shallow-submodules \
  https://github.com/aws/aws-sdk-cpp.git build/dependencies/aws-sdk-cpp
python3 tools/verify_dependencies.py build/dependencies/aws-sdk-cpp
cmake -S build/dependencies/aws-sdk-cpp -B build/aws -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/build/sdk" \
  -DBUILD_ONLY=s3 -DBUILD_SHARED_LIBS=ON -DENABLE_TESTING=OFF \
  -DAUTORUN_UNIT_TESTS=OFF -DENABLE_UNITY_BUILD=ON -DUSE_OPENSSL=ON
cmake --build build/aws --parallel 2
cmake --install build/aws
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/putgd
```

`cmake/dependency-lock.json` records the source commit and transitive submodule commits.
Verify these before configuring the SDK. Source hashes, rather than a floating branch,
define the dependency. Refresh the manifest deliberately when updating the SDK.
Only S3/core and their CRT dependencies are built. The SDK uses its own C++ settings;
application targets require C++20. SDK headers are isolated to the S3 adapter.

The build creates `putgd`, `putgd-cli`, and the private `putgd_s3_module` target
(`libputgd-s3.so` on Linux). Keep the app and module from the same build together.
Build-tree lookup uses its configured absolute directory. Installed applications
resolve the module under the package's private library directory; there is no
working-directory or user-configured plugin search. The module validates its
contract, source/toolchain/Qt build identity and safe-delete transport before use.
After a package update, restart a running resident before using S3; an incompatible
or missing module produces `s3_module_unavailable` with restart/reinstall guidance.

Local-only startup does not initialize or load the AWS SDK. The first provider
request starts its dedicated worker and loads the module. That worker owns SDK
initialization, requests and shutdown; the module's code remains mapped until
process exit. The official SDK runtime and module are packaged privately with
hashes in `share/putgd/aws-runtime-manifest.json`. Qt remains dynamically linked
to distribution libraries. The `s3_module_integration` contract covers absent,
wrong-contract, wrong-build and wrong-transport modules, concurrent first use,
shutdown and recovery in a new process.

Use `release`, `asan`, or `tsan` in place of `dev` for application presets. ASan/UBSan
and TSan are separate builds. The SDK is currently built in Release; sanitizer evidence
applies to application code, not an instrumented dependency. Set `-DBUILD_TESTING=OFF`
at configure time to exclude QtTest. `ctest` uses the offscreen Qt platform for shared
contract tests and a disposable Xvfb display for native X11 protocol tests. These
checks do not certify capture, clipboard ownership, or global hotkeys on a real desktop.

Windows/macOS provision the same Qt/QtKeychain/SDK packages with their normal package
manager and pass their installation prefix using `CMAKE_PREFIX_PATH`. Native desktop
adapters are Linux-first. Cross-platform compile results will be recorded by CI.

Local bootstrap evidence (Mint 22.3, GCC 13.3, Qt 6.4.2, QtKeychain 0.14.2):
the pinned SDK compiled and installed; the Debug app and both contract tests built;
SQLite transaction rollback, keychain fallback settings, SDK lifetime, and conditional
PUT/DELETE header serialization passed. Release also built with `BUILD_TESTING=OFF`
and contains no QtTest target. ASan/UBSan contract tests passed outside the restricted
process sandbox; inside that sandbox LeakSanitizer could not inspect its own threads.
Thread-checking setup and dependency instrumentation are documented in
[thread-sanitizer.md](thread-sanitizer.md). Current observed results are recorded
in [verification.md](verification.md).
The S3 integration tests use an isolated local HTTP fixture with synthetic keys;
keychain tests use an isolated D-Bus protocol peer or an unavailable service.
They do not contact AWS or read the user's secret store. CI jobs are authored
for Ubuntu 24.04/26.04, Windows, and macOS; they have not run yet.

U1 changes build configuration and introduces the skeleton. No prior implementation or
tests existed. Replacement evidence is the actual dependency compile/link and runtime
contract tests above; the first app configure failed as expected before SDK provisioning.

The Linux resident uses Unix-domain sockets through Qt Core notifications. Qt
Network remains a build dependency for the CLI, compatibility tests and lazy S3
module; non-Linux builds retain Qt local sockets. The native IPC contracts join
the ordinary `ctest --preset dev` run and its existing CI JUnit artifact. Physical
Cinnamon RSS measurements use the separate procedure in
[testing-memory.md](testing-memory.md), not a headless CI substitute.
