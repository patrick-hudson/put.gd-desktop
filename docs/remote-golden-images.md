# Find, copy and restore lab golden images

The workstation no longer keeps Linux goldens or Windows bases. On 2026-10-17,
with Patrick's approval, they were moved off the workstation to three verified copies:

| Location | Role | Root |
| --- | --- | --- |
| Fedora 44 (`192.168.68.70`, pin `f44`) | Primary copy; most free space | `/home/patrick/work/put.gd/work/` |
| Debian 13 (`192.168.68.69`, pin `d69`) | Second LAN copy | `/home/patrick/work/put.gd/work/` |
| S3 `putgd-lab-images-<account-id>` (us-west-2) | Off-site copy | `linux/<name>/` and `windows/<name>/` |

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
| `kde-provisioned-20261016` (provisioned Kubuntu KDE/Wayland) | `49fcacff969b81ac38e36fefdf21407d20c82187e1725c58f128a3b76065a81c` | `transfer-459d0181e995` | `transfer-541c1316ab6b` |
| `fedora-gnome-wayland` (Fedora GNOME source) | `fb83af6d5d34d8ba278023376878c822f42e6f33f65288f02fc8cbc541f373c8` | `transfer-de0468ff530f` | `transfer-ed01f06f6523` |
| `p2c-provisioned-a1` (Cinnamon/X11 variant from historical run `p2c-a1`) | `ebc60005f72aaf67f1c14f51a16e09749cf09d91ad1890e50a0eedda30b40252` | `transfer-55342ecaa28c` | `transfer-f544391582e0` |
| `gnome-wayland` (original Ubuntu GNOME install) | `ffb4aecd868da250b091c1ae9759552b71cfd3ffed12995b56560ace8f8602ad` | `transfer-8708f37d1464` | `transfer-c14ba36479ca` |
| `kde-wayland` (original Kubuntu KDE install) | `a5b75be961ecff6bbbc0008667529648918f20f0458260971d8e077ba4e5bb7a` | `transfer-2fbac618586d` | `transfer-4af6c524fe18` |

Use the provisioned GNOME/KDE images for new provisioned runs of those profiles.
Their manifests record `profile`, `sha256`, `standalone`, `virtualBytes`, source
chain and verification. Migration receipts are under
`work/housekeeping/standalone-goldens-20261016/`. Verify `fedora-gnome-wayland`'s
bootstrap/profile before calling it a provisioned acceptance guest. The original
`gnome-wayland`/`kde-wayland` installs and `p2c-provisioned-a1` are kept for
history and reprovisioning. They are not interchangeable with the provisioned
images. `cinnamon-x11` trusts only the workstation's `work/lab-bootstrap/identity`.
All images were standalone qcow2 files with 32 GiB virtual disks when offloaded.
That is not a permanent integrity guarantee, so recheck the hash before use.

The sealed Windows bundles are listed in [Windows lab](windows-vm-lab.md#base-storage-and-restore).

The Aurora goldens are named directories rather than `transfer-<id>/`, and each
holds `source.manifest.json`, `image-info.json`, the pinned guest `known_hosts`
and the bootstrap inputs beside `image.qcow2`. They are described in
[Aurora VM](aurora-vm.md). The independently rebuilt Fedora 45 cloud golden
below is verified on `f44`, `d69` and S3. It uses the same pinned Aurora OS as the
older `-852d34e885c1` golden, with its own disk identity. The older disk still
has no complete S3 image copy. The Fedora 44 evaluation goldens remain `f44`-only.

**Which Fedora 45 golden counts for acceptance.** The Aurora Fedora 45 row in the
[acceptance matrix](acceptance-matrix.md) stays on `-852d34e885c1`, booted as a VM
on a physical builder (`f44` or `d69`). That disk is the one every recorded
Aurora 45 result used. The cloud golden is a separate build of the same OS
payload, with different bytes. It serves as the off-site restore copy and as the
base of the `putgd-lab-aurora` Boat snapshot, for development, recipe debugging
and flake reproduction. Boat runs never count as acceptance rows, whichever golden
they use. To move acceptance to the cloud golden, a builder-hosted run must first
show it gives the same results, and the matrix must then be updated.

| Name (profile `aurora-kde-wayland`) | SHA-256 | Fedora `f44` | Debian `d69` |
| --- | --- | --- | --- |
| `aurora-testing-45.20261022.1-ec2-20261029-a1` (Fedora 45 cloud restore base) | `a71cdd0cc8b485ee7e384670245293dbafd7fe46b8cbb05b4fbccf0d1f0d8128` | same name; verified 2026-10-30 | same name; verified 2026-10-30 |
| `aurora-testing-45.20261022.1-852d34e885c1` (Fedora 45 base, use for acceptance) | `33fedf977f1e0cb7e34b944700f1959f4da5c708fa85dbbc9879077af1278556` | same name | same name; SHA-256 and sidecars verified 2026-10-23 (receipt `work/verification/golden-transfer-1d422e725efb`) |
| `aurora-stable-44.20260908.1-v3-492c3f8945d1` (Fedora 44 base) | `6a2fb407830f95228bb06f4d20d7cd0bd1aec8ed937ef59430e50eb608b62c2d` | same name | none |
| `aurora-stable-44.20260908.1-v2-af46889e2a54`, `-f9c006a376e1` (history) | see [Aurora VM](aurora-vm.md#goldens-on-f44) | same name | none |

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
| Host copies | Both builders: `~/work/put.gd/work/lab-images/aurora-testing-45.20261022.1-ec2-20261029-a1/image.qcow2` |
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
checksums are in the [verification receipt](evidence/2026-10-29-aurora-golden-s3-665/receipt.json).
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
prefix; they do not constitute an S3 golden image. The old f44 image and sidecar
hashes, sizes, modes and mtimes remained unchanged. The unused worker from
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
Windows candidates also retain the complete [base bundle lifecycle](windows-vm-lab.md).

For the first login to an older Linux source, agents use the protected local input
listed in [VM lab](vm-lab.md#protected-login-inputs), read only in memory, and the
visible masked guest console. Do not ask Patrick for a known lab login or copy
legacy input files to builders. Record whether the remote image has actually
passed standard-login provisioning; do not claim migration merely from a copy.
