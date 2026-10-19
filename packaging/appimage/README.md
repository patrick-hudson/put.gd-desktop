# AppImage prerelease assembly

Build x86_64 on Ubuntu 24.04 (glibc 2.39) with the official Qt 6.8.3 gcc_64
installation, normal distro GCC/CMake/Ninja, and the pinned official AWS SDK.
This matches the oldest current Cinnamon lab. A build on Ubuntu 26.04/Fedora 44
is not a compatible substitute: AppImage does not replace glibc.
Qt remains dynamically linked; this format bundles Qt instead of using distro Qt.
DEB/RPM continue using distro Qt.

`tools.lock.json` records immutable upstream releases, observed SHA-256 digests,
and exact LayerShellQt/ECM commits. The Qt plugin's digest was measured from its
fixed official release asset because that release predates GitHub asset digests.
The type-2 runtime is pinned separately so appimagetool does not fetch a mutable
runtime. The builder checks all tools before executing them.

## Build prerequisites

Provision Qt 6.8.3 with the existing pinned install-qt-action in GitHub Actions.
Include its native GTK3 platform theme, Wayland platforms, SQL SQLite plugin,
and Qt Wayland development headers. Do not force a different platform theme or
replace native folder dialogs. The builder rejects a missing GTK3 theme plugin. Qt's standard `qt.conf`
deployment paths select only the SQLite SQL driver, preserving all other plugin
directories and leaving the original official SDK untouched. MySQL/Mimer and
other unused database clients are not application dependencies.

Build `extra-cmake-modules` at `a5f0366eb92baa2a31b24e65f754fe4bc48ca28e`
(version 6.14.0), and `layer-shell-qt` at
`b1317ba122420dd0542ad12f4780b02ba7b6288f` (version 6.4.6), using their own
CMake projects and a private install prefix. LayerShellQt requires Qt >=6.8 and
ECM >=6.14. This is a dependency build, not a replacement toolchain.

```sh
cmake -S extra-cmake-modules -B build/ecm -G Ninja -DCMAKE_INSTALL_PREFIX="$DEPENDENCY_PREFIX"
cmake --build build/ecm --parallel 2
cmake --install build/ecm
cmake -S layer-shell-qt -B build/layershell -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$DEPENDENCY_PREFIX;$QT_ROOT" -DCMAKE_INSTALL_PREFIX="$DEPENDENCY_PREFIX" -DKDE_INSTALL_QTPLUGINDIR="$DEPENDENCY_PREFIX/lib/qt6/plugins" -DBUILD_TESTING=OFF
cmake --build build/layershell --parallel 2
cmake --install build/layershell
```

Check out the immutable commits in `tools.lock.json` with ordinary Git, then
verify `git rev-parse HEAD` against the lock. Clone each `qtLicenseSources` root
without building Qt; these are the matching 6.8.3 license/source notices for the
official binaries. Pass all source roots matching the modules actually bundled.

Build QtKeychain at `8eac9889775ca45c106d077bb0d3744754a1570a`:

```sh
cmake -S qtkeychain -B build/qtkeychain -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_ROOT" -DCMAKE_INSTALL_PREFIX="$DEPENDENCY_PREFIX" -DBUILD_WITH_QT6=ON -DBUILD_TEST_APPLICATION=OFF -DBUILD_TRANSLATIONS=OFF
cmake --build build/qtkeychain --parallel 2
cmake --install build/qtkeychain
```

The screen recorder `putgd-record` links a pinned upstream FFmpeg (`ffmpeg` in
`tools.lock.json`, 7.1.5) that `tools/releases/provision-appimage.py` builds into
the same dependency prefix with FFmpeg's own `configure`, using exactly the
options in `media.json` (`--enable-gpl` with libx264 and libx265, nothing
non-free, no programs, only the demuxer, decoders, encoders, muxers and
filters the export uses). It needs `nasm libva-dev libvpx-dev libwebp-dev
libopus-dev libffmpeg-nvenc-dev libx264-dev libx265-dev` from Ubuntu 24.04. The
GStreamer plugins come from the builder's own `gstreamer1.0-plugins-base`,
`-good`, `-bad`, `-ugly` (`x264enc`), `gstreamer1.0-pipewire`,
`gstreamer1.0-pulseaudio` and `gstreamer1.0-vaapi` packages. The FFmpeg source
checkout under `build/dependencies/appimage/ffmpeg` is passed as a
`--license-source`, so its COPYING.GPLv2, COPYING.LGPLv2.1 and LICENSE.md travel
with the image. After assembly, `build-appimage.sh` runs
`appimage_media_sources.py`, which needs `deb-src` entries for Ubuntu 24.04 (the
CI job adds them) to fetch the bundled GPL source packages for the
corresponding-source asset.

