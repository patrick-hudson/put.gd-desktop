# The lab

Physical builders, disposable VMs, goldens and cloud workers.

## Guarantees

- The five physical desktops serve one session at a time; take the desktop lock before any GUI work. Heavy work stays off the workstation.
- Linux golden disks are immutable: never boot or modify them directly, and never move, overwrite or delete a disk referenced by overlays. Never delete any copy without Patrick's approval and two other copies verified against the same SHA-256.
- Linux tests use fresh disks under `work/lab-runs/` through `tools/lab/lab.py`, which never shares the host clipboard and binds VNC and SSH forwarding to 127.0.0.1.
- Do not disable or redirect guest display output during graphical tests. Capture visual results for every graphical acceptance run, including failures.
- Boat runs never count as acceptance rows. Boat candidate builds provide package and offscreen catalog evidence, not desktop, hardware or release acceptance.
- Before a build or graphical campaign, require at least 10 GiB host reserve, 2 GiB guest reserve and more than 1,024 free inodes where the filesystem reports a count.

## Documents

- [Remote builders](remote-builders.md): the physical servers, host selection, sharing rules and visual evidence.
- [Remote VM displays](remote-vm-displays.md): tunnels and wall registration for remote QEMU displays.
- [Golden images](remote-golden-images.md): finding, copying and restoring goldens, and the standard guest login.
- [Disposable graphical lab](vm-lab.md): `tools/lab/lab.py`, VM lifecycle, provisioning and cleanup.
- [Aurora VM layer](aurora-vm.md): the Universal Blue layer for AppImage testing.
- [KDE neon VMs](neon-vm.md): released Plasma and Plasma master goldens.
- [Omarchy VM golden](omarchy-vm.md): the Arch and Hyprland golden, its unattended LUKS unlock and guest traps.
- [Boat candidates](boat-candidates.md): exact candidate builds in disposable Boat workers.
- [Boat snapshots](boat-snapshots.md): snapshot recipes and package targets.
- [HTTP receivers](http-receivers.md): Zipline, Chibisafe and XBackBone in containers, their own `.sxcu` files and the upload check for the custom uploader.
- [Lab storage and retention](lab-storage.md): capacity checks and storage retention.
