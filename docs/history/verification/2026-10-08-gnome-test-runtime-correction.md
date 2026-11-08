# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

### GNOME test runtime correction — 2026-10-08

The earlier source-41/source-43 suites used a Node-based isolated extension
ownership fixture on the host. It was absent in the guests and did not establish
GNOME Shell acceptance. The user rejected that guest dependency. The fixture now
uses GNOME's existing GJS interpreter, without Node or JavaScript packages, and
runs where that native runtime is already installed. The shared C++/Qt tests
remain portable; real GNOME capture/clipboard tests remain separate from these
mocked ownership assertions. Do not install Node to fill a guest coverage gap.
All five ownership assertions pass under the guest’s existing GJS 1.88.0.
Host CMake configuration passes without Node lookup. [The receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-08-gjs-ownership.json)
keeps this result separate from the frozen source-43 native build.