Use the pinned SDK provisioning instructions in `docs/build.md`; build the SDK
with the same distro compiler and OpenSSL/curl, with `USE_OPENSSL=ON`. The
application's release preset accepts `CMAKE_PREFIX_PATH` containing this SDK,
the official Qt root, and the dependency prefix. Configure and build normally:

```sh
cmake --preset release -DCMAKE_PREFIX_PATH="$SDK_PREFIX;$QT_ROOT;$DEPENDENCY_PREFIX" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTING=OFF -DPUTGD_RELEASE_TAG="$RELEASE_TAG"
cmake --build --preset release
```

`putgd-kde-region` must exist: omitting it breaks native KDE region capture.
`putgd-record` must exist and link `libavcodec.so.61` from the dependency
prefix; `build-appimage.sh` refuses a helper linked against the distro FFmpeg.
After the release build, use CMake's `DESTDIR` install into a new staging root.
Do not reuse stale staging contents.

```sh
DESTDIR="$STAGE" cmake --install build/release
python3 tools/releases/portable_appimage.py --stage "$STAGE" --output-dir "$OUTPUT" --tools-dir "$TOOL_CACHE" --qt-root "$QT_ROOT" --dependency-prefix "$DEPENDENCY_PREFIX" --version "$RELEASE_VERSION" --license-source "$QT_SOURCE" --license-source "$QTWAYLAND_SOURCE" --license-source "$QTKEYCHAIN_SOURCE" --license-source "$LAYERSHELL_SOURCE"
```

`--license-source` may be repeated for every upstream Qt module bundled; use the
source corresponding to Qt 6.8.3. The adapter retains upstream LICENSE/COPYING/
NOTICE files and distro copyright notices for deployed system libraries. The
native install already includes the application MIT license and SDK notices.
The LGPL license texts and upstream attribution metadata accompany the bundled
Qt modules. Qt dynamic replacements remain possible by extracting the image. The installer
must also retain the original source/dependency identities in release metadata.

Outputs are `putgd-VERSION-x86_64.AppImage`, `appimage-build.json`, and the AppDir.
The JSON includes original build identity, all deployed file hashes, source
licenses, tool/runtime pins, and the final executable's digest/size/mode.
After relocation, each production helper's actual `--version-json` must match the
full build manifest. Every relocated AppDir ELF and plugin must resolve with image
libraries and no original SDK path in the audit environment; bundled Qt/AWS/QtKeychain/LayerShell libraries
cannot resolve outside the AppDir. The receipt lists the dependency prefixes checked for image-only resolution;
ordinary system libraries remain permitted. These checks audit the relocated
AppDir before packing. Exact-image acceptance verifies the packed contents and
actual desktop behavior separately.
It records desktop acceptance as pending: assembly is not compositor evidence.
Only upload the image and receipt after package-specific acceptance passes.

## Screen recording runtime

`portable_appimage.py` deploys the GStreamer plugins listed in `media.json`
and `gst-plugin-scanner` with linuxdeploy (so their dependencies land in
`usr/lib`), then moves them to `usr/lib/gstreamer-1.0/` and
`usr/libexec/gstreamer-1.0/` with RPATHs pointing at `usr/lib`. Libraries that
must match the host are excluded from every linuxdeploy pass (`hostLibraries`:
libva, libpipewire, the NVIDIA encode libraries, OpenH264); libva and
libva-drm are also kept in `usr/lib/putgd-host-fallback/`, which AppRun adds to
the library path only when the host's linker cache has no `libva.so.2`.
`putgd-record` confines GStreamer to the image itself when it runs from the
mount (see [recording](../../docs/recording.md), "AppImage runtime"); nothing
else in the image or on the host inherits those variables.

On Plasma, AppRun writes a second temporary private entry beside the
ScreenShot2 one: `putgd-record-appimage-XXXXXXXX.desktop`, `Exec` naming the
mounted `putgd-record` and `X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1`,
so KWin offers its screencast protocol to the recorder; the same cleanup
removes both entries.

