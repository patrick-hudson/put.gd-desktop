# Repository channel at startup (#491)

Issue: [#491](https://github.com/patrick-hudson/put.gd-desktop/issues/491).
Code: [#563](https://github.com/patrick-hudson/put.gd-desktop/pull/563).
Code #563 merged into nightly as `ef4e567a32faa149f0f44d9a02c97343082ae5db`
on 2026-10-25 at 09:55:30Z, guarded against final head `7743cc4b`.
This documentation PR follows it; neither is a release approval.

## Final source acceptance: 7743cc4b

The review correction and targeted native acceptance are complete on exact source
`7743cc4bb9186db69bfd6935569de979d20b8c28`, tree
`d227590f429e3b71c460f90eb01025aff108f250`. Patrick authorized the local-only
`v26.1025.1257-nightly` tag on this commit in isolated builders. Clean Release
builds produced `26.1025.1257-nightly`; every package passed the native audit.
The tag and packages were never pushed or published. Earlier evidence below
remains attributed to `29496f5` and is not substituted for this final campaign.

Greptile found a valid race on the predecessor: removing or disabling a Nightly
repository during a check could announce the late Nightly result under Stable
and mark it seen there. The final guard requires the result channel to match the
current channel before notification or seen-state tracking. Eight deterministic
in-flight cases (DEB/RPM × removed/disabled × background/manual) all fail before
the guard and pass afterward. Clean final-source development builds
`26.1025.1257-dev+g7743cc4bb918` passed all five focused CTest suites on Debian
(96.56 s) and Fedora (106.04 s). [Exact logs](review-follow-up/) are retained.
Final-head CI passed Linux 24.04/26.04, ASan/UBSan, Windows compile, contracts and
the required CI summary. Path-skipped jobs are not credited. Greptile's final
check succeeded; Patrick explicitly confirmed the review as 5/5 and authorized
normal protected-branch merging without waiting for a rendered score comment.

[Final machine-readable evidence](final-7743cc4b/summary.json) records source,
package and binary identities, actual session/platform witnesses, first CLI
state, stored preferences, live MCP parity, picture hashes and cleanup receipts.
All graphical runs stayed visible on the wall. Every isolated socket passed its
length preflight before app launch. Actual installed `/usr/bin` executables and
system repository paths were used, with no build metadata or system-root override.

| Final package / desktop | Result and reviewed visuals |
| --- | --- |
| Fedora 44 RPM / physical GNOME Wayland | Five startup cases and live official-SDK MCP/CLI parity passed. [Managed](final-7743cc4b/f44-fresh-managed-first.png), [unmanaged](final-7743cc4b/f44-fresh-absent-first.png) |
| Debian 13 DEB / physical Plasma Wayland | Five cases passed. [Managed](final-7743cc4b/d69-fresh-managed-first.png), [unmanaged](final-7743cc4b/d69-fresh-absent-first.png) |
| Ubuntu 26 DEB / physical Kubuntu Plasma Wayland | Five cases passed. [Managed](final-7743cc4b/k72-fresh-managed-first.png), [unmanaged](final-7743cc4b/k72-fresh-absent-first.png) |
| Ubuntu 24 DEB / physical Mint Cinnamon X11 | Five CLI cases passed; independent manual visual retry passed. [Managed](final-7743cc4b/m74-fresh-managed-first.png), [unmanaged retry](final-7743cc4b/m74-fresh-absent-first.png) |
| Ubuntu 26 DEB / Ubuntu GNOME Wayland VM on d69 | Fresh managed and stored-choice restart passed. [Managed](final-7743cc4b/gnome-fresh-managed.png), [stored choice](final-7743cc4b/gnome-managed-stored-stable.png) |
| Ubuntu 26 DEB / Kubuntu KDE X11 VM on d69 | Fresh managed and stored-choice restart passed. [Managed](final-7743cc4b/kde-fresh-managed.png), [stored choice](final-7743cc4b/kde-managed-stored-stable.png) |
| Ubuntu 24 DEB / Ubuntu GNOME X11 VM on f44 | Fresh managed and stored-choice restart passed. [Managed](final-7743cc4b/u24-fresh-managed.png), [stored choice](final-7743cc4b/u24-managed-stored-stable.png) |

The five physical cases are fresh managed, managed with saved Stable and checks
off, absent, invalid, and disabled repository configuration. Initial update state
has no attempted check or result; repository-managed installs immediately show
Nightly and preserve the stored manual choice. Unmanaged installs allow that
choice. Opening Settings does not change configuration. The live MCP adapter on
Fedora reports the same post-patch state as the production CLI in every case.
The seven rows prove targeted startup behavior, not full release functional,
performance, memory or fresh-agent acceptance.

| Native package target | SHA-256 |
| --- | --- |
| Fedora 44 RPM | `7828c74debe49ced2fb7dd792d025fe347e80d6067d73231969a88362f65fbdf` |
| Debian 13 DEB | `85e3ec6d0c181b28d894e33e00a04162bea11e805d7f7e47858578e86c1006ad` |
| Ubuntu 26.04 DEB | `4feb3f55a18eea30b06575068cd4f6b6bb0edd04fbbf2bf8b4d6d00a6a733d4b` |
| Ubuntu 24.04 DEB | `5d918d1351dbf9e705bfc4960bbfb8b20ce521c8757346e2a45977f0f7b38221` |

Fedora, Debian and physical Kubuntu use app-only scale `.7`; Mint and all three
VMs use default scale `1`. The physical privacy crops are respectively
`[0,180,1920,900]`, `[0,0,1540,1000]`, `[0,64,1920,952]` and
`[0,0,1920,1000]`. Admission remains at least 98% cover; saved frames require at
least 50% cover and 90% at every edge. These are bounded functional captures.
Only owned PID windows were positioned. No physical session was logged out or
its desktop settings changed.

Mint run `native-20261025T094214Z` passed the CLI cases and its managed visual,
but its [unmanaged frame](final-7743cc4b/m74-rejected-incomplete-manual.png) has
incomplete sidebar paint and is **not credited**. Fresh independent default-scale
run `native-20261025T094603Z` supplies the reviewed complete manual image. Both
attempts and driver versions are retained. The Ubuntu24 wrapper initially refused
Btrfs's dynamic-inode `f_favail=0`; it was corrected to use the repository's existing
`storage_preflight` classification, with lab launch guards unchanged. Ubuntu26's
first old-package purge returned exit 100 without a retained detailed error;
a logged retry with `DPkg::Lock::Timeout=60` removed it and installed the final
package successfully. Its owned guest was resized from 1280×800 to 1920×1080 using
`lab.py layout` before app acceptance. No failed attempt is counted as a pass.

The Ubuntu24 broad `loginctl` probe selected its SSH session (`Type=tty`), not
the graphical login. The receipt preserves that limitation. Its immutable
visibly verified GNOME/X11 golden disables Wayland in GDM; the final app launched
from GNOME Shell’s environment and reported the production X11 adapter with
`libqxcb.so` loaded. The earlier golden identity is supporting OS/session
provisioning evidence, not a substitute for the final app checks.

All physical packages and the Mint temporary GTK4 introspection dependency were
removed, and original absent repository state restored. Checked-clean releases:
f44 `09:42:54Z`, d69 `09:42:55Z`, k72 `09:42:57Z`, m74 final retry `09:47:11Z`.
Fresh independent overlays `k491r`, `u24491r` and `g491r` were stopped after their
saved visuals were reviewed; completion evidence was exported and hash-verified
before guarded retirement at `09:47:12Z`, `09:51:40Z` and `09:52:19Z` respectively.
Their wall tiles and tunnels were removed. Immutable goldens and unrelated
projects/processes were preserved. Raw receipts and build/install logs remain in
private `work/491/final/` archives, indexed by checksum in the final summary.

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

## Initial development verification

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
The next #561 thread received direct handoffs. During this initial fixture campaign no packages, extensions or session
configuration were installed/changed; no logout or new tunnel was needed.
Pre-existing work and processes were preserved. m74's performance lock was not
touched. No VM was launched during the initial fixture campaign.

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

## Installed native package verification

Patrick explicitly authorized the local-only test tag `v26.1025.1256-nightly`
on exact commit `29496f5871820a4caaaf5d687c35db5a02c8b4f8` in isolated builder
checkouts. Those checkouts have their push URL disabled. The tag was never pushed;
no release or package was published. Standard Release builds and
`tools/releases/native.py` produced and audited the actual DEB/RPM files, with
production update support enabled by normal versioning. There is no synthetic
build metadata or system-root override in these package checks.

The [machine-readable campaign](summary.json) records package checksums, manifests,
installed binary hashes, startup timing, CLI responses, settings preservation,
Qt platform mappings, socket preflights and saved-picture checksums.

| Package / desktop | Targeted result | Saved startup / unmanaged result |
| --- | --- | --- |
| Fedora 44 RPM / physical GNOME 50 Wayland | Five cases passed; real official-SDK MCP helper exactly matched production CLI state in every case | [Managed](pictures/f44-fresh-managed-first.png), [manual Stable](pictures/f44-fresh-absent-settled.png) |
| Ubuntu 26.04 DEB / physical Kubuntu Plasma Wayland | Five cases passed | [Managed](pictures/k72-fresh-managed-first.png), [manual Stable](pictures/k72-fresh-absent-settled.png) |
| Debian 13 DEB / physical Plasma Wayland | Five cases passed after corrected privacy crop | [Managed](pictures/d69-fresh-managed-first.png), [manual Stable](pictures/d69-fresh-absent-settled.png) |
| Ubuntu 24.04 DEB / physical Mint 22.3 Cinnamon X11 | Five cases passed; final first-open managed, unmanaged and restart visuals reviewed | [Managed](pictures/m74-fresh-managed-first.png), [manual startup](pictures/m74-fresh-absent-first.png), [stored choice](pictures/m74-managed-stored-stable-first.png) |
| Ubuntu 26.04 DEB / fresh Ubuntu 26.04.1 GNOME Wayland VM on d69 | Fresh managed startup and restart with saved Stable preference passed | [Fresh](pictures/ubuntu26-gnome-fresh-managed.png), [stored choice](pictures/ubuntu26-gnome-managed-stored-stable.png) |

The five physical-host cases are fresh managed, managed with saved Stable and
daily checks off, fresh absent repository, fresh invalid file and fresh disabled
repository. Each uses the actual installed system repository path and a new
private profile. All initial statuses are supported, with `attemptedAt`,
`checkedAt` and `result` null. Repository ownership immediately selects Nightly;
changing the stored manual preference to Stable leaves that effective channel
Nightly. Unmanaged cases follow the stored Stable choice. Opening Settings does
not rewrite the configuration. The managed profile is restarted to prove that
its stored choice survives startup. No network check is invoked.

Fedora, Debian and Kubuntu package runs use application-only
`QT_SCALE_FACTOR=.7`; the final Mint run uses default scale 1. The certified regions retain the 98% initial magenta admission,
50% remaining cover and 90% at every edge: f44 `[0,180,1920,900]`, k72 `[0,64,1920,952]`, m74 `[0,0,1920,1000]`, d69 `[0,0,1540,1000]`. These are bounded crop functional
checks, not whole-desktop/default-scale performance evidence. On m74, only owned
PID windows move once to `(410,130,1100,700)`. On d69 an immediately unloaded, owned-PID KWin
script places the window at `(270,150,1000,650)`.

Two additional d69 admission attempts failed (96.57% and 96.02%) before app
launch; the corrected left crop excludes a right-side obstruction. The first
Mint attempt had correct startup state but failed the screenshot edge check
because its window opened at the upper left. No refused pixels were exported.
The original attempts, scripts and diagnostics remain private; thresholds were
never lowered. Debian’s later passing run supersedes its graphical refusals. The first scaled Mint run passed numerical gates but failed visual review due to incomplete painting. A default-scale retry, `native-20261025T051010Z`, has fully readable managed, unmanaged and stored-choice first-open frames. Some later post-patch frames remain incomplete and are not credited. The CLI assertions establish the post-patch state independently; failed frames and driver versions remain private.

All test packages were removed and the original absent repository state restored.
No physical session logout, extension copy or display setting change was made.
Checked-clean releases were k72 `04:48:49Z`, f44 `04:51:37Z`, m74 final retry `05:11:13Z`,
and d69 `05:00:44Z` on 2026-10-25. The earlier refused d69 package slot also
released cleanly at `04:58:20Z`. Pre-existing processes stayed untouched.

The Ubuntu Wayland VM `g491`, run ID
`7c2c2fd9-c1cd-4162-bbec-d7d466c6a0eb`, used the immutable provisioned golden
`563730cc1d929bd946d20cf30a9aecfe3c4ad40eb12ba7f9abcff1c461e342bb`.
Its independent overlay ran with KVM, two vCPUs and 6 GiB RAM, with a separate
visible wall tunnel. Host/guest capacity passed. The first launch refused a pin
for a different port before creating a VM; the verified same-guest key was
remapped to the chosen endpoint. Evidence was exported and its manifest verified,
then guarded cleanup retired only the owned overlay at `04:52:03Z` UTC. Its tunnel and wall tile were removed; the golden is unchanged.

## Remaining scope

Native GNOME/X11 passed on Ubuntu 24.04.5 / GNOME 46 / Qt 6.4.2 in an independent KVM overlay on f44. [Fresh managed](pictures/ubuntu24-gnome-x11-fresh-managed.png) and [stored-choice restart](pictures/ubuntu24-gnome-x11-managed-stored-stable.png) show repository ownership before any check. Kubuntu 26.04.1 / Plasma 6.6 / Qt 6.10.2 native KDE/X11 also passed, with `kwin_x11`, `Type=x11` and loaded `libqxcb.so` verified. [Fresh managed](pictures/kubuntu-x11-fresh-managed.png) and [stored-choice restart](pictures/kubuntu-x11-managed-stored-stable.png) were reviewed. The targeted startup matrix is complete. Full release functional/memory
profiles and the complete fresh-agent walkthrough are not established by these
targeted startup checks. These local test packages are not published release
assets, and nothing here authorizes release publication.


## New GNOME/X11 golden and retention

Ubuntu 24.04.5 supplies the native GNOME/X11 session missing from Ubuntu 26's
GNOME profile. A new disposable guest was provisioned from the official Ubuntu
24.04 cloud image (published 2026-10-17; checksum verified against its official
SHA256SUMS), using standard distro desktop packages. The approved default lab
login was visibly verified through the masked console; protected inputs and
private keys are excluded from evidence. The resulting standalone 40 GiB golden
passed `qemu-img check` and byte-content comparison to the stopped provisioning
overlay. Its SHA-256 is
`fff0b55cc2d09d403bea61dd17d46165994cd8a9d0c2d467c5883c7fb795e245`.

Immutable copies are on f44 and d69 under
`/home/patrick/work/put.gd/work/lab-images/ubuntu24-gnome-x11-20261025-491/`.
The streamed Debian replica was checksum-verified and checked independently;
no disk image was stored on the workstation, and the workstation SSH key
remained local. After Patrick authorized local `aws login`, authentication was renewed and the
required private S3 backup completed. The encrypted Glacier Instant Retrieval
object and Standard manifest are under
`linux/ubuntu24-gnome-x11-20261025-491/`. The 3,597,396,735 compressed bytes
were streamed back and matched SHA-256
`f102a09515b5d9f53214edfae312c4ef0b7d9547897b8171f86f0c78491bfba3`;
independent remote decompression matched the uncompressed golden hash above.
[The retained S3 receipt](final-7743cc4b/golden-s3-receipt.json) records object
metadata, encryption and verified copies. No image file was stored on the
workstation, no credentials were copied, and no golden copy was deleted. Other threads receive this immutable
input and create their own overlays; their acceptance does not count for #491.

The #491 acceptance overlay `u24491`, run
`9fa63e67-b4b8-4b57-b6a9-5aa43b6d314e`, used KVM, 2 vCPUs, 6 GiB, native X11,
Qt 6.4.2 and default scale. Its production X11 capability witness and saved
GNOME desktop images establish the session used. Exact package/binary identities
are in the summary. Evidence and completion-manifest hashes were exported and
verified before guarded retirement at `05:17:28Z`; the independent provisioning
overlay retired at `05:18:16Z`. Two initial cleanup commands refused incorrect
canonical-root assumptions; rerunning the original checkout's cleanup tool with
explicit `--images-root /home/patrick/work/put.gd/work/lab-images` passed all
existing guards. The owned tunnel and wall tile were removed.


KDE/X11 used #559's sealed standalone golden
`5306a311c69220421d3efbbb8929300634c3812214edef3b9203b830ce300826`,
with an independent overlay `k491` on d69 (run
`34e65b15-02c5-42e9-a28c-10b544f56a99`, KVM, 2 vCPUs, 6 GiB).
The inherited old package was replaced by the exact Ubuntu 26 DEB. A preliminary
run is retained; the credited `vm-x11-20261025T051951Z` run followed visible
confirmation of the fully loaded desktop. Both startup screenshots and the
native platform mapping were reviewed. Verified export preceded guarded overlay
retirement; its receipt is in the summary. The owned wall tile/tunnel were removed.

The [rejected scaled Mint manual frame](pictures/m74-rejected-scaled-manual.png)
is retained as failed visual evidence, never a passing result. Corrected default-scale
first-open images above supersede it for the startup check.
