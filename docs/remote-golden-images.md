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
uploads after 7 days. The Kubuntu builder holds no golden.

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
