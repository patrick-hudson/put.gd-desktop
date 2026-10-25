# Third-party components

The application uses dynamically linked distro Qt 6, QtKeychain, XCB, libxkbcommon,
Wayland, OpenSSL and libcurl. Platform plugins and the SQLite SQL driver remain
distribution packages. Their exact installed versions and package identities belong
in each acceptance manifest; their source and license records come from that distro.
No Qt or compiler runtime is privately bundled by the Linux package.

The package privately includes the official AWS SDK for C++ and its CRT shared
libraries. `cmake/dependency-lock.json` identifies SDK 1.11.907 and all 15 pinned
submodules. `tools/verify_dependencies.py` checks the actual source tree. Packaging
copies the license texts from those exact sources, including the CRT components
and s2n-tls, into the installed `share/doc/putgd` directory. The runtime manifest
records the packaged shared-library SHA256 values. See `docs/build.md` for rebuilding
these dependencies with their normal CMake builds.

The vendored `ext-data-control-v1.xml` retains the original copyright and permission
text for Simon Ser, Ivan Molodetskikh and Neal Gompa. Its generated client bindings
are built by the distro `wayland-scanner`. The complete protocol and notice are also
installed as `share/doc/putgd/wayland-ext-data-control-v1.xml`. The vendored
`wlr-data-control-unstable-v1.xml`, used on compositors that predate ext (for
example KWin 6.3), likewise retains its Simon Ser and Ivan Molodetskikh notice and
is installed as `share/doc/putgd/wayland-wlr-data-control-unstable-v1.xml`.

The GNOME integration imports the installed Shell/GJS/Mutter APIs; it does not bundle
a replacement GNOME runtime. The KDE integration links the distro LayerShellQt
library when the supported Qt version and development package are available.

The optional MCP helper is distributed separately from the native application.
`tools/mcp/uv.lock` identifies its Python dependencies, including the official MCP
SDK and jsonschema. Preserve their upstream notices when packaging that helper.

The AppImage is different: it bundles the official Qt 6.12.0 binaries with their
ICU, QtKeychain, LayerShellQt, a pinned FFmpeg, the AWS SDK, the Proton helper and
Ubuntu 24.04 libraries. Its notices are generated from the image's contents when it
is built (`tools/releases/appimage_notices.py`, `packaging/appimage/components.json`):
`share/doc/putgd/runtime-notices.txt` and `bundled-components.json` in the image list
every bundled component with its version, licence, source and files, and
`share/doc/putgd/bundled-licenses` holds their licence texts.

This inventory records build and package inputs. Actual installed-package notice
inspection, lifecycle checks and the complete release matrix remain release gates.
