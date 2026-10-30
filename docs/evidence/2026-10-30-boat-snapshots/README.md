# Boat snapshot refresh, 2026-10-30

This campaign created six immutable named snapshots. New lanes use the last
three recommended versions below. The first three were already published when
package-target, canonical-layout, fast-scratch and Mint Update scope was added;
they were preserved and superseded. No original named snapshot was replaced or
deleted. This is Boat development-cache evidence, not physical-desktop,
performance, memory or release acceptance.

The [implementation and authoritative manifests](https://github.com/patrick-hudson/put.gd-desktop/tree/b77f6124a0463f74fdc9b81dccb2a048ed228860/tools/lab/boat)
are in code PR [#782](https://github.com/patrick-hudson/put.gd-desktop/pull/782).
The [operator recipe](../../boat-snapshots.md) explains repeatable provisioning,
matching package targets and guarded retirement. The snapshot cannot contain
its own later publication ID; each external publication receipt binds its
embedded preparation manifest to the returned ID. Repository manifests also
record target metadata for the superseded IDs, without changing those caches.

| Created name | Snapshot ID | Frozen built tree | What is proved / status |
| --- | --- | --- | --- |
| `putgd-build-base-v2` | `f84f252c-0cce-4e9f-8a51-ca904c5bb7b8` | `e003bc7983e0a567611121cda65fc1670395bebf` | Preparation 114/114; fresh configure/build without provisioning; 114 overall with one known #635 retry. Superseded; no fast-scratch claim. |
| `putgd-lab-cinnamon-v2` | `78986e35-4791-42f8-a43d-5ee7b20700a9` | `e003bc7983e0a567611121cda65fc1670395bebf` | New probe golden; preparation and first fresh suite 114/114; inherited-pin launch/probes/cleanup. Its later preparation fork built 6ad and installed/started the matching 24.04 package. Superseded; no Mint Update override. |
| `putgd-lab-ubuntu-gnome-v3` | `c5e00501-fc17-4478-9476-8626ecf7ab00` | `e003bc7983e0a567611121cda65fc1670395bebf` | New probe/idle-policy golden; preparation 114/114; fresh inherited-pin launch/preflight/probes/cleanup. That same fresh fork later built 6ad and installed/started its 26.04 package in another fresh v3 overlay. Superseded; unused outside-root legacy copy remains in this preserved ID. |
| `putgd-build-base-v3` | `301fa0da-1124-4256-9f94-6c29fad5182b` | `6ad9f957e6544f30c5e45c08af2306db77a9ff64` | Recommended: fresh configure/build and plain 114/114, zero retries, automatic tmpfs. Matching 24.04 DEB installed and Settings opened in a separate visible fresh fork. |
| `putgd-lab-cinnamon-v3` | `849cde13-4322-4f89-a08e-86fa427564c9` | `6ad9f957e6544f30c5e45c08af2306db77a9ff64` | Recommended: warm build, matching 24.04 package, probes, Mint Update override, inherited-pin launch, reset, held-overlay refusal and guarded cleanup. Fresh host suite 113/114 first pass; known #635 focused retest passed. |
| `putgd-lab-ubuntu-gnome-v4` | `e27763c5-50e2-4ece-b27f-412345fb796f` | `6ad9f957e6544f30c5e45c08af2306db77a9ff64` | Recommended: fresh configure/build, matching 26.04 package with visible Settings, probes, pinned-key idle preflight, canonical images, reset, held-overlay refusal and guarded cleanup. Full host suite 112/114 first pass; both focused retests passed. |

The replacement source/package revision was frozen from `origin/nightly` at
`6ad9f957e6544f30c5e45c08af2306db77a9ff64`, version
`26.1030.2481-dev+g6ad9f957e654`. Later lane builds must still request their exact
candidate commit. No source version was bumped by hand.

## Required fresh-fork build and package proof

Build-base fresh worker `bx_pvwpzwe7` inherited
`PUTGD_CI_TMPDIR=/dev/shm` and `TMPDIR=/dev/shm` through `/etc/environment`.
Its inherited runner verified `tmpfs:1021994`, created a private short scratch
and private XDG homes, and ran as `putgd-ci`. Configure used timeout scale 3;
execution used time scale 3, the CI resource spec and `ctest --preset dev
--parallel 12`, with no retry wrapper. One plain full run passed **114/114 in
141.87 seconds**, with no manual scratch setup, apt install, SDK provisioning
or Proton reprovisioning. Runtime and bootstrap hashes were unchanged.

See [fresh host receipt](fresh-forks/fbase-host-verification.json),
[exact runner](fresh-forks/fbase/fresh-verify-host.sh),
[full log](fresh-forks/fbase-host.log),
[JUnit](fresh-forks/fbase/snapshot-tests.xml), and
[no-retry ledger](fresh-forks/fbase/snapshot-flakes.json).
The preparation suite passed 114/114 in 131.39 seconds; GNOME preparation
passed 114/114 in 127.50 seconds. Both used the same no-retry CI settings.

The first base startup saved a correct screenshot, but its intended wall
forward failed because another owned tunnel still held port 6261. That run
is functional startup evidence only. The [collision log](context/base-wall-port-collision.log)
is preserved. A second fresh fork, `bx_rxgaakqm`, repeated the matching-DEB
install and Settings startup after verifying the exact SSH listener PID.
See its [visible startup receipt](fresh-forks/fbase2-visible-verification.json),
[wall receipt](wall/fbase2-wall-receipt.json), and [saved screen](fresh-forks/fbase2/package/app-started.png).
Both forks were deleted after export; the extra fork did not repeat CTest.

The base's embedded manifest recorded auxiliary lab controller `234c5028`,
but its fresh fork contained merged controller `b063ba38`: the controller was
updated while Boat was saving the binding. The first verification setup
correctly stopped before CTest at that auxiliary metadata check. This driver
is not used by build-base's configure/build/test runner. The [correction](context/base-auxiliary-controller-correction.json)
and setup refusal are retained; the repository manifest records the observed
controller and the embedded manifest's checksum. The application/package,
Proton runtime and scratch settings were unchanged. Later lab saves remained
quiescent until ready readback and their controller identities match exactly.

| Snapshot/profile | Matching target | Audited DEB SHA-256 |
| --- | --- | --- |
| Build-base / Mint Cinnamon | `ubuntu24.04` | `dd8a58d3779e7e7873c69e364a72dbfa5439c9f3088841dc447c47afd2cdaa9f` |
| Ubuntu GNOME 26.04 / Qt 6.10.2 | `ubuntu26.04` | `969c6899a1297b056ef45b2eb3c69150af7fbd554db8fcde5959172dc8a998cd` |

Both DEBs were built from the exact clean 6ad source using the candidate's
native target recipe and that source's digest-pinned target distro container.
The existing preparation workers executed `candidate.recipe` directly to
reuse workers; this was not an extra candidate-controller sandbox campaign.
The supported lane command is `candidate.py COMMIT --artifact ubuntu26.04`
(or `ubuntu24.04` for Cinnamon/base). This is option (b) for #770. The host
remains Ubuntu 24.04; no host-built Noble DEB was substituted for GNOME.

See the [24.04 native audit](packages/ubuntu24.04/native-audit.json),
[26.04 native audit](packages/ubuntu26.04/native-audit.json), exact candidate
recipes and full build logs in those folders. Audited DEBs remain inside their
snapshots under `/home/user/snapshot-packages/TARGET/`; large package binaries
are retained in the private archive, not committed here. Installed version and
Git identity match the packaged build in each new recommended profile. App
startup also verifies the installed resident inode through production lab code.
No libraries were substituted and no desktop/compositor/portal packages were
installed or upgraded to force compatibility.

## Golden provenance, preflight and cleanup

All new images were flattened from stopped fresh overlays to standalone qcow2;
no golden was booted or modified directly. Source hashes were checked before
and after sealing. The source/target paths, SHA-256 values, probe package
contract, built commit, target and parent chains are in adjacent golden
manifests and the authoritative repository records.

| Profile/image | Direct source SHA-256 | New SHA-256 |
| --- | --- | --- |
| Initial Cinnamon probes | `4a21566d5b741e55f349f4f07e1c687c99d1c99b5394ff0d18ed5c9e685693da` | `518e3e45034c459e91118f95e3987971e19ea346486ab8219970b8484a892e48` |
| Cinnamon package intermediate, retained | `518e3e45034c459e91118f95e3987971e19ea346486ab8219970b8484a892e48` | `218f91afeda30448d82de38ce127ba3a77f3fbb2e0a8c2fbcf8ac27dc277c1b9` |
| Final Cinnamon v3 session image | `218f91afeda30448d82de38ce127ba3a77f3fbb2e0a8c2fbcf8ac27dc277c1b9` | `23b3e37571bdc7b452fdc30d53e70386423d75cb377e59653eed8a652c8bfab7` |
| Initial GNOME probes/idle policy | `563730cc1d929bd946d20cf30a9aecfe3c4ad40eb12ba7f9abcff1c461e342bb` | `349698ea91542e77a73801299aea300ff221ab11f07c4b5daf246fc72815c656` |
| Final GNOME v4 | `349698ea91542e77a73801299aea300ff221ab11f07c4b5daf246fc72815c656` | `8666b7dd8b98932e2fa794ec7bb4ccf0e82a8195e97a8580997ad70ed593a6c4` |

The canonical root is `/home/user/work/put.gd/work/lab-images`.
Cinnamon v3 selects `cinnamon-x11-probes-v3-session/image.qcow2`; GNOME v4
selects `gnome-wayland-probes-v4/image.qcow2`. Build-base preserves its inherited
canonical Cinnamon source (`4a21566d…`) unchanged and makes no probe-golden claim.

#685 changed the candidate controller and its unit tests; it did not change the
lab launcher/cleanup implementation or migrate an existing snapshot in code.
#686 documented a separately prepared canonical GNOME v2 copy. The inspected
v3 preparation fork contained both that canonical `563730cc…` disk and the
unused legacy `/home/user/putgd-gnome-golden/image.qcow2`, as well as the new
canonical probe image. New runs always selected the canonical image. To remove
that ambiguity from v4, the unreferenced legacy directory was relocated into
`lab-images/gnome-legacy-source-563730cc/` under the lifecycle lock, after
backing-reference and privileged read-only descriptor/mapping inventories.
Its image SHA-256 was unchanged. A pre-existing pilot overlay referencing the
original canonical Cinnamon base was preserved and its base was not moved.
See [inventory](context/gnome-image-inventory.json),
[relocation receipt](context/gnome-legacy-relocation.json), and
[exact relocation script](context/canonicalize-unused-copy.py).

The probe contract is the pinned surface-driver provisioning script plus the
requested tesseract package. Every tool and Python import was checked on fresh
forks without installing probes there. No OCR product feature was added.
GNOME uses the same console-verified guest host key in the inherited bootstrap
files. Their checksums are unchanged across launch; no manual guest-key work
was performed in verification.

GNOME v3 already passed the original #776 preflight helper
(`1e074f7c34708af97a18a9949499942ec583804d510729619de0d966683959e6`) on a
fresh fork: screensaver idle activation and lock are false, session idle-delay
is `uint32 0`, and the power idle dim/suspend actions are disabled. Its first
fresh run `fb50e286-cf1b-4889-8a37-b46050717495` and the later fresh-v3 package
run `d90afd11-c236-4069-909d-9b208630b158` retain preflight/read-only policy
receipts. V4 preserves that policy; its new version is for the expanded
package/layout scope, not an overwritten v3.

The fresh host checks used each snapshot's inherited recipe. Fresh guest
checks used the tested code-PR recipe in a separate `/home/user/fresh_recipe.py`;
the exported receipt records that file's SHA-256, and the private tar retains
its bytes. The built application and cached package remain source 6ad. This
collects explicit UI-show and complete socket-path witnesses without changing
bootstrap files or installing probes on verification forks.

Final lab verification uses #787's **merged** controller
`b063ba38d5fcaaa363c7ac1f9aa8ff449a41f6cb`, separately from built source 6ad.
Earlier preparation used branch commit `234c5028`; both identities are retained.
The exact held overlay device/inode is witnessed before cleanup. Cleanup must
refuse while that descriptor is open and leave the overlay present; after only
that owned holder is released, dry-run and actual guarded retirement succeed.

Cinnamon fresh fork `bx_wsvvktyu`: [receipt](fresh-forks/fcinnamon-lab-verification.json),
[launch](fresh-forks/fcinnamon/guest/launch.json),
[read-only tools](fresh-forks/fcinnamon/guest/probes.log),
[installed version](fresh-forks/fcinnamon/guest/installed-version.json),
[saved Settings](fresh-forks/fcinnamon/guest/app-started.png),
[held refusal](fresh-forks/fcinnamon/guest/held-overlay-refusal.json), and
[guarded cleanup](fresh-forks/fcinnamon/guest/cleanup.json).
The [session override check](fresh-forks/fcinnamon/guest/session-policy-after-start.json)
reports `Hidden=true`, autostart disabled, and no Mint Update PID before or after
app startup. No process kill occurred in fresh verification. Only the new
provisioning overlay received the supported per-user XDG override; the source
golden is unchanged. This addresses the #785 foreground-guard observation;
this campaign did not replay the editor row campaign.

GNOME fresh fork `bx_kywgfsas`: [receipt](fresh-forks/fgnome-lab-verification.json),
[launch](fresh-forks/fgnome/guest/launch.json),
[read-only tools](fresh-forks/fgnome/guest/probes.log),
[idle readback](fresh-forks/fgnome/guest/idle-policy.json),
[installed version](fresh-forks/fgnome/guest/installed-version.json),
[saved Settings](fresh-forks/fgnome/guest/app-started.png),
[held refusal](fresh-forks/fgnome/guest/held-overlay-refusal.json), and
[guarded cleanup](fresh-forks/fgnome/guest/cleanup.json).

## Preserved failures and limits

The disk-backed build attempt `bx_vvh4xqcg` failed six tests on its first pass
and retained three timeout failures after the prior CI retry policy. Its
[ledger](failed-disk-backed-build/snapshot-flakes.json), JUnit and
[log](context/pbuild-updated-host.log) were exported before deletion. The healthy
Cinnamon preparation worker supplied the new base instead. This supports the
#792 scratch workaround; it does not establish an underlying provider or
filesystem cause, or claim a controlled same-worker comparison here.

Cinnamon's final fresh host full suite passed 113/114 on its first pass. The
one failure was the known #635 `gui_history_view` geometry check at line 832,
actual 53 versus expected 12, also seen in the initial base fresh verification.
Its [first full JUnit](fresh-forks/fcinnamon/snapshot-tests.xml),
[first full ledger](fresh-forks/fcinnamon/snapshot-flakes.json),
[full log](fresh-forks/fcinnamon-host.log),
[focused retest command](context/fcinnamon-focused.sh),
[focused JUnit](fresh-forks/fcinnamon/focused-tests.xml), and
[focused passing log](context/fcinnamon-focused.log) are preserved. The focused
retest passed in 58.80 seconds without changing assertions, socket ownership,
resources or timeout scales. This is 114 passing overall with a known flake,
not a zero-retry full-suite pass. The base-v3 #792 proof is the separate plain
114/114 run above.

GNOME's final fresh host suite was **112/114 in 152.45 seconds** on its first
plain full pass. The two failures were `x11_isolated` (settling/pixel readback)
and `editor_job_integration` (digest mismatch returned `editor_timeout` rather
than `editor_io`). The older compositor-settling area is tracked by #765/#773;
#773 landed after the frozen 6ad source, and this run does not validate that
newer fix. The editor-helper occurrence is filed separately as
[#812](https://github.com/patrick-hudson/put.gd-desktop/issues/812).
The separate focused run passed both tests on the same binaries: editor in
5.44 seconds and X11 in 20.64 seconds. No assertions, timeouts, resources or
socket ownership checks were changed. See the [first JUnit](fresh-forks/fgnome/snapshot-tests.xml),
[first ledger](fresh-forks/fgnome/snapshot-flakes.json),
[full log](fresh-forks/fgnome-host.log),
[focused command](context/fgnome-focused.sh),
[focused JUnit](fresh-forks/fgnome/focused-tests.xml), and
[focused log](context/fgnome-focused.log). This is 114 passing overall with
recorded first-pass failures, not a no-retry full-suite pass. Both final lab
snapshots had separate 114/114 preparation suites; their required fresh-fork
configure/build and production guest checks are recorded independently.

An initial GNOME package preparation failed its capacity check before starting
a VM: the old 24 GiB file plus an uncompressed standalone output would violate
the reserve. That [failed attempt](context/vgnome-guest-package-capacity-attempt.log)
was retained. The successful path used compressed qcow2 with a hard output-size
limit preserving 10 GiB reserve. Source bytes stayed unchanged.

During GNOME preparation, the canonical-copy inventory lock overlapped the first
held-overlay check. The guard correctly refused with "Another VM lifecycle
operation is in progress", and the harness stopped. The [original refusal](preparation/gnome/guest/held-overlay-refusal.json)
and [failed log](context/vgnome-guest-package.log) remain. After that lock was
released, the merged controller proved the real [held-descriptor refusal](preparation/gnome/guest/held-overlay-refusal-merged-controller.json)
and [successful cleanup](preparation/gnome/guest/cleanup.json), without rewriting
run identity or bypassing any guard. Final fresh-fork checks used the merged
controller end to end.

Aurora v2 was not started. [#769](https://github.com/patrick-hudson/put.gd-desktop/issues/769)
tracks its unverified Toolbx/overlay route; no rpm-ostree change or Aurora golden
mutation was attempted. Issues #770, #775, #776, #790 and #792 remain open for
the coordinator, with evidence comments rather than closure.

## Retention and worker retirement

All 12 owned sandboxes were deleted, including stopped states;
[the final all-state audit](context/owned-workers-absent.json) lists each ID and
its deletion receipt. Peak active count was at most three. No stop/resume or
GNOME replacement was used. One early [recover request](context/gnome-recover.json) returned
`recover_unsupported`; hydration subsequently completed without a power cycle.
Additional starts included the failed disk-backed preparation and the separate
visible base startup proof. Existing preparation workers were reused for retries.
Unrelated workers were preserved.

[The protected-name audit](context/protected-snapshots-unchanged.json) compares
all five original named IDs and verifies all six newly created bindings. Only
owned wall entries/tunnels and the owned isolated SSH agent were retired; see
[the runtime receipt](context/runtime-cleanup.json).

`SHA256SUMS` indexes this public packet. [The private restore index](context/archive-index.json)
records retained bulk archives/packages by hash and relative path; no golden,
credential, session or private SSH key is included in this repository packet.
Raw PPMs remain private; the PNGs here are lossless format conversions of the
actual saved screens. [Recipe tests](context/recipe-tests.txt) and
[candidate contracts](context/candidate-tests.txt) record 34 and 46 passing
lightweight tests respectively. No builds or VMs ran on the workstation.
