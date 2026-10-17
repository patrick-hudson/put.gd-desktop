# Find and copy lab golden images

The source images are on Patrick's main workstation in
`/home/patrick/work/put.gd/work/lab-images/`. They are ignored private artifacts;
a fresh Git clone or `git ls-files` will not list them. Run this on the workstation:

```sh
cd /home/patrick/work/put.gd
rg --files --hidden --no-ignore work/lab-images
qemu-img info --output=json --backing-chain work/lab-images/gnome-provisioned-20261016.qcow2
```

## Current source inventory

Paths below are relative to the workstation's `work/lab-images/`, checked
2026-10-16. Recheck the selected profile, guest version, backing chain and hash.

| Profile | Image | Metadata |
| --- | --- | --- |
| Cinnamon/X11 | `cinnamon-x11.qcow2` | Profile and bootstrap/launch instructions in [VM lab](vm-lab.md) |
| Provisioned Ubuntu GNOME/Wayland | `gnome-provisioned-20261016.qcow2` | `gnome-provisioned-20261016.manifest.json` |
| Provisioned Kubuntu KDE/Wayland | `kde-provisioned-20261016.qcow2` | `kde-provisioned-20261016.manifest.json` |
| Fedora GNOME source | `fedora-gnome-wayland.qcow2` | Verify bootstrap/profile before calling it a provisioned acceptance guest |

Use the provisioned GNOME/KDE images for new provisioned runs of those profiles.
Their manifests record `profile`, `sha256`, `standalone`, `virtualBytes`, source
chain and verification. Migration receipts are under
`work/housekeeping/standalone-goldens-20261016/`. Nearby older images are not
interchangeable. All four listed images were standalone qcow2 files with 32 GiB
virtual disks at inventory; this is not a permanent integrity guarantee.

## Standalone copy recipe

Use the [preferred builders and pinned SSH access](remote-builders.md). Check
remote tools, storage/inodes, existing VMs, backing chains and reserved ports
before transfer. Run the following Bash example on the workstation for Fedora;
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
