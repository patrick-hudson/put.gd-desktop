# Repository channel at startup (#491)

Issue: [#491](https://github.com/patrick-hudson/put.gd-desktop/issues/491).
Code: [#563](https://github.com/patrick-hudson/put.gd-desktop/pull/563).
This documentation PR follows the code PR; neither is a release approval.

## Change and stored settings

`UpdateService::start()` reads the existing local repository parser before the
first UI snapshot. Repository ownership and action are separate from cached feed
results. Check start and completion refresh local ownership even on a network
failure. The resident still launches the existing CLI child for network access;
this change adds no Qt Network or TLS dependency to it.

A supported DEB/RPM follows a recognized enabled repository's Stable/Nightly
channel. Missing, disabled, foreign or unparseable files fall back to the stored
manual choice (or the build channel). Existing handling of custom repository
suites remains unchanged: a suite that names neither supported channel does not
select a feed channel. AppImage and unsupported builds retain their own rules.

Before this fix, changing the initial dropdown could write `stable`, `nightly`,
or `build` into `preferences.updates.channel`. Later repository detection only
changed the displayed/effective channel: it did not undo that saved choice.
The startup fix prevents the transient editable dropdown, without migrating or
resetting existing preferences that may intentionally serve a manual install.
Regression coverage checks an unrelated deferred settings save as well as
editable unmanaged startup.

## Verification scope

Exact source: `29496f5871820a4caaaf5d687c35db5a02c8b4f8`, tree
`c8d83f965c9e8314111665516497784671206f89`, clean Git build
`26.1025.1256-dev+g29496f587182` on both hosts. Base was nightly
`80250d2559606e7fa739e789dcffb03c96f9c53b`.

| Host | Build/toolkit | Result |
| --- | --- | --- |
| f44, Fedora 44 | GCC 16.2.1, distro Qt 6.11.2, `dev`, target `fedora44` | All five focused CTest suites passed in 112.52 s; Settings suite 109.57 s |
| d69, Debian 13 | GCC 14.2, distro Qt 6.8.2, `dev`, target `debian13` | All five focused CTest suites passed in 99.39 s; Settings suite 95.35 s |
| f44 MCP adapter | Locked official SDK, production CLI, synthetic session peers | 5 tests passed in 22.621 s |
| d69 physical Plasma 6.3.6 / native Wayland, session `20103` | The exact `test_gui_settings` binary, **synthetic supported build metadata** | Four startup rows passed with saved, reviewed VNC visuals |
| f44 physical GNOME 50.6 / Wayland, session `1100` | Cover preflight only | Blocked; Settings was never launched for a native check |

The five suites are `update_feed_contract`, `update_check_request`,
`update_service_schedule`, `update_system_repository`, and `gui_settings`.
The startup matrix adds 100 combinations across five package targets, five local
repository states, two stored preferences and checks on/off. Separate cases cover
stale persisted ownership and failed-network refresh. The GUI test dispatches the
production `update.status` endpoint and compares it to the service used by the
first snapshot. MCP's existing contract verifies forwarding to that same endpoint.
This was a targeted remote build/test run, not the full local CTest or full
acceptance suite. GitHub CI for the exact code head subsequently passed its
required `CI` summary, including both Linux jobs, ASan/UBSan, Windows compile and
contracts. Packages and PipeWire jobs were skipped, not counted as passes.
`ldd` on both built residents listed neither Qt Network nor libssl; this is a
linkage check, not a resident memory measurement.

[Machine-readable identities, test results, hashes and cleanup](summary.json)
include product and test binary SHA-256 values. For example the native d69 widget
binary was `aa22831c5a26a4ce9404d5fb11b0bdebc63f90795e0e779a417a821fbff88255`.

## Saved native widget visuals

These four images are **Qt regression fixtures on the real d69 desktop**, not an
installed DEB/RPM or release acceptance. The lower-left footer carries the actual
development identity; the Updates status deliberately carries the fixture's
`26.1022.968-nightly` identity supplied through the existing unit-test seam.
The real service reads a temporary local repository file before the first
snapshot. No feed request runs. All settings and repository fixtures are private;
the system repository and user's settings are untouched.

- [Fresh repository profile](pictures/fresh-repository.png): Stable is disabled,
  the source file is named, and status is “Not checked yet”.
- [Repository with stored Nightly preference](pictures/repository-with-manual-choice.png):
  Stable remains system-controlled; the stored Nightly preference survives an
  unrelated deferred save.
- [Fresh manual profile](pictures/fresh-manual.png): Nightly is editable and no
  repository source/instructions are shown.
- [Manual profile with stored Stable preference](pictures/manual-with-choice.png):
  the preference is editable and remains intact during unrelated saves.

[Original desktop-run receipt](d69-native-widget.json) retains process IDs,
preflight, native session environment, screenshot hashes, exits and cleanup.
The driver captured through the physical desktop's loopback VNC listener with
its existing wall tunnel intact. Before retaining pixels it required at least
98% magenta cover in the capture region; while the owned test window was open,
it required at least 50% cover and 90% at every edge. It saved only the verified
`[0,64,1920,952]` crop. Receipt `size` fields describe the original 1920×1080 VNC
frame; the PNG files themselves are 1920×952. All four saved settled images were
visually inspected. No unrelated window or credential field is present.

## Failed preflights and cleanup

Fedora's first cover sample was 0%; a bounded paint wait then reached only
95.929% whole-frame cover. A final cropped attempt reached 97.070%, still below
98%. The gate refused every attempt before Settings launch, retained no pixels,
and the driver stopped its cover process. The reason for the uncovered area was
not established. These are blocked visual preflights, not passing GNOME evidence:
[first attempt](f44-preflight-1.json), [paint-wait attempt](f44-preflight-2.json),
[cropped attempt](f44-preflight-3.json). Thresholds were never lowered.

Initial Fedora configuration attempts lacked the explicit existing SDK runtime
and source paths; corrected configuration used the host's pinned official SDK.
The initial all-target build was deliberately interrupted to keep shared host
work bounded, then the required targets were built successfully. The Debian
metadata-only `plasmashell --version` probe aborted without a display; versions
were read through `dpkg-query` instead. These original logs remain in the private
archives; none was counted as a successful build/test or graphical result.

Both locks passed their cleanup checks with zero newly left items and no
`--not-mine` exemptions: f44 `codex-491-f44` released at
`2026-10-25T04:23:31Z`; d69 `codex-491-d69` at `2026-10-25T04:24:17Z`.
The next #561 thread received direct handoffs. No packages, extensions or session
configuration were installed/changed; no logout or new tunnel was needed.
Pre-existing work and processes were preserved. m74's performance lock was not
touched. No VM was launched, so no overlay retirement was required.

## Reproduction and retained evidence

Source/build roots on both builders: `/home/patrick/work/i491`. Commands used
`cmake --preset dev`, `nice -n 10 cmake --build --preset dev --target ...` with
its bounded two-job preset, and:

```sh
ctest --preset dev -R '^(update_feed_contract|update_check_request|update_service_schedule|update_system_repository|gui_settings)$'
PUTGD_CLI="$PWD/build/dev/putgd-cli" uv run --project tools/mcp --locked python -m unittest discover -s tests/integration -p test_mcp.py -v
```

The build targets were `test_update_service test_update_check test_system_updates
test_update test_gui_settings putgd putgd-cli putgd-ui`. Both used the already
provisioned SDK at `/home/patrick/work/put.gd/build/sdk` and its pinned source at
`/home/patrick/work/put.gd/build/dependencies/aws-sdk-cpp`; the explicit runtime
path was `sdk/lib64` on Fedora and `sdk/lib` on Debian. Distro Qt remained dynamic.
All isolated test/runtime paths were new and private, with socket preflights
before process launch. Native fixture socket paths were 42 UTF-8 bytes. The MCP
suite also asserts each actual temporary socket length before listening.

Private raw evidence and reproducer versions remain under this code worktree's
`work/491/`, with SHA-256 values in `summary.json`. The exported archives were
verified against the remote files before extraction. Remote copies remain at
`/home/patrick/work/i491-evidence-f44.tgz`,
`/home/patrick/work/i491-evidence-f44-v2.tgz` and
`/home/patrick/work/i491-evidence-d69.tgz`; their source logs remain readable under
`work/491` on each host. The v2 Fedora archive adds source/linkage/cleanup metadata
without deleting the original archive. The private `native-widget*.py` drivers
are retained with hashes, as are their failed preflights. No test source or
unique failed result was deleted.

## Blocked acceptance gates

Supported update checking requires an exact tagged release build. This branch
is untagged; `Update::build()` intentionally reports `development_build` for its
native package targets. The task forbids manual `v*` tags, merging, and release
publication. No tag, manifest override, production bypass, or release was made.
Native supported DEB/RPM acceptance on the fixed exact release artifact therefore
remains blocked until an authorized release artifact exists. A Qt regression
fixture's explicit supported build identity is synthetic and is not package or
release acceptance.

The complete Mint/Ubuntu/Kubuntu/Fedora/Debian and native X11/Wayland matrix,
release functional/memory gates, and a fresh-agent walkthrough are not established
by focused widget/core tests. Offscreen results are not desktop proof. These
limits must remain visible even if CI passes.
