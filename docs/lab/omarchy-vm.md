# Omarchy (Hyprland) VM golden

Omarchy is an Arch Linux and Hyprland setup from omarchy.org
([basecamp/omarchy](https://github.com/basecamp/omarchy)). Its guest is the lab target for
Hyprland and for Arch packages, tracked in
[#1275](https://github.com/patrick-hudson/put.gd-desktop/issues/1275). It has an acceptance
row in the [matrix](../testing/acceptance-matrix.md#hyprland-omarchy-vm-row) that is not a
nightly or release gate; adding it to the nightly desktop check is Patrick's decision.

The golden was built end to end on a temporary EC2 instance on 2026-11-08 (UTC), uploaded
to S3 from there, and only downloaded to the builders. The instance is terminated.

## Golden

| Item | Value |
| --- | --- |
| Name | `omarchy-hyprland-20261108` |
| Source | Omarchy 4.5.0 release ISO `https://iso.omarchy.org/omarchy-4.5.0-x86.iso`, 6,540,910,592 bytes, SHA-256 `5d6288bc57f96f6231719830457582c846a2291985201c36a9a7d1216978b00f`; its `.sig` verified as a good signature from key `40DFB630FF42BCFFB047046CF0134EE680CAC571` (Omarchy &lt;pkgs@omarchy.org&gt;); ISO commit `69f9a702975766a9dd9d5e39101c203c6a963885` |
| Guest | Omarchy `4.5.0-3`, kernel `7.2.15-1-omarchy`, Hyprland `0.56.2-4` (Lua configuration), xdg-desktop-portal `1.22.1`, xdg-desktop-portal-hyprland `1.4.1`, qt6-base `6.12.0`, layer-shell-qt `6.8.1`, SDDM `0.21.0`, Limine `12.9.4`, gstreamer `1.28.8`, pipewire `1.6.9`, mesa `26.2.5`; btrfs on LUKS2 |
| Packages | `[omarchy]` from `https://pkgs.omarchy.org/stable/$arch`; core, extra and multilib from Omarchy's pinned Arch mirror `https://stable-mirror.omarchy.org/$repo/os/$arch`. The image has no synced package databases. |
| Session | SDDM autologin as `lab` into `omarchy.desktop` (Hyprland through uwsm): `Service=sddm-autologin`, `Type=wayland`, seat0. `XDG_CURRENT_DESKTOP=Hyprland`, `WAYLAND_DISPLAY=wayland-1`, `QT_QPA_PLATFORM=wayland;xcb`. Omarchy's desktop shell is one quickshell process (bar, dock, tray, notifications, lock and idle). |
| Login | [Standard remote guest login](remote-golden-images.md#standard-remote-guest-login). The disk passphrase is the same password (LUKS keyslot 0). |
| Unattended unlock | LUKS keyslot 1 holds a 64-byte `/crypto_keyfile.bin` (pbkdf2, 1,000 iterations). `/etc/mkinitcpio.conf.d/zz-putgd-lab-luks-keyfile.conf` adds it to the initramfs (`FILES+=(/crypto_keyfile.bin)`), and Omarchy's busybox `encrypt` hook uses it without a prompt. The key sits in the unified kernel image on the unencrypted ESP: this disk is not encrypted in any useful sense, which is fine for a lab guest only. |
| Firmware | UEFI without Secure Boot. Overlays boot from a fresh OVMF variables copy, as `lab.py` makes one, through the installer's fallback `EFI/BOOT/BOOTX64.EFI` (Limine), then the UKI. SSH answers about 16 s after QEMU starts. |
| Lab settings | Omarchy's Stay Awake on (`~/.local/state/omarchy/indicators/stay-awake`: no idle lock or screensaver); the four first-run intro notifications dismissed (they otherwise return at every login); `/etc/sudoers.d/90-putgd-lab-pacman` lets `lab` run only `/usr/bin/pacman` without a password, as the apt and dnf rules on other goldens do; sshd enabled and ufw allowing SSH (the installer does this when `authorized_keys` is supplied); hostname `omarchy-lab`; America/Chicago. |
| SSH | Host key `SHA256:QXkymaRZDqZC2DYzEmSJbqiZ1HNfVfRI41Lh/vokXSs`; the golden's `known_hosts` pins `[127.0.0.1]:22440` to `:22443`. Authorized key: only the bootstrap key `putgd-lab-omarchy` (`SHA256:iujcLZEruB3NquogHdTqR4ot4OVpcc+coKpR4z6nqKg`), kept on the workstation and at `~/work/put.gd/work/lab-bootstrap-omarchy/identity` on `f44` and `d69`. |
| Display | Hyprland renders in software with plain `-device virtio-vga` (no virgl and no host GPU), as Omarchy's own CI runs it. It boots at 1280x800; `lab.py layout` sets other sizes. The desktop shows on the wall like any other VM. |
| Image | Standalone qcow2, 4,923,523,072 bytes, virtual 51,539,607,552 (48 GiB), SHA-256 `bd74b7832c4a43f15a06f5a5b448e670a77d33ae75516b5bc5856773b69f3256`, `qemu-img compare` identical to the provisioned disk, `qemu-img check` clean. The encrypted root barely compresses: the zstd object is 4,869,090,452 bytes. |
| Copies | S3 and `f44` and `d69`; see [golden images](remote-golden-images.md#omarchy-hyprland-golden). |

Use the `omarchy-hyprland` profile in `tools/lab/profiles/` with this golden. It needs
the lab changes from [#1284](https://github.com/patrick-hudson/put.gd-desktop/pull/1284):
quickshell as the session process, the Stay Awake idle policy, and Hyprland outputs through
`hyprctl`.

```sh
g=~/work/put.gd/work/lab-images/omarchy-hyprland-20261108
r=~/work/put.gd/work/lab-runs/o1     # a new, short run name
python3 tools/lab/lab.py launch --profile omarchy-hyprland \
  --golden "$g/image.qcow2" --known-hosts "$g/known_hosts" \
  --bootstrap-key ~/work/put.gd/work/lab-bootstrap-omarchy/identity \
  --run "$r" --ssh-port 22440 --vnc-display 40
```

## Guest traps

- Hyprland 0.56 reads a Lua configuration. `hyprctl keyword` is refused; use
  `hyprctl eval '<lua>'` (for example `hl.monitor({ output = "Virtual-1", mode =
  "1920x1080@60", position = "0x0", scale = 1 })`). `hyprctl dispatch` takes Lua too.
- Never run a bind handle's `:remove()` in a guest you still need: a stale handle crashed
  Hyprland, which then restarted in `--safe-mode`. Don't run `hl.unbind(...)` by hand on a
  chord Omarchy uses in a guest you still need: it takes every bind written with that exact
  modifier order and key (spaces and case ignored), Omarchy's own and other submaps' included.
  `hyprctl reload` restores the configuration's binds and drops every runtime one.
- A Lua mistake in `~/.config/hypr/*.lua` makes Hyprland fall back to defaults. Regex strings
  belong in long brackets: `[[/tmp/\.mount_[^/]+/usr/bin/putgd]]`.
- Critical-urgency notifications stay in quickshell's `~/.local/state/omarchy/notifications/live.json`
  and come back after a reboot. Dismiss them with `omarchy-shell notifications dismiss
  "<summary>"` from the session environment (it needs `OMARCHY_PATH`).
- Commands from SSH need the session environment: take `WAYLAND_DISPLAY`,
  `HYPRLAND_INSTANCE_SIGNATURE` and `OMARCHY_PATH` from quickshell's `/proc/<pid>/environ`.
- `sudo -n` works only for `pacman`. Anything else needs `sudo -S` with the lab password on
  stdin. `pacman -Sy` then `-S` in an overlay is a partial upgrade against a moving mirror;
  keep it to overlays you will retire.
- With `ecosystem:enforce_permissions` on, Hyprland asks through `hyprland-dialog`, which
  crashes under software rendering (`DRM_IOCTL_MODE_CREATE_DUMB` permission denied), so the
  asking client hangs. Use explicit `hl.permission` rules in the VM.
- `lab.py run` scenarios need `putgd-desktop-fixture`, which only lab DEB builds ship.

## How it was made

The recipe needs no AWS credentials beyond an instance role scoped to the golden's S3 prefix.

1. Launch an `m7i.2xlarge` Ubuntu 24.04 instance in us-west-2 with
   `NestedVirtualization=enabled`, a 200 GiB gp3 volume, shutdown behaviour `terminate`, a
   12-hour `shutdown -h` timer, SSH open only to the workstation's address, and an instance
   profile allowed only `s3:PutObject`/`GetObject` and multipart calls on
   `linux/omarchy-hyprland-*`. Install `qemu-system-x86`, `ovmf`, `zstd`, `mtools`,
   `dosfstools`, `jq` and the AWS CLI.
2. Download the ISO, check its `.sha256` and verify its `.sig` against the Omarchy key.
3. Write the installer's unattended configuration to a 4 MiB FAT drive labelled `CIDATA`:
   `user_configuration.json` (full disk `/dev/vda`, 2 GiB ESP, btrfs subvolumes, LUKS on the
   root partition, Limine, `linux-omarchy`, the `omarchy` and `omarchy-settings` packages),
   `user_credentials.json` (`lab` with the standard password as a `openssl passwd -6` hash and
   the same `encryption_password`), `user_encrypt_installation.txt` (`true`),
   `authorized_keys` (the bootstrap key and a temporary provisioner key), and cloud-init
   `meta-data`/`user-data`. The shapes follow `test/integration.d/base-test.sh` in
   [omacom-io/omarchy-iso](https://github.com/omacom-io/omarchy-iso).
4. Boot QEMU (q35, KVM, UEFI, `virtio-vga`, the ISO as `ide-cd` bootindex 2, the drive as
   `usb-storage`, a 48 GiB qcow2 as `virtio-blk` bootindex 1). The install finishes in about a
   minute and reboots into the LUKS prompt; type the passphrase once with QMP `send-key`.
5. Over SSH: add the keyfile and the mkinitcpio drop-in and run `limine-mkinitcpio`; add the
   pacman sudoers rule; run `omarchy-toggle-idle stay-awake`; dismiss the intro
   notifications; remove provisioning files; vacuum the journal; `fstrim -av`; remove the
   provisioner key last; power off.
6. Boot an overlay with a fresh OVMF variables copy and no ISO or drive, confirm SSH with the
   bootstrap key, `sddm-autologin`, no notifications, Stay Awake, and a clean desktop frame.
7. `qemu-img convert`, `compare` and `check`; hash; `zstd --long=27`; upload with
   `GLACIER_IR`, SSE-S3 and the `sha256`, `zst-sha256`, `virtual-size` and `source-iso-sha256`
   metadata, plus the manifest, `known_hosts` and a desktop screenshot. Download it back on
   the instance and compare both hashes and the image.
8. Pull it to each builder through a short-lived presigned URL, checking both hashes, the
   image format, backing file and virtual size, and `qemu-img check`; then make it read-only.
9. Terminate the instance and delete its role, instance profile, security group and key pair.

Private receipts are on the workstation under `work/omarchy-1275-c/` (`ec2-receipts/`,
`shots/`, `cidata/`), and each builder's restore receipt is `restore-sha256sums.txt` beside
the image.
