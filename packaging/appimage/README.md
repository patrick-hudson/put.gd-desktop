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
and Qt Wayland development headers. The image also carries Qt's offscreen platform:
the resident runs image decoding and previews in `putgd-ui --image-job` on it, and
Qt aborts that worker when the plugin is absent. After deployment the builder
starts the worker from the AppDir on offscreen and rejects an image where it does
not start. It carries Qt's Wayland client plugins too, which linuxdeploy deploys
only for Qt Wayland Compositor applications: the `xdg-shell` shell integration,
the `bradient` and `adwaita` decorations and the EGL client buffer integration,
the set that Debian's and Fedora's qt6-wayland packages ship. Qt's Wayland
platform does not start without a shell integration (LayerShellQt's is applied
per window, never as the default), so without them every window fell back to
XWayland, and a forced `QT_QPA_PLATFORM=wayland` aborted. The builder then starts
the same worker on Wayland under a private headless weston (install `weston` on
the builder) and rejects an image whose worker does not start with `xdg-shell`.
Do not force a different platform theme or
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
non-free, no programs, only the demuxers, decoders, encoders, muxers and
filters the export and the lab diagnostics use). It needs `nasm libva-dev
libvpx-dev libwebp-dev libopus-dev libffmpeg-nvenc-dev libx264-dev libx265-dev`
from Ubuntu 24.04. The
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

The bundled `pipewire` plugin is the builder's (Ubuntu 24.04, PipeWire 1.0.5),
while libpipewire and the daemon are the host's, often newer, so the recorder
doesn't rely on the plugin's defaults for negotiation. This plugin asks for at least
8 buffers, KWin offers 2-4, and PipeWire 1.5.81 and later refuse to link
ranges that don't overlap, so the recorder sets `min-buffers=1` (#137;
`tests/desktop/pipewire/run_buffers.sh` checks a plugin against a host's
PipeWire without a desktop). A plugin from a newer PipeWire is not a drop-in
replacement: PipeWire 1.6's imports functions that libpipewire 1.0 lacks.

On Plasma, AppRun writes a second temporary private entry beside the
ScreenShot2 one: `putgd-record-appimage-XXXXXXXX.desktop`, `Exec` naming the
mounted `putgd-record` and `X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1`,
so KWin offers its screencast protocol to the recorder; the same cleanup
removes both entries.

On Wayland the recorder's windows behave like the image's other windows (see
"Runtime behavior"): the control bar and the review window are native Wayland
clients, and on Plasma the helper's region selector, ring and dimming are
LayerShellQt overlay surfaces. Frames still come from KWin's or Mutter's
stream. The AppImage recordings in the
[packaging receipt](../../docs/evidence/2026-10-18-recording-packaging.md)
ran before the image carried `xdg-shell`, with those windows on XWayland.
[2026-10-19 Wayland recording](../../docs/evidence/2026-10-19-appimage-recording-wayland.md)
repeats them with nightly `9042776`. On Debian 13 Plasma 6.3 Wayland, Fedora 44
GNOME 50 Wayland and the Kubuntu Plasma X11 control:

- the bar and review were native Wayland windows;
- the region selector, ring and dimming came from the helper's layer-shell surfaces
  on Plasma and from the put.gd Shell extension on GNOME;
- `overlayPlacement` matched [recording](../../docs/recording.md) for each compositor;
- every clip scored far above a frozen-pattern control in MP4, WebM, GIF and WebP.

The image's own `putgd-record` diagnostics (`--motion`, `--preview`,
`--av-sync`, `--av-marker`) read its working clips and saved MP4 (H.264), WebM
and GIF copies; run them from the extracted image with `APPDIR` set
([recording](../../docs/recording.md), "Lab diagnostics"). H.265 MP4 and WebP
copies still need a distro build (and Pillow for WebP motion). The components
they need are the `mov` and `gif` demuxers, the Opus, GIF, MJPEG and PCM
decoders, libvpx's VP9 decoder through FFmpeg's `libvpx_vp9` wrapper, and the
MJPEG and PCM encoders. They added 303,104 bytes to the FFmpeg libraries'
SquashFS (zstd, 16 KiB blocks) and 7 s to FFmpeg's build at `-j3`; FFmpeg's own
VP9 decoder would have added 1.14 MiB, and its HEVC decoder another 0.34 MiB.
After assembly, `build-appimage.sh` extracts the image and runs
`tools/releases/appimage_diagnostics.py`, which fails the build unless the
helper's marker clip and its MP4, WebM and GIF exports each pass `--motion`
and `--preview`, and the ones with sound `--av-sync` (written to
`appimage-diagnostics.json` next to the receipt).

