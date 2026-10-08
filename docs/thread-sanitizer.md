# ThreadSanitizer dependency build

Production uses distribution Qt. Thread checks use a separate instrumented Qt
dependency because an uninstrumented event dispatcher hides synchronization from
TSan. A minimal queued-call reproducer demonstrated this problem with system Qt;
the same reproducer passes with instrumented Qt. A deliberately racy control still
reports its race. Do not suppress all Qt or application reports.

Use the normal GCC, CMake and Ninja packages from [build.md](build.md). The
dependency below lives in ignored `work/`, changes no system library, and uses
Qt's supported `FEATURE_sanitize_thread` CMake option. The headless contracts and isolated X11 protocol tests do
not certify behavior on a real Cinnamon or Wayland desktop.

Download the official Qt 6.4.2 source archive and its upstream libxkbcommon 1.6
compatibility patch; verify both before extraction or application:

```sh
mkdir -p work/sanitizer-diagnosis
cd work/sanitizer-diagnosis
curl --fail --location --output qtbase-everywhere-src-6.4.2.tar.xz \
  https://download.qt.io/archive/qt/6.4/6.4.2/submodules/qtbase-everywhere-src-6.4.2.tar.xz
curl --fail --location --output qtbase-xkbcommon-1.6.patch \
  https://github.com/qt/qtbase/commit/8af35d27e8f02bbb99aef4ac495ed406e50e3cca.patch
printf '%s\n' \
  'a88bc6cedbb34878a49a622baa79cace78cfbad4f95fdbd3656ddb21c705525d  qtbase-everywhere-src-6.4.2.tar.xz' \
  '893c2ec5ba4695d86aa71cbad42eaecec3a5433f956283265975f93c16379f0b  qtbase-xkbcommon-1.6.patch' | sha256sum --check
tar -xf qtbase-everywhere-src-6.4.2.tar.xz
cd qtbase-everywhere-src-6.4.2
patch --dry-run --fuzz=0 -p1 < ../qtbase-xkbcommon-1.6.patch
patch --fuzz=0 -p1 < ../qtbase-xkbcommon-1.6.patch
cd ../../..
```

Apply the patch once to a fresh extraction. It is Qt's upstream compatibility fix,
not an application workaround or a change to installed libxkbcommon.

Instrument the matching D-Bus, GLib, libsecret and QtKeychain dependencies too.
The normal development packages are `meson libdbus-1-dev libglib2.0-dev
libsecret-1-dev libffi-dev libpcre2-dev libmount-dev libexpat1-dev libgcrypt20-dev`.
For Qt’s instrumented X11 plugin, also install `libxcb-icccm4-dev
libxcb-render-util0-dev libxcb-sync-dev`, alongside the XCB packages in build.md.
Use these official releases; verify the SHA256 before extracting into
`work/sanitizer-diagnosis/`:

