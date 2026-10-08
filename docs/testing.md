# Testing

Build and run repository tests with `cmake --preset dev`, `cmake --build --preset dev`,
and `ctest --preset dev`. See [build.md](build.md) for pinned SDK provisioning and
sanitizer builds. Qt offscreen tests prove core/widget contracts; isolated Xvfb checks
prove X11 protocol behavior. Neither establishes real Cinnamon or Wayland support.

Run host lab contracts with `python3 tests/desktop/test_lab.py`. These cover private
QEMU command construction, QMP protocol/event handling through an actual UNIX socket,
native session rejection, TCG evidence classification, locked ISO identity, independent
pixel/paste comparisons, and actual/public byte hash/URL-paste mismatch rejection.
The socket peer is synthetic and is expressly not compositor evidence.

Use [vm-lab.md](vm-lab.md) to provision a golden Cinnamon guest, install the development
DEB and execute a named scenario. The single command `python3 tools/lab/lab.py run
--run RUN --scenario region` emits machine-readable results/JUnit and inspectable
synthetic evidence; use the other named capture/clipboard scenarios and configured
S3/history options as documented. Every graphical pass needs actual saved/public bytes
and independent native paste, plus external QMP witnesses. Never call a profile supported
from portal discovery, offscreen mocks or uploader-window screenshots.

Physical Cinnamon acceptance and the GNOME/Plasma native Wayland/helper matrix are still
required. Full U6 completion additionally needs two independent golden-image runs,
wrong-result and timeout cleanup witness evidence, and the required multihead layout
cases. U9 requires a fresh-agent walkthrough; U10 owns release-package lifecycle proof.
Keep credentials out of commands, evidence and golden images. Public service tests use
synthetic objects in an isolated destination and never alter bucket controls or user
objects. Record actual operation/history IDs and deletion effects before confirmation.
