# put.gd desktop

A Qt/C++ desktop sharing app being built for Linux. The planned workflow captures a
region, focused window, or full desktop, or uploads clipboard files/images/text to S3
and copies a public link. Settings, local archives, upload history, remote deletion,
CLI, and MCP use shared services. Additional storage providers fit the same contract.

Implementation is in progress. The shared operation journal, JSON settings, local
archives, indexed history, private previews, and resident JSON CLI are implemented
and tested with synthetic sources/providers. Production capture and S3 upload are
still being implemented. Cinnamon/X11, GNOME Wayland, and Plasma Wayland remain
planned targets pending real graphical acceptance tests.

See [build instructions](docs/build.md) and [implementation issue](https://github.com/patrick-hudson/put.gd-desktop/issues/1).
The [CLI](docs/cli.md), [configuration](docs/configuration.md),
[shared architecture](docs/architecture.md), and [verification record](docs/verification.md)
describe the current interfaces and observed checks.