The post-deployment audit runs the relocated `putgd-record --capabilities`
with `APPDIR` set and no display. It fails unless GStreamer initialises, every
`requiredElements` entry loads, every plugin comes from `usr/lib/gstreamer-1.0`,
FFmpeg's H.264 decoder opens, MP4, WebM, GIF and WebP export are available,
the bundled `x264enc` encodes, the bundled libx264 and libx265 open, and no
host-matched library was bundled. A builder without a GPU (CI) has only those
fallbacks, so the audit proves them; the host's VA-API and NVENC encoders are
exercised on the lab desktops. The receipt keeps the result under
`postDeploymentAudit.recording` and the media list, fallbacks, FFmpeg pin and
GPL components under `recordingRuntime`.

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

## Update information

Tagged images embed `zsync|https://app.put.gd/dl/<channel>/appimage.zsync` with
`appimagetool -u`, `nightly` for nightly tags and `stable` for candidates and
stable releases; untagged builds embed nothing ([AppImage updates](../../docs/release.md#appimage-updates)).
`tools/releases/appimage_update.py` derives both values from the tag.
After appimagetool, `portable_appimage.py` reads the section back, writes
`putgd-VERSION-x86_64.AppImage.zsync` with
`zsyncmake -u https://github.com/OWNER/REPO/releases/download/TAG/putgd-VERSION-x86_64.AppImage`,
and checks its header (`Filename`, `URL`, `Length`, `SHA-1`) and block count
against the image. appimagetool's own `.zsync`, which names the image by a
relative URL, is replaced.

zsyncmake is the static zsync 0.6.2 that the pinned appimagetool 1.9.1 bundles.
`tools.lock.json` (`zsyncmake`) records its path in that image and its digest;
the builder extracts it with `--appimage-extract` and checks the digest before
running it. zsync2's `zsyncmake2` (2.0.0-alpha-1 build 93) is not used: it drops
a file's trailing partial block, so its `Length` and `SHA-1` describe a
truncated image (6,000,000 bytes recorded as 5,998,592).

For a lab update test, `PUTGD_APPIMAGE_UPDATE_ORIGIN=http://127.0.0.1:8765` in
the environment of `tools/releases/build-appimage.sh` points both the update
information (`ORIGIN/appimage.zsync`) and the `.zsync`'s image URL
(`ORIGIN/putgd-VERSION-x86_64.AppImage`) at a local server; `collect.py` rejects
such an image. The recorded check moves one build to the next with
`appimageupdatetool`:
[AppImage zsync updates (issue #293)](../../docs/evidence/2026-10-20-appimage-zsync-293/README.md).

The AppDir root holds exactly one desktop entry, `putgd.desktop`, as the AppImage
spec requires. A linuxdeploy pass without `--desktop-file` links the first entry it
finds in `usr/share/applications`, so the NoDisplay `putgd-ui.desktop` could join it,
and AppImageLauncher (libappimage) then refuses to integrate the image (#306).
After the last pass `portable_appimage.py` removes such links (the entries stay in
`usr/share/applications`) and rejects a root without exactly one `putgd.desktop`
that sets neither `NoDisplay` nor `Hidden`. The receipt records it under
`postDeploymentAudit.rootDesktopEntry`.

### Start at login after an update

Updaters that follow the `.zsync` write the new version under its versioned
`Filename:` beside the old image (AppImageLauncher keeps the old one,
`appimageupdatetool --remove-old` deletes it), so an autostart entry naming the
old file would start the old version, or nothing (#307). From an AppImage, launch
at login therefore runs `/bin/sh <data>/putgd/login/appimage-start.sh --background`
(`src/app/appimage-login-start.sh`, written by `LoginStartup` with mode 0700 and
rewritten at every start, so the running image becomes the configured one). At
login the script starts the newest `putgd-VERSION-x86_64[_md5].AppImage` in the
configured image's folder, ordered by version (`sort -V`, prereleases before their
release), that is the user's own file, executable, writable by nobody else, has the
AppImage magic and prints the same update information as the configured image
(`--appimage-updateinformation`). Group write counts as private only for the user's
own group (named after the user, no other members), which umask 002 gives every file
on Debian, Ubuntu, Mint and Fedora. The folder must pass the same check, or only the
configured image is used. The configured image itself only has to be an executable file,
as the autostart entry required before. Without update information
(untagged builds) the configured image is the only candidate. Gear Lever replaces
the configured file in place; with no versioned sibling the script starts it
without probing anything. put.gd touches only its own autostart entry and script,
never another program's entries or images.

## Runtime behavior

On Wayland sessions the views, the editor and the KDE region helper are native
Wayland clients, as with the DEB/RPM; set `QT_QPA_PLATFORM=xcb` to use XWayland
instead. KWin draws their frames; on GNOME, Qt draws adwaita frames for the
views and bradient for the editor, which clears the desktop identity before it
starts (as in the DEB/RPM). The region helper sets LayerShellQt's overlay role
per window and needs the Wayland platform, so KDE region capture works only
natively. Before the image carried `xdg-shell` it answered `region_unavailable`.
A Wayland compositor takes a window's icon from the desktop entry named by its
app ID (`putgd-ui` for the views and the editor, `putgd-record` for the
recorder's bar and review), not from the window, and Qt 6.8 has no
`xdg-toplevel-icon` support. So in a Wayland session AppRun writes
`putgd-ui.desktop` and `putgd-record.desktop` into the user's applications
directory while the image runs: NoDisplay entries with `StartupWMClass` set to
the app ID, `Icon` naming the mounted
`usr/share/icons/hicolor/scalable/apps/putgd.svg` (the build rejects an image
without it) and `Exec` naming the persistent `APPIMAGE`. The file name must be
the app ID: KWin looks the entry up by name only, so `StartupWMClass` in an
integrated install's own entry (AppImageLauncher, Gear Lever) would restore
GNOME's icon but not KWin's. AppRun skips an ID the host already has, in
`XDG_DATA_HOME` or `XDG_DATA_DIRS` (a DEB/RPM install, a user's own entry, or
another running launch), and publishes each entry with `ln`, so it never
replaces one. On X11 the windows carry their own icon and nothing is written.
Acceptance:
[2026-10-19 native Wayland](../../docs/evidence/2026-10-19-appimage-native-wayland.md),
[2026-10-19 window icons](../../docs/evidence/2026-10-19-appimage-wayland-icons.md).

The application AppRun is installed after deployment, so generic deployment
hooks cannot override its host-tool environment or force unavailable GTK2 themes.
AppRun resolves sibling helpers inside the mounted image. Login startup names
the persistent `APPIMAGE` path, through the login script above, rather than a
temporary `/tmp/.mount_*` path.
KWin authorizes screenshots against the resident executable's canonical path.
Host permission-cache calls retain the original host library/plugin environment,
and the child retains the caller's umask. On KDE, AppRun uses `mktemp` with mode 0600 to create a unique NoDisplay host
permission desktop entry for the current mount, invokes distro `kbuildsycoca6`,
and removes only that entry on normal exit/termination. Existing user desktop
entries are never rewritten. The resident applies the same executable and
restricted-interface checks to these entries. KWin's own permission check stays
enabled. A missing/rejected host registration is an explicit startup failure.

Every entry AppRun writes carries `X-PutGD-AppImage-Owner`, the PID and start
time of the launch that wrote it. On exit or termination AppRun removes the
entries it wrote (and on Plasma rebuilds the KDE cache), so nothing remains once
the image stops, and nothing is left to remove when the file is deleted. A
killed wrapper cannot clean up; its entries name an unmounted executable and
cannot grant access to a later mount, and the next launch in a Plasma or
Wayland session removes every entry whose owner no longer runs. Entries without
the key (a user's own, or ones written by older images) are never touched.

AppRun uses a lightweight shell supervisor to keep these entries alive; measure
this process as part of the resident family on KDE and on Wayland. Neither this wrapper
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
