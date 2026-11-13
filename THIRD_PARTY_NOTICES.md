# Third-party components

The application uses dynamically linked distro Qt 6, QtKeychain, XCB, libxkbcommon,
Wayland, OpenSSL and libcurl. Platform plugins and the SQLite SQL driver remain
distribution packages. Their exact installed versions and package identities belong
in each acceptance manifest; their source and license records come from that distro.
No Qt or compiler runtime is privately bundled by the Linux package.

The interface and recording helper embed unmodified IBM Plex Sans Regular,
Medium and SemiBold and IBM Plex Mono Regular, copyright IBM Corp., Reserved
Font Name Plex, under SIL OFL 1.1. The sources and original per-family licences
are in `src/ui/fonts`; packages install `OFL-sans.txt` and `OFL-mono.txt` beside
their runtime notices.

The package privately includes the official AWS SDK for C++ and its CRT shared
libraries. `cmake/dependency-lock.json` identifies SDK 1.11.907 and all 15 pinned
submodules. `tools/verify_dependencies.py` checks the actual source tree. Packaging
copies the license texts from those exact sources, including the CRT components
and s2n-tls, into the installed `share/doc/putgd` directory. The runtime manifest
records the packaged shared-library SHA256 values. See `docs/release/build.md` for rebuilding
these dependencies with their normal CMake builds.

The vendored `ext-data-control-v1.xml` retains the original copyright and permission
text for Simon Ser, Ivan Molodetskikh and Neal Gompa. Its generated client bindings
are built by the distro `wayland-scanner`. The complete protocol and notice are also
installed as `share/doc/putgd/wayland-ext-data-control-v1.xml`. The vendored
`wlr-data-control-unstable-v1.xml`, used on compositors that predate ext (for
example KWin 6.3), likewise retains its Simon Ser and Ivan Molodetskikh notice and
is installed as `share/doc/putgd/wayland-wlr-data-control-unstable-v1.xml`.
The capture and window-list protocols used on wlroots and Mir compositors,
`ext-image-copy-capture-v1.xml`, `ext-image-capture-source-v1.xml` and
`ext-foreign-toplevel-list-v1.xml` from wayland-protocols 1.45 and
`wlr-foreign-toplevel-management-unstable-v1.xml` from wlr-protocols, are vendored
unmodified with their copyright and permission text and installed beside it as
`share/doc/putgd/wayland-<protocol>.xml`. The Hyprland protocols
`hyprland-global-shortcuts-v1.xml` and `hyprland-toplevel-mapping-v1.xml` from
hyprwm/hyprland-protocols (commit `cd1aba6bd78775825eb230b3a4274c48b6705eb7`,
BSD-3-Clause; Copyright 2022 Vaxry and 2025 WhySoBad respectively) are vendored the same way,
unmodified with their copyright and license text, and installed as
`share/doc/putgd/wayland-hyprland-global-shortcuts-v1.xml` and
`share/doc/putgd/wayland-hyprland-toplevel-mapping-v1.xml`. `wlr-layer-shell-unstable-v1.xml`
from wlr-protocols (commit `b010a03648b88d143236de193bddbfea0c08bc84`, Copyright 2017 Drew
DeVault) is vendored unmodified the same way and installed as
`share/doc/putgd/wayland-wlr-layer-shell-unstable-v1.xml`. The two protocols a scrolling
capture sends its wheel steps through are vendored the same way:
`wlr-virtual-pointer-unstable-v1.xml` from wlr-protocols (last changed in commit
`c11408942e2fb54d41dadb84cdf844331076ae11`, Copyright 2019 Josef Gajdusek, MIT-style), installed as
`share/doc/putgd/wayland-wlr-virtual-pointer-unstable-v1.xml`, and KWin's `fake-input.xml` from
plasma-wayland-protocols 1.17.0 (Copyright 2015 Martin Gräßlin, LGPL-2.1-or-later), installed as
`share/doc/putgd/wayland-fake-input.xml`. The native packages and the AppImage also ship the
LGPL 2.1 text for that file as `share/doc/putgd/LGPL-2.1.txt`.

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

Nayuki's QR Code generator v1.8.0 is vendored unmodified as
`src/core/qrcodegen/qrcodegen.cpp` and `qrcodegen.hpp`, from tag commit
`720f62bddb7226106071d4728c292cb1df519ceb`, under the MIT licence.
`cmake/dependency-lock.json` pins the commit and both file hashes;
`tools/verify_dependencies.py` verifies the actual files. Every package retains
Project Nayuki's full copyright and permission notice in `runtime-notices.txt`
and `qrcodegen-LICENSE`; AppImage notices and every package's SBOM identify it.