The image carries Qt's Wayland platform plugins but not its `xdg-shell`
shell integration (only LayerShellQt's), so on a Wayland session Qt falls back
to XWayland for ordinary windows, including the recorder's control bar and
review window; frames still come from KWin's or Mutter's native stream. With
`QT_QPA_PLATFORM=wayland` forced in the environment those windows abort ("No
shell integration named xdg-shell found"). This predates recording and applies
to every window in the image.

The post-deployment audit runs the relocated `putgd-record --capabilities`
with `APPDIR` set and no display. It fails unless GStreamer initialises, every
`requiredElements` entry loads, every plugin comes from `usr/lib/gstreamer-1.0`,
FFmpeg's H.264 decoder opens, WebM, GIF and WebP export are available and no
host-matched library was bundled. The receipt keeps the result under
`postDeploymentAudit.recording` and the media list, fallbacks and FFmpeg pin
under `recordingRuntime`. A build without a GPU (CI) has no H.264 encoder, so
MP4 export is absent there by design; it appears on hosts whose driver encodes
H.264.

The recording runtime grew the image from 98,929,144 bytes (nightly 317) to
112,736,760 bytes (`7aa1aff`): 10.5 MB for GStreamer, FFmpeg and the plugins,
and 3.3 MB for x264/x265. Each release also carries the 18.4 MB
corresponding-source asset. The idle family on the Debian 13 Plasma/Wayland
desktop measured 111.7 MiB RSS / 76.1 MiB PSS for nightly 317 and
112.3 / 76.7 MiB for this image (resident, `putgd-ui`, AppRun and runtime; the
resident itself 29.0 and 29.3 MiB). While recording, the image's helper peaks
at 110-131 MiB, against 141-153 MiB for the distro packages' helpers, whose
FFmpeg carries every codec.

## Compression and memory

The pinned appimagetool uses zstd with 16 KiB SquashFS blocks through its standard
`--mksquashfs-opt` option. The same source119 prototype used 5.461 MiB for FUSE
instead of 16.719 MiB with the tool's default 128 KiB blocks. Its download grew
from 50,813,432 to 55,835,128 bytes. The complete family peaked at 41.652 MiB
versus 52.055 MiB; both failed the unchanged 40 MiB numerical gate. The app itself
was 36.191 MiB in the smaller-block run.

The user accepted AppImage-specific format overhead after this investigation;
this does not change the native package target or turn either failed numerical
gate into a pass. These are dirty development prototypes on the Cinnamon VM,
not final-tag or GNOME/KDE acceptance. All nine functional cases passed and
normal tray Quit removed the resident and FUSE processes. The operation counts
differed (14 versus 9), so attribute the FUSE reduction separately rather than
claim the entire family difference is solely due to compression.

[Recorded prototype evidence](../../docs/evidence/releases/2026-10-12-prototypes.json)
pins the package bytes, commands, observer and results. Include every mount and
launcher process in future quiet-family measurements; never hide FUSE from RSS.

## Runtime behavior

The application AppRun is installed after deployment, so generic deployment
hooks cannot override its host-tool environment or force unavailable GTK2 themes.
AppRun resolves sibling helpers inside the mounted image. Login startup writes
the persistent `APPIMAGE` path rather than a temporary `/tmp/.mount_*` path.
KWin authorizes screenshots against the resident executable's canonical path.
Host permission-cache calls retain the original host library/plugin environment,
and the child retains the caller's umask. On KDE, AppRun uses `mktemp` with mode 0600 to create a unique NoDisplay host
permission desktop entry for the current mount, invokes distro `kbuildsycoca6`,
and removes only that entry on normal exit/termination. Existing user desktop
entries are never rewritten. The resident applies the same executable and
restricted-interface checks to these entries. KWin's own permission check stays
enabled. A missing/rejected host registration is an explicit startup failure.
A killed wrapper cannot guarantee cleanup; stale entries name an unmounted
executable and cannot grant access to a later mount. Normal launches clean up.

AppRun uses a lightweight shell supervisor to keep the permission entry alive;
measure this process as part of the resident family on KDE. Neither this wrapper
nor a successful `--version` invocation establishes the <=40 MiB memory gate.

Notification clicks use the host `gio open` command and its configured HTTP(S)
URI handler to open the uploaded image in the browser. Install the distro GIO
command-line tools (`libglib2.0-bin` on Debian/Ubuntu, `glib2` on Fedora).

The GNOME 50 Shell extension lives on the host and must be installed/enabled from
the same application source. Bundling its files inside the AppImage does not
install a host Shell extension. Keep that setup visible in release instructions.
The production CLI can be called through the extracted AppDir's
`usr/bin/putgd-cli`; MCP continues forwarding only to that production CLI.

Before acceptance, test actual installed image on Cinnamon first: startup,
tray/settings/history, native fonts/dialogs, all four actions, clipboard files /
images / text, configured archives, exact history deletion, autostart after a
fresh mount, and family memory after helpers exit. Then test current GNOME 50
and KDE Wayland (especially region selection, restricted authorization, window
capture and host extension). Keep sessions visible on the VNC wall.

Adapter-only checks, which launch no production application or Unix socket:

```sh
python3 tests/unit/test_portable_appimage.py
ctest --preset dev -R '^login_startup_contract$'
```

Primary sources: [Qt Linux 6.8 requirements](https://doc.qt.io/qt-6.8/linux.html),
[LayerShellQt 6.4.6 build requirements](https://github.com/KDE/layer-shell-qt/blob/v6.4.6/CMakeLists.txt),
[linuxdeploy Qt plugin](https://github.com/linuxdeploy/linuxdeploy-plugin-qt/tree/ce5291e25979d7d6417551d787f308b88c1b8d76),
[KWin authorization](https://github.com/KDE/kwin/blob/Plasma/6.6/src/utils/serviceutils.h).
