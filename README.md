# put.gd desktop

A Qt/C++ desktop sharing app being built for Linux. The planned workflow captures a
region, focused window, or full desktop, or uploads clipboard files/images/text to S3
and copies a public link. Settings, local archives, upload history, remote deletion,
CLI, and MCP use shared services. Additional storage providers fit the same contract.

Implementation is in progress. The initial source is a build skeleton; capture and upload
features are not available yet. Cinnamon/X11, GNOME Wayland, and Plasma Wayland are
planned targets, pending real graphical acceptance tests.

See [build instructions](docs/build.md) and [implementation issue](https://github.com/patrick-hudson/put.gd-desktop/issues/1).