| Archive | Official source | SHA256 |
| --- | --- | --- |
| dbus-1.14.10.tar.xz | [D-Bus release](https://dbus.freedesktop.org/releases/dbus/dbus-1.14.10.tar.xz) | `ba1f21d2bd9d339da2d4aa8780c09df32fea87998b73da24f49ab9df1e36a50f` |
| glib-2.80.0.tar.xz | [GNOME release](https://download.gnome.org/sources/glib/2.80/glib-2.80.0.tar.xz) | `8228a92f92a412160b139ae68b6345bd28f24434a7b5af150ebe21ff587a561d` |
| libsecret-0.21.4.tar.xz | [GNOME release](https://download.gnome.org/sources/libsecret/0.21/libsecret-0.21.4.tar.xz) | `163d08d783be6d4ab9a979ceb5a4fecbc1d9660d3c34168c581301cd53912b20` |
| qtkeychain-0.14.2.tar.gz | [QtKeychain release](https://github.com/frankosterfeld/qtkeychain/archive/refs/tags/0.14.2.tar.gz) | `cf2e972b783ba66334a79a30f6b3a1ea794a1dc574d6c3bebae5ffd2f0399571` |

From the repository root, build the dependencies with their normal build systems:

```sh
putgd_tsan_root="$PWD/work/sanitizer-diagnosis"
putgd_tsan_deps="$putgd_tsan_root/deps-tsan"
setarch x86_64 -R meson setup "$putgd_tsan_root/glib-tsan-build" \
  "$putgd_tsan_root/glib-2.80.0" --prefix="$putgd_tsan_deps" --libdir=lib \
  -Dbuildtype=debugoptimized -Db_sanitize=thread -Dtests=false \
  -Dintrospection=disabled -Ddocumentation=false -Dman-pages=disabled \
  -Dglib_debug=disabled -Dnls=disabled
setarch x86_64 -R meson compile -C "$putgd_tsan_root/glib-tsan-build" -j 2
setarch x86_64 -R meson install -C "$putgd_tsan_root/glib-tsan-build"
cmake -S "$putgd_tsan_root/dbus-1.14.10" -B "$putgd_tsan_root/dbus-tsan-build" \
  -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$putgd_tsan_deps" \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread \
  -DDBUS_BUILD_TESTS=OFF -DDBUS_BUILD_X11=OFF -DDBUS_WITH_GLIB=OFF -DENABLE_SYSTEMD=OFF
setarch x86_64 -R cmake --build "$putgd_tsan_root/dbus-tsan-build" --parallel 2
cmake --install "$putgd_tsan_root/dbus-tsan-build"
setarch x86_64 -R env PKG_CONFIG_PATH="$putgd_tsan_deps/lib/pkgconfig" \
  meson setup "$putgd_tsan_root/libsecret-tsan-build" "$putgd_tsan_root/libsecret-0.21.4" \
  --prefix="$putgd_tsan_deps" --libdir=lib -Dbuildtype=debugoptimized -Db_sanitize=thread \
  -Dmanpage=false -Dgtk_doc=false -Dvapi=false -Dintrospection=false
setarch x86_64 -R meson compile -C "$putgd_tsan_root/libsecret-tsan-build" -j 2
setarch x86_64 -R meson install -C "$putgd_tsan_root/libsecret-tsan-build"
```

Configure Qt in a fresh build directory so cached system GLib discovery cannot
replace the instrumented dependency. Keep GLib enabled: libsecret requires its
main context to dispatch asynchronous credential results.

```sh
setarch x86_64 -R env PKG_CONFIG_PATH="$putgd_tsan_deps/lib/pkgconfig" \
  cmake -S work/sanitizer-diagnosis/qtbase-everywhere-src-6.4.2 \
  -B work/sanitizer-diagnosis/qtbase-tsan-build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$PWD/work/sanitizer-diagnosis/qtbase-tsan" \
  -DFEATURE_sanitize_thread=ON -DFEATURE_glib=ON \
  -DFEATURE_opengl=OFF -DFEATURE_xcb=ON -DFEATURE_xkbcommon_x11=ON -DFEATURE_forkfd_pidfd=OFF \
  -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF
setarch x86_64 -R cmake --build work/sanitizer-diagnosis/qtbase-tsan-build \
  --target Core Gui Network Sql Test Widgets DBus \
    QSQLiteDriverPlugin QOffscreenIntegrationPlugin QXcbIntegrationPlugin \
    qtpaths qdbuscpp2xml qdbusxml2cpp --parallel 2
setarch x86_64 -R env PKG_CONFIG_PATH="$putgd_tsan_deps/lib/pkgconfig" \
  cmake -S "$putgd_tsan_root/qtkeychain-0.14.2" -B "$putgd_tsan_root/qtkeychain-tsan-build" \
  -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$putgd_tsan_deps" \
  -DBUILD_WITH_QT6=ON -DBUILD_TEST_APPLICATION=OFF -DBUILD_TRANSLATIONS=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread \
  -DQt6_DIR="$putgd_tsan_root/qtbase-tsan-build/lib/cmake/Qt6" \
  -DCMAKE_PREFIX_PATH="$putgd_tsan_root/qtbase-tsan-build/lib/cmake;$putgd_tsan_deps"
setarch x86_64 -R cmake --build "$putgd_tsan_root/qtkeychain-tsan-build" --parallel 2
cmake --install "$putgd_tsan_root/qtkeychain-tsan-build"
setarch x86_64 -R cmake -S . -B build/tsan-instrumented-qt -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DPUTGD_SANITIZER=thread \
  -DQt6_DIR="$PWD/work/sanitizer-diagnosis/qtbase-tsan-build/lib/cmake/Qt6" \
  -DQt6Keychain_DIR="$putgd_tsan_deps/lib/cmake/Qt6Keychain" \
  -DCMAKE_PREFIX_PATH="$PWD/work/sanitizer-diagnosis/qtbase-tsan-build/lib/cmake;$PWD/build/sdk"
setarch x86_64 -R cmake --build build/tsan-instrumented-qt --parallel 2
setarch x86_64 -R env QT_QPA_PLATFORM=offscreen \
  PATH="$putgd_tsan_deps/bin:$PATH" LD_LIBRARY_PATH="$putgd_tsan_deps/lib" \
  TSAN_OPTIONS="halt_on_error=1:suppressions=$PWD/tests/sanitizer/tsan.supp" \
  ctest --test-dir build/tsan-instrumented-qt --output-on-failure
```

GCC 13 TSan on this host initially aborts with `unexpected memory mapping`.
`setarch -R` disables address randomization only for the child process and its
children; it changes no global setting. It is also required for instrumented
Qt's `moc` during builds. Run outside a process sandbox that prohibits this
personality setting or local fixture sockets.

`FEATURE_forkfd_pidfd=OFF` selects Qt's supported fork backend for this test
dependency. With the default clone/vfork path, GCC 13 TSan aborts internally at
`tsan_rtl.cpp:253` before the fixture child starts. This option retains process
launch and IPC tests; production distribution Qt is unaffected.

One dependency-only lock-order report has a separate source diagnosis. Calling
only `QDBusConnection::sessionBus()` reproduces it with both system and
instrumented D-Bus 1.14.10. `internal_bus_get` holds the global bus lock while
`_dbus_connection_new_for_transport` locks a newly allocated connection. Qt uses
the private-connection path: that new connection has not been published to any
other thread at the construction edge. Later disconnect locks it before the
global bus lock; TSan combines those edges into a hypothetical cycle.
The exact construction function is the sole `deadlock:` entry in
[`tests/sanitizer/tsan.supp`](../tests/sanitizer/tsan.supp). This does not disable
race or lock-order detection. The Qt-only reproduction passes with that entry;
an unrelated two-thread lock-order control still fails with exit 66. Never add
a general Qt, D-Bus or application race suppression to handle other reports.

Use the matching instrumented `dbus-run-session` and daemon from the test prefix.
Do not load its private libdbus ABI into the distribution daemon. The runtime
path changes above apply only to test children.

Application code, these Qt modules, QtKeychain, GLib, libsecret and D-Bus are
instrumented. The existing Release AWS SDK, OpenSSL and libcurl are not
instrumented by this recipe; do not describe a passing run as proof of their internal race
freedom. Investigate actual reports before attributing them to a dependency.
Native adapters require their own real-session checks.

The small reproductions and detector controls are saved under
[`tests/sanitizer/probes`](../tests/sanitizer/probes). They are an opt-in diagnostic
CMake project, excluded from the application test suite. Build them against the
same test dependencies:

```sh
setarch x86_64 -R cmake -S tests/sanitizer/probes \
  -B work/sanitizer-diagnosis/probe-repro-build -G Ninja \
  -DQt6_DIR="$putgd_tsan_root/qtbase-tsan-build/lib/cmake/Qt6"
setarch x86_64 -R cmake --build work/sanitizer-diagnosis/probe-repro-build --parallel 2
setarch x86_64 -R env LD_LIBRARY_PATH="$putgd_tsan_deps/lib" \
  TSAN_OPTIONS="halt_on_error=1:suppressions=$PWD/tests/sanitizer/tsan.supp" \
  work/sanitizer-diagnosis/probe-repro-build/qt_queue
setarch x86_64 -R env PATH="$putgd_tsan_deps/bin:$PATH" \
  LD_LIBRARY_PATH="$putgd_tsan_deps/lib" \
  TSAN_OPTIONS="halt_on_error=1:suppressions=$PWD/tests/sanitizer/tsan.supp" \
  dbus-run-session -- work/sanitizer-diagnosis/probe-repro-build/qt_dbus
```

Both Qt reproductions should exit zero. Run `known_race` and `known_deadlock`
with that same `setarch`/`LD_LIBRARY_PATH`/`TSAN_OPTIONS` environment: each must
report its named defect and exit 66. An exit zero from either control invalidates
race-check evidence. Removing the sole suppression from the D-Bus reproduction
should expose the diagnosed construction lock-order report.
