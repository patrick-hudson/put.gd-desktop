# put.gd desktop

A Qt/C++ sharing app for Linux: capture a region, active window or desktop, or
upload clipboard files, images and text. Configurable global hotkeys upload to
S3 and copy the public URL. Settings, local archives, naming rules, upload history,
remote deletion, CLI and optional MCP use shared services. The storage interface
supports adding other providers.

The Linux implementation includes Cinnamon/X11, GNOME Shell 50 and KDE Plasma
Wayland integrations. Recorded source114 live-S3 acceptance covers all three
with exact remote cleanup and quiet resident-family memory below 40 MiB.
[Verification](docs/verification.md) distinguishes that evidence from remaining
full release/lifecycle scenarios and from new package-format acceptance.

Linux prerelease packaging produces distro-specific DEBs, a Fedora RPM and an
AppImage with compiled source identities, dependency inventories and checksums.
See [release instructions](docs/release.md) and [versioning](docs/versioning.md).
Flatpak's host capture integration is tracked separately in
[issue #14](https://github.com/patrick-hudson/put.gd-desktop/issues/14).

Build with C++20, Qt 6, CMake presets, Ninja and the pinned official AWS C++ SDK.
Start with [build instructions](docs/build.md) and the
[implementation contract](https://github.com/patrick-hudson/put.gd-desktop/issues/1).
The [CLI](docs/cli.md), [configuration](docs/configuration.md),
[architecture](docs/architecture.md), [agent guide](docs/agents.md), and
[CI coverage](docs/ci.md) describe the interfaces and reproducible checks.

The application is [MIT licensed](LICENSE); bundled dependencies retain their
own licenses and notices.
