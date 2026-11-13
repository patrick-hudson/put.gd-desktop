# Find, copy and restore lab golden images

The workstation no longer keeps Linux goldens. On 2026-10-17,
with Patrick's approval, they were moved off the workstation to three verified copies:

| Location | Role | Root |
| --- | --- | --- |
| Fedora 44 (`192.168.68.70`, pin `f44`) | Primary copy; most free space | `/home/patrick/work/put.gd/work/` |
| Debian 13 (`192.168.68.69`, pin `d69`) | Second LAN copy | `/home/patrick/work/put.gd/work/` |
| S3 `putgd-lab-images-<account-id>` (us-west-2) | Off-site copy | `linux/<name>/` |

`<account-id>` is the AWS account of Patrick's `aws login` session; commands
derive it with `aws sts get-caller-identity`. The bucket is private: all Block
Public Access settings are on, ownership is `BucketOwnerEnforced` and SSE-S3
encryption is the default. Disks are zstd-compressed and stored in Glacier
Instant Retrieval (millisecond reads, per-GB retrieval fee, 90-day minimum);
small metadata uses Standard. A lifecycle rule aborts incomplete multipart
uploads after 7 days. Kubuntu (`k72`), Mint (`m74`) and Aurora (`aurora-amd`) hold
extra copies of two goldens so they can lend their Proton sessions to guests; see
[below](#copies-on-k72-m74-and-aurora-amd).

All copies are ignored private artifacts; `git ls-files` will not list them.
Treat every copy as an immutable source. Never boot, modify, move or overwrite
one. Never delete any of the three copies without Patrick's explicit approval
and two other copies that have been verified against the same SHA-256. Offload receipts are under
the workstation's `work/housekeeping/golden-offload-20261017/` (private), and each
per-host transfer receipt is in `work/verification/golden-transfer-<id>/`.

## Current inventory

Host paths are relative to `~/work/put.gd/work/lab-images/`. Each Linux
`transfer-<id>/` holds read-only `image.qcow2`, plus `source.manifest.json` for
provisioned images. Directories created by the offload also hold `origin.json`
with the original workstation name, SHA-256 and virtual size. The S3 object is
`linux/<name>/<name>.qcow2.zst`. Its `sha256` metadata is the uncompressed
image hash from the table, and `zst-sha256` and `virtual-size` are also recorded.
`linux/<name>/<name>.manifest.json` holds the sidecar when one exists.

| Name (profile) | SHA-256 | Fedora `f44` | Debian `d69` |
| --- | --- | --- | --- |
| `cinnamon-x11` (Cinnamon/X11) | `4a21566d5b741e55f349f4f07e1c687c99d1c99b5394ff0d18ed5c9e685693da` | `transfer-32dfa35ea5c8` | `transfer-7dfa9973258d` |
| `gnome-provisioned-20261016` (provisioned Ubuntu GNOME/Wayland) | `563730cc1d929bd946d20cf30a9aecfe3c4ad40eb12ba7f9abcff1c461e342bb` | `transfer-2f93113ef3b0` | `transfer-0d2922e34f4b` |
| `gnome-provisioned-20261016-trimmed-20261113` (the image above with more free disk; nightly Ubuntu GNOME row) | `a330ab0d14f84e1190ac1ef6740149bf4392f9aa266a5c44647f0c6d60779696` | not copied | `gnome-provisioned-20261016-trimmed-20261113` |
| `kde-provisioned-20261016` (provisioned Kubuntu KDE/Wayland) | `49fcacff969b81ac38e36fefdf21407d20c82187e1725c58f128a3b76065a81c` | `transfer-459d0181e995` | `transfer-541c1316ab6b` |
| `fedora-gnome-wayland` (Fedora GNOME source) | `fb83af6d5d34d8ba278023376878c822f42e6f33f65288f02fc8cbc541f373c8` | `transfer-de0468ff530f` | `transfer-ed01f06f6523` |
| `p2c-provisioned-a1` (Cinnamon/X11 variant from historical run `p2c-a1`) | `ebc60005f72aaf67f1c14f51a16e09749cf09d91ad1890e50a0eedda30b40252` | `transfer-55342ecaa28c` | `transfer-f544391582e0` |
| `gnome-wayland` (original Ubuntu GNOME install) | `ffb4aecd868da250b091c1ae9759552b71cfd3ffed12995b56560ace8f8602ad` | `transfer-8708f37d1464` | `transfer-c14ba36479ca` |
| `kde-wayland` (original Kubuntu KDE install) | `a5b75be961ecff6bbbc0008667529648918f20f0458260971d8e077ba4e5bb7a` | `transfer-2fbac618586d` | `transfer-4af6c524fe18` |
| `fedora44-budgie` (Budgie/Wayland) | `37f84c90fc165e6817ffc5dd969eaaf57a36c195dc04f0f3bcc269b5e1b83a80` | `fedora44-budgie`; verified 2026-11-08 | `fedora44-budgie`; verified 2026-11-08 |
| `ubuntu-budgie-2604` (Budgie/Wayland) | `7719f71aa6dac412c7c5b58502f2d27530fa8485c6f978ef2d6be3302cd4ba74` | `ubuntu-budgie-2604`; verified 2026-11-08 | `ubuntu-budgie-2604`; verified 2026-11-08 |
| `fedora45-budgie` (Budgie/Wayland) | `a6749512438d507ef2b2c47fe675955627464d9a5a85341002f91cd54fb401ad` | `fedora45-budgie`; verified 2026-11-08 | `fedora45-budgie`; verified 2026-11-08 |
| `ubuntu-budgie-2404` (Budgie/X11) | `aeebdf7eba6d11f3598d4565cae78da34c70ee9be008cf77096882f35d0c7cf5` | `ubuntu-budgie-2404`; verified 2026-11-08 | `ubuntu-budgie-2404`; verified 2026-11-08 |

Use the provisioned GNOME/KDE images for new provisioned runs of those profiles.
Their manifests record `profile`, `sha256`, `standalone`, `virtualBytes`, source
chain and verification. Migration receipts are under
`work/housekeeping/standalone-goldens-20261016/`. Verify `fedora-gnome-wayland`'s
bootstrap/profile before calling it a provisioned acceptance guest. The original
`gnome-wayland`/`kde-wayland` installs and `p2c-provisioned-a1` are kept for
history and reprovisioning. They are not interchangeable with the provisioned
images. `cinnamon-x11` trusts only the workstation's `work/lab-bootstrap/identity`.
The historical offloaded images above were standalone qcow2 files with 32 GiB
virtual disks; the newer Budgie images below have 40 GiB disks.
That is not a permanent integrity guarantee, so recheck the hash before use.

### Trimmed Ubuntu GNOME golden (d69, 2026-11-13)

`gnome-provisioned-20261016` boots with 3.1 GB free on its 32 GiB disk. Its
`/home/lab` holds about 15.6 GB of source and build trees from October
development. On `v26.1113.4757-nightly` the nightly Ubuntu GNOME row ran out of
guest disk twice: once a capture session was lost, and once GNOME's "Low Disk
Space" notification covered the test pattern.

`gnome-provisioned-20261016-trimmed-20261113` is that image with 14 of those
trees and 5 archives removed from `/home/lab`. It boots with 19 GB free.
Nothing else changed: same packages, account, host key, bootstrap key and idle
policy, so it uses the parent's `known_hosts` pin. It was made on d69 by booting
the parent in an overlay, deleting the trees by name as `lab`, zero-filling the
free space, powering off cleanly and running `qemu-img convert` to a standalone
qcow2 (10.4 GiB on disk). `origin.json` beside the image records the parent,
the hash and the change.

d69's `~/.config/putgd-nightly/config.json` names it as the Ubuntu GNOME row's
`golden`. The previous file is kept beside it as
`config.json.before-trimmed-gnome-20261113`. The parent image is unchanged and
is still the one to use for anything that needs those old trees. The trimmed
image is on d69 only and is not in S3.

### Budgie goldens (EC2, 2026-11-08)

The four Budgie images above are standalone **40 GiB** qcow2 files, each at
`<name>/image.qcow2` with `source.manifest.json`, pinned `known_hosts`,
`upload-metadata.json` and `restore-receipt.json` on both f44 and d69. All disk and
source files are read-only. They trust the existing per-builder bootstrap public
keys from `work/lab-bootstrap-f44/identity` and `work/lab-bootstrap-d69/identity`;
private keys stayed on their owning hosts. Login is the
[standard remote guest login](#standard-remote-guest-login).

| Golden / profile | Desktop and display manager | Compressed SHA-256 |
| --- | --- | --- |
| `fedora44-budgie` | Budgie 10.10.2, labwc 0.9.6, SDDM | `1c7c8ba1ac09c7f5eeb5e07f0d07ebf145e027bb20ca1371a847a176b4dc0c19` |
| `ubuntu-budgie-2604` | Budgie 10.10.2, labwc 0.9.3, SDDM | `e30f799525997f2d7a2fadd4bcb9d89a40495de8353fd0740b9728d6b8b1a7e9` |
| `fedora45-budgie` | Budgie 10.10.3, labwc 0.20.2, SDDM | `a5e95f7ad4270366aded8da2a6397770b4e6df8b93e48cb779c3dd3de8c5c855` |
| `ubuntu-budgie-2404` | Budgie 10.9.1, X11, LightDM | `3afe192ce09b968e17ffd3329940f3c58d7327228919e0cc26acb8a84d252eac` |

Built entirely on EC2 `i-0f8e59ab52b315c3b` (`m7i.2xlarge`, nested virtualization,
`us-west-2a`) from signed/checksummed official Fedora Budgie Spin 44-1.7/45-1.4
and Ubuntu Budgie 24.04.5/26.04.1 media. Manual login and display-manager autologin
were visibly verified before sealing. Installation, standalone conversion,
`qemu-img compare`/`check`, zstd compression and uploads all ran on EC2. No
multi-GB image was uploaded from the LAN. The instance is terminated and its
EBS volume, temporary security group and imported public key are removed.

The private S3 bucket is `putgd-lab-images-626193791566` in `us-west-2`. Each image
uses `linux/<name>/<name>.qcow2.zst` (Glacier Instant Retrieval, SSE-S3) with
`sha256`, `zst-sha256`, `virtual-size=42949672960` and `source-instance-id`
metadata; its manifest is `linux/<name>/<name>.manifest.json`. Small sidecars
are under `linux/<name>/sidecars/`. A full S3 download/decompression on EC2
matched both hashes; independent pull-only restores on both builders also
matched both hashes and passed standalone/virtual-size and `qemu-img check`.
No immutable golden was booted directly or replaced. Provisioning evidence is
retained privately at
`linux/fedora44-budgie/sidecars/campaign-provisioning-20261108.tar.gz`
(SHA-256 `8d46effc3aaa8995ede968f4a973dce041b5b033bad187ca239ef4ec877f52c7`).

Fresh d69 overlays passed real-session preflight and ran published nightly
`26.1105.3986` DEBs/AppImage with saved VNC/QMP frames. All three labwc sessions
(both bridge generations) fired real QEMU Ctrl+Shift+1/2/3 and removed `customputgd*` entries
cleanly. Ubuntu 24.04's existing X11 adapter captured region/window/desktop
and bound all three hotkeys. These are step-1 infrastructure/probe results;
they do not certify the new Budgie adapter or a Budgie 11 desktop.
See [Budgie runs](vm-lab.md#budgie-goldens) for profile and launch details.

### Verified Boat snapshot copies

These historical copies are preserved. New lanes use the [probe-equipped replacements](#probe-equipped-boat-copies) below.

The Boat copies use profile directories below
`/home/user/work/put.gd/work/lab-images/`. A directory's profile name is not the
source image identity from the inventory above:

| Saved snapshot | Canonical disk in a fresh fork | Source image identity | SHA-256 |
| --- | --- | --- | --- |
| `putgd-lab-ubuntu-gnome-v2` | `gnome-wayland/image.qcow2` | `gnome-provisioned-20261016` (provisioned Ubuntu GNOME/Wayland) | `563730cc1d929bd946d20cf30a9aecfe3c4ad40eb12ba7f9abcff1c461e342bb` |
| `putgd-lab-cinnamon` | `cinnamon-x11/image.qcow2` | `cinnamon-x11` | `4a21566d5b741e55f349f4f07e1c687c99d1c99b5394ff0d18ed5c9e685693da` |
| `putgd-build-base` | `cinnamon-x11/image.qcow2` | `cinnamon-x11` | `4a21566d5b741e55f349f4f07e1c687c99d1c99b5394ff0d18ed5c9e685693da` |

In particular, the Boat `gnome-wayland/` directory contains a verified copy of
**`gnome-provisioned-20261016`**, not the historical unprovisioned `gnome-wayland`
image with digest `ffb4aecd…`. The v2 preparation copied the standalone disk into
this canonical root without moving or deleting the source; both the copied bytes
and the source inventory digest were checked. The older
`putgd-lab-ubuntu-gnome` snapshot remains preserved with its original outside-root
path and is superseded for new runs. See [Boat candidate builds](boat-candidates.md#lab-snapshots)
for placement, receipts and the guarded cleanup root.

Leftover Windows base images remain under `work/windows/bases/` on d69 and m74;
do not delete them without Patrick's approval.

The Aurora goldens are named directories rather than `transfer-<id>/`, and each
holds `source.manifest.json`, `image-info.json`, the pinned guest `known_hosts`
and the bootstrap inputs beside `image.qcow2`. They are described in
[Aurora VM](aurora-vm.md). The current Fedora 45 golden is on all five builders and
in S3. On 2026-11-02, with Patrick's approval, the two older Fedora 45 goldens were
deleted from the builders: `-852d34e885c1`, which never had a complete S3 image, so
no copy remains, and the 2026-10-29 cloud golden, which remains in S3 and in the
Boat snapshot. Deleting `-852d34e885c1` was an approved exception to the two-copies
rule above, made knowing it can't be restored. The Fedora 44 v3 golden stays `f44`-only, also
an approved exception (2026-11-09): it is an optional row and can be rebuilt from its
source tag. Fedora 44 v1 and v2 were removed from `f44` on 2026-11-07, also approved exceptions
to that rule, so no copy of either remains.

**Which Fedora 45 golden counts for acceptance.** Since 2026-11-02 the Aurora
Fedora 45 row in the [acceptance matrix](../testing/acceptance-matrix.md) uses
`aurora-testing-45.20261101.2-ec2-20261102-a1` (Plasma 6.8.2), booted as a VM on a
physical builder; see [Plasma 6.8 golden](aurora-vm.md#plasma-68-golden). Every Aurora
45 result recorded before then used `-852d34e885c1` (Plasma 6.7.5), which no longer
exists. The 2026-10-29 cloud golden is a separate build of that older OS
payload. It serves as an off-site restore copy and as the
base of the `putgd-lab-aurora` Boat snapshot, for development, recipe debugging
and flake reproduction. Boat runs never count as acceptance rows, whichever golden
they use. A new golden moves acceptance once a builder-hosted run has passed on it
and the matrix has been updated.

| Name (profile `aurora-kde-wayland`) | SHA-256 | Fedora `f44` | Debian `d69` |
| --- | --- | --- | --- |
| `aurora-testing-45.20261101.2-ec2-20261102-a1` (Fedora 45, Plasma 6.8.2, use for acceptance) | `731b43833009724506feb20d35a19a417da8bbae2890940571b7da4edc1a35ae` | same name; verified 2026-11-02 | same name; verified 2026-11-02 |
| `aurora-testing-45.20261022.1-ec2-20261029-a1` (Fedora 45 cloud restore base, Plasma 6.7.5) | `a71cdd0cc8b485ee7e384670245293dbafd7fe46b8cbb05b4fbccf0d1f0d8128` | removed 2026-11-02; S3 only | removed 2026-11-02; S3 only |
| `aurora-testing-45.20261022.1-852d34e885c1` (Fedora 45 base, Plasma 6.7.5) | `33fedf977f1e0cb7e34b944700f1959f4da5c708fa85dbbc9879077af1278556` | removed 2026-11-02 | removed 2026-11-02; no copy remains |
| `aurora-stable-44.20260908.1-v3-492c3f8945d1` (Fedora 44 base) | `6a2fb407830f95228bb06f4d20d7cd0bd1aec8ed937ef59430e50eb608b62c2d` | same name | none |
| `aurora-stable-44.20260908.1-v2-af46889e2a54`, `-f9c006a376e1` (history) | see [Aurora VM](aurora-vm.md#goldens-on-f44) | removed 2026-11-07; no copy remains | none |

### Probe-equipped Boat copies

Use `putgd-lab-cinnamon-v3` and `putgd-lab-ubuntu-gnome-v4` for new development.
The old names and the intermediate Cinnamon v2 / GNOME v3 remain intact but
are superseded. All disks below are standalone qcow2 Boat derivatives under
`/home/user/work/put.gd/work/lab-images/`; the physical-builder/S3 source
inventory above is unchanged. Their adjacent manifests record both image
hashes and provisioning packages. The repository manifests in
`tools/lab/boat/manifests/` also bind the snapshot ID and cached build commit.

| Snapshot | Golden folder | SHA-256 | App package target |
| --- | --- | --- | --- |
| `putgd-lab-cinnamon-v2` (superseded) | `cinnamon-x11-probes-v2` | `518e3e45034c459e91118f95e3987971e19ea346486ab8219970b8484a892e48` | `ubuntu24.04` |
| `putgd-lab-cinnamon-v3` | `cinnamon-x11-probes-v3-session` | `23b3e37571bdc7b452fdc30d53e70386423d75cb377e59653eed8a652c8bfab7` | `ubuntu24.04` |
| `putgd-lab-ubuntu-gnome-v3` (superseded) | `gnome-wayland-probes-v2` | `349698ea91542e77a73801299aea300ff221ab11f07c4b5daf246fc72815c656` | `ubuntu26.04` |
| `putgd-lab-ubuntu-gnome-v4` | `gnome-wayland-probes-v4` | `8666b7dd8b98932e2fa794ec7bb4ccf0e82a8195e97a8580997ad70ed593a6c4` | `ubuntu26.04` |

Append `/image.qcow2` to each folder. Cinnamon's original source is
`4a21566d…5693da`; its retained compressed intermediate is
`cinnamon-x11-probes-v3/image.qcow2`, SHA-256
`218f91afeda30448d82de38ce127ba3a77f3fbb2e0a8c2fbcf8ac27dc277c1b9`.
GNOME's original provisioned source is `563730cc…42bb`. Full parent chains are
in the manifests. Build-base v3 preserves its inherited canonical Cinnamon
image unchanged; it makes no guest-probe claim.

The new images add the pinned surface-driver probe contract plus tesseract.
GNOME retains the verified guest host pin and the lab idle policy. Cinnamon
v3 adds a per-user XDG override disabling Mint Update autostart. Both
recommended lab snapshots contain matching audited DEBs and the merged #787
lab controller. GNOME's host is Ubuntu 24.04; its Qt 6.10 guest requires the
Ubuntu 26.04 target container package, not a host-built Noble DEB.

The final GNOME preparation also relocated its unused legacy outside-root
copy into `gnome-legacy-source-563730cc/` only after a hash, complete backing
reference and open-file inventory. Its bytes and SHA-256 stayed unchanged;
no referenced base or original named snapshot was moved or changed.
[The campaign record](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-30-boat-snapshots/README.md) retains the
inventory, failed attempts, fresh launch/reset/held-overlay-refusal/guarded-cleanup
receipts and worker deletion proof. Follow the [recipe](boat-snapshots.md) to
make another new version; never overwrite these images or snapshot names.

### Aurora Plasma 6.8 golden

Built on EC2 on 2026-11-02 from Aurora `testing-45.20261101.2`; see
[Plasma 6.8 golden](aurora-vm.md#plasma-68-golden) for the facts and recipe.

| Item | Verified value |
| --- | --- |
| S3 image key | `linux/aurora-testing-45.20261101.2-ec2-20261102-a1/aurora-testing-45.20261101.2-ec2-20261102-a1.qcow2.zst` |
| S3 manifest key | Same prefix, `aurora-testing-45.20261101.2-ec2-20261102-a1.manifest.json` |
| Image SHA-256 | `731b43833009724506feb20d35a19a417da8bbae2890940571b7da4edc1a35ae` |
| Compressed SHA-256 | `6a7ef6dd591ea7b69afb0d1ee421450d7a10890f709745bb8e3e89f641ee5a53` |
| Image / compressed / virtual bytes | 11,139,153,920 / 4,171,244,697 / 50,994,348,032 |
| Storage | Glacier Instant Retrieval, SSE-S3; small sidecars use Standard |
| Host copies | `f44`, `d69`, `k72`, `m74` and `aurora-amd`: `~/work/put.gd/work/lab-images/aurora-testing-45.20261101.2-ec2-20261102-a1/image.qcow2` |

The S3 object passed a download round trip on EC2 (both hashes and `qemu-img compare`),
and each builder's restore checked both hashes and `qemu-img check`. The
`putgd-lab-aurora` Boat snapshot still holds the 2026-10-29 Plasma 6.7.5 golden.

### Omarchy Hyprland golden

Built on EC2 on 2026-11-08 from the Omarchy 4.5.0 ISO; see
[Omarchy VM golden](omarchy-vm.md) for the facts and recipe.

| Item | Verified value |
| --- | --- |
| S3 image key | `linux/omarchy-hyprland-20261108/omarchy-hyprland-20261108.qcow2.zst` |
| S3 sidecars | Same prefix: `omarchy-hyprland-20261108.manifest.json`, `sidecars/known_hosts`, `sidecars/golden-boot-verify.png` |
| Image SHA-256 | `bd74b7832c4a43f15a06f5a5b448e670a77d33ae75516b5bc5856773b69f3256` |
| Compressed SHA-256 | `c56cfe7ddb2fe345663ff2c18f07c8ede9bc692ba6b2354267228dadb1eb2a1b` |
| Image / compressed / virtual bytes | 4,923,523,072 / 4,869,090,452 / 51,539,607,552 |
| Storage | Glacier Instant Retrieval, SSE-S3; small sidecars use Standard |
| Host copies | `f44` and `d69`: `~/work/put.gd/work/lab-images/omarchy-hyprland-20261108/image.qcow2`, with `known_hosts`, the manifest and `restore-sha256sums.txt` |
| Bootstrap key | `~/work/put.gd/work/lab-bootstrap-omarchy/identity` on `f44` and `d69` |

The S3 object passed a download round trip on EC2 (both hashes and `qemu-img compare`),
and each builder's restore checked both hashes, the format, backing file and virtual size,
and `qemu-img check`.

### Aurora Fedora 45 cloud golden and Boat snapshot

For [#665](https://github.com/patrick-hudson/put.gd-desktop/issues/665), Patrick
approved using the EC2-built `aurora-testing-45.20261022.1-ec2-20261029-a1`
instead of uploading the older builder disk over the home uplink. Both use
Aurora `testing-45.20261022.1` from the same pinned OCI payload,
`ghcr.io/ublue-os/aurora@sha256:969e4f6f164742b9f3f85e74b958faf88e357b65620e7c2f1cc28723948a9930`.
They are separate immutable goldens; a different qcow2 hash is expected after
independent provisioning. Existing builder disks were not replaced.

| Item | Verified value |
| --- | --- |
| S3 image key | `linux/aurora-testing-45.20261022.1-ec2-20261029-a1/aurora-testing-45.20261022.1-ec2-20261029-a1.qcow2.zst` |
| S3 manifest key | Same prefix, `aurora-testing-45.20261022.1-ec2-20261029-a1.manifest.json` |
| Image SHA-256 | `a71cdd0cc8b485ee7e384670245293dbafd7fe46b8cbb05b4fbccf0d1f0d8128` |
| Compressed SHA-256 | `10cb310ce793cc3d3666bbb2b212d2f6db81392ec5c70bc24b27c64acf600be0` |
| Image / compressed / virtual bytes | 10,503,323,648 / 4,311,949,729 / 50,994,348,032 |
| Storage | Glacier Instant Retrieval, SSE-S3; small sidecars use Standard |
| S3 composite SHA-256 | `UJFDDDU3KHt+Megzp2WwAIpSImrkPoPbZQLMcTutsMQ=-65` |
| Host copies | None since 2026-11-02; restore from S3 |
| Boat snapshot | `putgd-lab-aurora`, ready; snapshot `23bc3677-0b0d-42eb-9930-3e29cc3c47b5` |

The EC2 job verified manual standard login and plasmalogin autologin as `lab`,
Fedora 45, KDE Plasma/Wayland, the masked `uupd.timer`, and the lab idle policy.
It verified standalone qcow2 metadata and `qemu-img check`, then downloaded the
S3 image back to EC2 and checked both hashes and `qemu-img compare`. The Fedora
and Debian restores have matching image and sidecar hashes. The EC2 instance
and its temporary volume, network interface, security group, keypair and IAM
resources were deleted after verification. These are OS-provisioning receipts,
not application desktop acceptance.

The S3 prefix also holds the provisioning evidence and
`sidecars/job-completion.json`. UEFI code, the blank VARS template and the
provisioned VARS are under `sidecars/firmware/` as `OVMF_CODE_4M.fd`,
`OVMF_VARS_4M.fd` and `provisioned-vars.fd`. Their SHA-256 values and server
checksums are in the [verification receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-29-aurora-golden-s3-665/receipt.json).
Future runs must use private writable copies of VARS, preserving the archived
firmware and golden.

`putgd-lab-aurora` was created by the EC2/Boat preparation session from
`putgd-build-base`, with `--no-env`, finite TTLs and hydration checks. It contains
the directly restored S3 golden, a built nightly tree and persistent build
caches. Its clean built source is `6c09178f058f93eaba64f198c54151cd67f94a2e`,
version `26.1030.2333-dev+g6c09178f058f`, at `/home/user/work/put.gd/src`.
The golden is that source tree's
`work/lab-images/aurora-testing-45.20261022.1-ec2-20261029-a1/image.qcow2`.
The host uses Ubuntu 24.04, dynamic distro Qt 6.4.2 and official AWS SDK 1.11.907.
The golden restore verified compressed/image hashes, virtual size, standalone
metadata and `qemu-img check` in 199.63 seconds. Its preparation CTest run had
113 tests: `gui_tray` recovered on retry; `gui_settings` and `proton_transport`
failed both attempts. Both match known flakes, not defects in this image:
`gui_settings` at `test_settings.cpp:2250` is
[#707](https://github.com/patrick-hudson/put.gd-desktop/issues/707), and
`proton_transport`'s cancellation reason mismatch is
[#577](https://github.com/patrick-hudson/put.gd-desktop/issues/577). Run one at a
time on the snapshot's own build, with the guest stopped, `proton_transport` passed
8 of 8 and `gui_settings` 2 of 2 (independent check, 2026-10-30). The failed
attempts are kept in the template receipt.

Create a disposable worker with
`boat new --from putgd-lab-aurora --type large --ttl 7200 --no-env --name YOUR_RUN_NAME`.
Wait for `hydrated: true`, then source `/home/user/aurora-builder/env.sh` before
building. The SDK prefix and `/home/user/putgd-cache` caches are retained.
`/usr/bin/python3 /home/user/aurora-builder/start-aurora.py --name aYOURRUN`
creates a fresh disposable public-key overlay and private guest key on the
worker, preflights sockets and invokes the production lab launcher; it does
not require the workstation's private key or agent forwarding. Follow
[VM lab](vm-lab.md) and [Aurora](aurora-vm.md) display/overlay rules before
launching a guest. Retire completed overlays through guarded cleanup and
delete only your worker after exporting evidence. `start-aurora.py` also leaves
its key-seed disk, `work/lab-images/keyseed-<run>/image.qcow2` (an overlay of
the golden), which guarded cleanup does not remove. Deleting the worker removes
it; if you keep a worker for several runs, delete each `keyseed-<run>`
directory after that run's cleanup. An independent fresh fork on 2026-10-30
booted the golden under UEFI into the `lab` Plasma Wayland autologin session,
with the pinned image digest, sshd, the narrow sudo rule and a masked
`uupd.timer`. Guarded cleanup then accepted its overlay.

The separate older-disk upload was cancelled at 84 verified 64 MiB parts
(5,637,144,576 bytes), and its incomplete multipart upload was aborted. No
complete image object was created for `aurora-testing-45.20261022.1-852d34e885c1`.
Its already-uploaded small metadata and OVMF copies remain under that old
prefix; they do not constitute an S3 golden image. At that time the old f44 image and
sidecar hashes, sizes, modes and mtimes remained unchanged. The unused worker from
that upload session was deleted, preserving other sessions' workers and the
EC2-built named snapshot. The receipt distinguishes these two sessions and
identities explicitly.

The KDE neon goldens are named directories like the Aurora ones. Each is on
both builders and in S3 (`linux/<name>/<name>.qcow2.zst` and
`<name>.manifest.json`); see [KDE neon VMs](neon-vm.md).

| Name (profile `neon-kde-wayland`) | SHA-256 | Fedora `f44` | Debian `d69` |
| --- | --- | --- | --- |
| `neon-unstable-noble-20261023-d613510c07bd` (Plasma, Frameworks and Gear git master) | `9fef081035ca6691446672b2914c09ff4a7b4a4b359577a6087279b0d892e8f6` | same name; SHA-256 and sidecars verified 2026-10-23 (receipt `work/verification/golden-transfer-7b9fb1191be6`) | same name (made here) |
| `neon-user-6.8.1-20261023-9ef808278853` (released Plasma 6.8.1) | `191852a5b552492daa9eac1093acd7e15578b0cc5374f4c9a660037af5863238` | same name (made here) | same name; SHA-256 and sidecars verified 2026-10-23 (receipt `work/verification/golden-transfer-6141be2a1211`) |

### Copies on k72, m74 and aurora-amd

On 2026-10-29 and 2026-10-30, two goldens were copied from `f44` to k72, m74 and
aurora-amd, for Proton lending. These are extra copies: the three-copy rule above still counts Fedora,
Debian and S3.

| Name | Kubuntu `k72` | Mint `m74` | Aurora `aurora-amd` |
| --- | --- | --- | --- |
| `gnome-provisioned-20261016` | `transfer-eff2bd963713` | `transfer-659d95302859` | `transfer-000d35aedb8b` |
| `cinnamon-x11` | `transfer-4ecf0e847901` | `transfer-1e7496436875` | `transfer-69d4d8858821` |

- **How they were copied:** each image was streamed once from f44 through a pipe on the
  workstation and written to no disk there. For m74 and aurora-amd, one stream was teed
  to both.
- **Verification:** before promotion, each copy matched the inventory SHA-256 and passed
  `qemu-img info` (qcow2, no backing file, same virtual size) and `qemu-img check`. The
  sidecars were copied and hash-checked too.
- **Receipts:** under the workstation's `work/verification/golden-transfer-<id>/`.

Aurora's paths are under `/var/home/patrick/`; pass them to `lab.py` in that form.

## Use a host copy in place

On Fedora or Debian, pass the inventory's final `image.qcow2` path to the runner's
`--golden` with a fresh short `--run` path. Do not make another copy on the same
host. The runner creates the disposable overlay. Check the hash first:

```sh
sha256sum ~/work/put.gd/work/lab-images/transfer-ID/image.qcow2
```

## Restore to the workstation

Local VMs are the documented fallback. Restore only the image the run needs,
to its historical workstation name, and remove that local copy again once no
overlay references it. Pull from Fedora, from Debian if Fedora is unavailable,
and from S3 only if neither host is reachable. S3 needs a valid `aws login`
session and incurs retrieval and egress charges. Run from the workstation:

```sh
set -euo pipefail
name=gnome-provisioned-20261016
expected_sha=PASTE_FROM_INVENTORY
host_copy=/home/patrick/work/put.gd/work/lab-images/transfer-ID/image.qcow2
dest=/home/patrick/work/put.gd/work/lab-images/$name.qcow2
test ! -e "$dest" && test ! -e "$dest.part"
mkdir -p "$(dirname "$dest")"
rsync -aS --protect-args \
  -e 'ssh -i /home/patrick/.ssh/awskey_110724 -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/f44/known_hosts' \
  "patrick@192.168.68.70:$host_copy" "$dest.part"
# Or, without either host:
# bucket=putgd-lab-images-$(aws sts get-caller-identity --query Account --output text)
# aws s3 cp "s3://$bucket/linux/$name/$name.qcow2.zst" - | zstd -dc > "$dest.part"
test "$(sha256sum "$dest.part" | cut -d ' ' -f 1)" = "$expected_sha"
qemu-img check "$dest.part"
chmod 0444 "$dest.part"
ln "$dest.part" "$dest" && rm "$dest.part"
```

For Debian substitute `.69` and `d69/known_hosts`, and use its inventory path.
Copy the provisioned sidecar (`source.manifest.json` on a host, or the S3
`.manifest.json`) to `${dest%.qcow2}.manifest.json` when the run needs it.
Record the restore source in the run's evidence.

## Standalone copy recipe

Use this to add a verified copy to another builder, such as Kubuntu, or to
re-create a lost host copy. Keys stay on the workstation, so builders cannot
copy directly to each other. First [restore the image to the workstation](#restore-to-the-workstation),
then run this recipe with that local file as `golden`, and finally remove the temporary
local copy. Use the [preferred builders and pinned SSH access](remote-builders.md).
Check remote tools, storage/inodes, existing VMs, backing chains and reserved
ports before transfer. Run the following Bash example on the workstation for Fedora;
for Debian substitute `.69` and `d69/known_hosts` in both SSH and rsync.
The example creates a fresh destination and never overwrites an existing golden.

```sh
set -euo pipefail
golden=/home/patrick/work/put.gd/work/lab-images/gnome-provisioned-20261016.qcow2
transfer_id=$(python3 -c 'import uuid; print(uuid.uuid4().hex[:12])')
remote_dir=/home/patrick/work/put.gd/work/lab-images/transfer-$transfer_id
receipt=/home/patrick/work/put.gd/work/verification/golden-transfer-$transfer_id
mkdir -m 700 "$receipt"
ssh_options=(-i /home/patrick/.ssh/awskey_110724 -o BatchMode=yes
  -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes
  -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/f44/known_hosts)
qemu-img info --output=json "$golden" > "$receipt/source-info.json"
python3 - "$receipt/source-info.json" <<'CHECK_SOURCE'
import json, sys
info = json.load(open(sys.argv[1]))
assert info['format'] == 'qcow2' and not info.get('backing-filename')
CHECK_SOURCE
qemu-img check "$golden" > "$receipt/source-check.txt"
expected_sha=$(sha256sum "$golden" | cut -d ' ' -f 1)
printf '%s\n' "$expected_sha" > "$receipt/source.sha256"
ssh "${ssh_options[@]}" patrick@192.168.68.70 \
  "umask 077; mkdir -p /home/patrick/work/put.gd/work/lab-images; mkdir -m 700 '$remote_dir'"
rsync -aS --protect-args \
  -e 'ssh -i /home/patrick/.ssh/awskey_110724 -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/f44/known_hosts' \
  "$golden" "patrick@192.168.68.70:$remote_dir/image.qcow2.part"
actual_sha=$(ssh "${ssh_options[@]}" patrick@192.168.68.70 \
  "sha256sum '$remote_dir/image.qcow2.part'" | cut -d ' ' -f 1)
test "$actual_sha" = "$expected_sha"
test "$(sha256sum "$golden" | cut -d ' ' -f 1)" = "$expected_sha"
ssh "${ssh_options[@]}" patrick@192.168.68.70 \
  "qemu-img info --output=json '$remote_dir/image.qcow2.part'" > "$receipt/destination-info.json"
ssh "${ssh_options[@]}" patrick@192.168.68.70 \
  "qemu-img check '$remote_dir/image.qcow2.part'" > "$receipt/destination-check.txt"
python3 - "$receipt/source-info.json" "$receipt/destination-info.json" <<'CHECK_COPY'
import json, sys
source, copied = [json.load(open(p)) for p in sys.argv[1:]]
assert copied['format'] == source['format'] == 'qcow2'
assert copied['virtual-size'] == source['virtual-size']
assert not copied.get('backing-filename') and not source.get('backing-filename')
CHECK_COPY
```

Copy the selected sidecar manifest explicitly when present and verify it too:

```sh
sidecar=${golden%.qcow2}.manifest.json
if [ -f "$sidecar" ]; then
  scp "${ssh_options[@]}" "$sidecar" "patrick@192.168.68.70:$remote_dir/source.manifest.json"
  sidecar_sha=$(sha256sum "$sidecar" | cut -d ' ' -f 1)
  remote_sidecar_sha=$(ssh "${ssh_options[@]}" patrick@192.168.68.70 \
    "sha256sum '$remote_dir/source.manifest.json'" | cut -d ' ' -f 1)
  test "$remote_sidecar_sha" = "$sidecar_sha"
fi
printf '%s\n' "$golden" "$remote_dir/image.qcow2" "$expected_sha" > "$receipt/transfer.txt"
```

Preserve the manifest's original source identity; add this separate transfer
receipt rather than rewriting historical manifest/run paths. Do not copy a whole `work/`
directory, `credentials.txt`, protected password files or the workstation SSH key.
If a source has a backing chain, stop this recipe and create/verify a new standalone
image through [VM lab's flatten-and-compare procedure](vm-lab.md).

After all checks and sidecar collection, promote without replacing any existing
final path. The hard link fails if that name exists; only its staging name is removed:

```sh
ssh "${ssh_options[@]}" patrick@192.168.68.70 \
  "chmod 0444 '$remote_dir/image.qcow2.part' && ln '$remote_dir/image.qcow2.part' '$remote_dir/image.qcow2' && rm '$remote_dir/image.qcow2.part'"
```

Record the final path/hash and export the receipt to the workstation. On the
builder pass this final path as the runner's `--golden` with a fresh short `--run`
path. Preflight socket lengths there. Never boot the source or copied golden
directly; create independent run overlays through `tools/lab/lab.py`. Connect the
actual guest using [remote VM display tunnels](remote-vm-displays.md).

## Standard remote guest login

| Setting | Required value for newly provisioned remote lab VMs |
| --- | --- |
| Guest username | `lab` |
| Shared default lab password | `putgd-lab` |

Patrick explicitly approved publishing this shared disposable-guest login. This
is the exception to the no-credentials-in-source rule. Physical builders continue
using Patrick's existing SSH-key access.

A copied historical image does not acquire this password during transfer. Boot an
independent provisioning candidate, set its `lab` account to the shared default
through an administrative guest channel, and verify login visibly on its own wall
tile. Send setup input in memory over the authenticated channel, without process
arguments or logs. Shut down and create a new standalone golden at a new path,
with its own hash/manifest. Preserve both original and copied immutable sources.

For the first login to an older Linux source, agents use the protected local input
listed in [VM lab](vm-lab/hosts-and-bases.md#protected-login-inputs), read only in memory, and the
visible masked guest console. Do not ask Patrick for a known lab login or copy
legacy input files to builders. Record whether the remote image has actually
passed standard-login provisioning; do not claim migration merely from a copy.
