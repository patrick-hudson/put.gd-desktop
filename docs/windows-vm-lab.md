# Windows desktop lab

This is infrastructure for future Windows acceptance. The approved Linux plan
currently defers native Windows capture adapters and distributables. An installed
Windows desktop or a passing host isolation test does not establish app acceptance.

Use `python3 tools/lab/windows.py` on the actual QEMU host, with `/dev/kvm`
access. Install normal distro packages `qemu-system-x86 qemu-utils ovmf swtpm
swtpm-tools`. All media, disks, firmware variables, TPM state and run witnesses
live under the ignored `work/windows/` directory. No cloud credentials, private
payloads, license keys or personal accounts belong in a base image.

## Official installation media

Retrieved on 2026-10-08, English US x64, Enterprise evaluation editions:

| Image | Official link | Published SHA256 |
| --- | --- | --- |
| Windows 11 Enterprise evaluation (26H2, 26300.9457) | [Microsoft evaluation download](https://www.microsoft.com/en-us/evalcenter/download-windows-11-enterprise) | `bc3f24086ebadc94489066b5ad78089e2cf5c3491e90e790bb81a2b199c10e38` |
| Windows 10 Enterprise evaluation (21H2, 19044.1288) | [Microsoft community manager's evaluation links](https://techcommunity.microsoft.com/discussions/windows10space/accessing-trials-and-kits-for-windows-10/3361184) | `69efac1df9ec8066341d8c9b62297ddece0e6b805533fdb6dd66bc8034fba27a` |

Windows 11's current en-US link is
`https://go.microsoft.com/fwlink/?LinkId=2382600&clcid=0x409&culture=en-us&country=us`.
Its checksum is from [Microsoft's authenticity page](https://go.microsoft.com/fwlink/?LinkId=2382606).
Windows 10's still-working historical en-US link is
`https://go.microsoft.com/fwlink/p/?LinkID=2195400&clcid=0x409&country=US&culture=en-us`.
Its checksum is from [Microsoft's 21H2 hash list](https://download.microsoft.com/download/9/3/b/93bfb33f-643f-43c9-a36f-cf3611c72187/Windows10Enterprise21H2HashValues.pdf).
Verify hashes after every download; fwlinks can change. Local retrieval details
are recorded in `work/windows/media/sources.json`. Download to `.partial`, resume
with `curl -C -`, and rename only after curl succeeds and the hash matches.

Microsoft's current Windows 10 evaluation landing page redirects to an end-of-support
article. The historical ISO is **21H2, not 22H2**; preserve that distinction in evidence.
The [consumer 22H2 ISO](https://www.microsoft.com/en-us/software-download/windows10ISO)
is a different licensing route, not a 90-day Enterprise evaluation.
Do not substitute LTSC or consumer editions silently.

Evaluations are time limited. Record actual activation status and expiry with
`slmgr /dlv` and `slmgr /xpr` inside each guest. Cloning does not extend the period;
do not roll back the clock or use snapshots to evade expiry. Microsoft's evaluation
guidelines describe hourly shutdowns after expiry. Keep an expired base for evidence,
and obtain appropriately licensed media if the lab must continue beyond evaluation.

## Install each base once, visibly

The helper uses KVM, 2 virtual CPUs, 4096 MiB RAM, an 80 GiB **sparse** qcow2 disk,
Q35 SATA, standard VGA, USB tablet, enrolled Microsoft Secure Boot UEFI and TPM 2.0.
SATA/e1000e need no external Windows virtio drivers. The host clipboard, host folders
and host block devices are not shared. VNC binds only to localhost; the existing
[display wall](http://127.0.0.1:8765/) discovers both guests automatically. `-display
none` suppresses an extra host SDL window; the VNC graphical display stays active.

```sh
python3 tools/lab/windows.py prepare --os windows11 --vm work/windows/bases/windows11
python3 tools/lab/windows.py start --vm work/windows/bases/windows11 --display 30 \
  --iso work/windows/media/windows11-enterprise-eval-x64.iso
python3 tools/lab/windows.py prepare --os windows10 --vm work/windows/bases/windows10
python3 tools/lab/windows.py start --vm work/windows/bases/windows10 --display 31 \
  --iso work/windows/media/windows10-enterprise-eval-21h2-x64.iso
```

Do not repeat `prepare` for existing directories. The helper refuses overwrite.
Ports are `127.0.0.1:5930` and `127.0.0.1:5931`. The wall is view-only; use a VNC
viewer or the helper's QMP key command for input. Keep the wall visible throughout
graphical work. Press a key at the DVD boot prompt, use the normal installer,
and choose the VM's blank disk. Microsoft license acceptance belongs to the user.
Use a disposable local `lab` account through the installer-supported organization/
domain-join route where offered. Do not bypass security or account requirements;
hand off actual account/password entry when required. The provisioned `lab`
accounts have no password, as explicitly approved by the user.

Networking is **off** by default, including during installation. After installation,
shut down, detach the ISO by starting without `--iso`, and enable temporary outbound
NAT with `--network` for evaluation activation and deliberate Windows updates. There
are no inbound forwarded ports. Avoid running an outdated Windows 10 guest online
for ordinary browsing. Record OS build after updates, finish pending restarts, confirm
the desktop boots from disk, and then shut down cleanly. Capture versions, activation
expiry and `Get-Tpm | Select-Object TpmPresent,TpmReady,RestartPending` / `Confirm-SecureBootUEFI` results before sealing.

```sh
python3 tools/lab/windows.py status --vm work/windows/bases/windows11
python3 tools/lab/windows.py key --vm work/windows/bases/windows11 ctrl alt delete
python3 tools/lab/windows.py screenshot --vm work/windows/bases/windows11
python3 tools/lab/windows.py shutdown --vm work/windows/bases/windows11
python3 tools/lab/windows.py start --vm work/windows/bases/windows11 --display 30 --network
# Finish activation/updates in the visible guest; record actual expiry.
python3 tools/lab/windows.py shutdown --vm work/windows/bases/windows11
python3 tools/lab/windows.py seal --vm work/windows/bases/windows11 \
  --notes 'Replace with actual build, activation expiry, secure boot/TPM checks and provisioning summary'
```

Repeat for Windows 10. `shutdown` waits for a real guest shutdown before stopping
QEMU and TPM. If Windows ignores ACPI, use the visible Start menu to shut down and
retry. `shutdown --force` stops only the owned VM and invalidates clean-shutdown
status: it is for recovering a stuck disposable guest, not producing a base.
QEMU/TPM PID checks reject other processes. The TPM is stopped through `swtpm_ioctl`,
which works with the distro AppArmor profile. Lifecycle commands take interprocess
locks and report “VM busy” rather than race against a seal or another start. Never
copy a running TPM or firmware. Forced shutdown uses the owned QEMU PID even if QMP
is unavailable, and refuses sealed bases.

## Repeatable test copies

Sealing checks qcow2 integrity and hashes the disk, UEFI variables, TPM state and
identity. Base files become read-only, and `start` refuses a sealed base. Every clone
verifies hashes (allow time to read the full base disk) and creates a fresh disk overlay plus **independent writable copies**
of UEFI and TPM state. Clones preserve VM UUID to avoid gratuitous hardware identity
changes. Give every concurrent guest its own VNC display number.

```sh
python3 tools/lab/windows.py clone --base work/windows/bases/windows11 \
  --vm work/windows/runs/w11-001
python3 tools/lab/windows.py start --vm work/windows/runs/w11-001 --display 30
# Install the Windows build and synthetic fixtures in this overlay; test visibly.
python3 tools/lab/windows.py screenshot --vm work/windows/runs/w11-001
python3 tools/lab/windows.py shutdown --vm work/windows/runs/w11-001
```

Repeat with `w11-002` to reset; retain the old overlay and witness for diagnosis.
Stock Windows power settings can blank an idle console. Wake it with the helper
`key --vm ... spc` and restore the visible `lab` desktop before acceptance; a
blank/locked screen is not graphical evidence. For a long test session, run
`powercfg /change monitor-timeout-ac 0` and `powercfg /change standby-timeout-ac 0`
in that disposable guest. Make test-specific configuration changes in the overlay.
There is no automatic deletion, merge-to-base or destructive reset command. Never
move a base that overlays reference. Update by provisioning and sealing a new base
directory, leaving prior runs linked to their original base.

On this 32 GiB host, prefer one active Windows test guest at a time alongside the
Linux lab. Each active Windows guest needs 4 GiB; downloads use about 13 GiB combined,
and sparse disks grow with writes up to 80 GiB each. Monitor `free -h` and `df -h`;
retain useful evidence but remove explicitly disposable stopped runs when space is
needed. An 80 GiB virtual capacity is not an 80 GiB initial allocation.

## Verification boundaries

Run `python3 -m unittest discover -s tests/desktop -p test_windows_lab.py -v`
for host disk/firmware/TPM isolation, immutable base and unclean seal checks.
These use synthetic state and real qemu-img/qemu-io; they do not certify an installed
Windows desktop, TPM behavior inside Windows or application acceptance.
Actual desktop installation, disk-only boot, activation/expiry, secure boot/TPM,
independent clone boots and the Windows application matrix must each be recorded
separately. This helper does not port the Linux fixture runner or build a Windows app.

### Provisioning evidence (2026-10-08)

Both official ISOs matched Microsoft's published SHA256 values. Both installed
through their real visible Microsoft installers on new VM disks. The user approved
both license agreements and passwordless local `lab` accounts. Normal offline setup
was available in both; no requirement bypasses were used. Both booted from disk with
the installation DVD detached, and activated using temporary outbound NAT.

| Baseline | In-guest build | Evaluation expiry reported by `/xpr` (guest local time) | Security checks |
| --- | --- | --- | --- |
| Windows 11 Enterprise evaluation | 26H2, 26300.9457 | 2027-01-06 02:31:41 | Secure Boot true; TPM present/ready true; restart pending false |
| Windows 10 Enterprise evaluation | 21H2, 19044.1288 | 2027-01-06 02:25:09 | Secure Boot true; TPM present/ready true; restart pending false |

These are the original ISO baselines, without deliberate OS updates. Windows 10 is
21H2, **not 22H2**. The expired watermark seen on initial offline boots changed to
“license valid for 90 days” after activation; neither clock nor licensing was altered.
Curated build/security/expiry screenshots are under each base's
`security-expiry-build.png`; no product keys are part of the evidence.

Windows 10 was cleanly shut down and sealed. `w10-001` booted offline and signed in
automatically. A synthetic `clone-one-only` desktop folder was created there.
`w10-002` verified the original base hashes, booted independently offline with
activation intact, and had no marker folder. Both copies retain separate firmware
and TPM state. Windows 11 is also sealed after verified clean shutdown.
`w11-001` and `w11-002` both booted independently offline and retained activation.
The synthetic marker created in `w11-001` was absent from `w11-002`, and the base
hashes matched again before the second clone.

The clean `w11-002` and `w10-002` copies are left running offline on VNC ports
5930 and 5931. Their display/sleep AC timeouts are disabled for long test sessions;
the sealed baselines retain stock Windows power settings. Both remain visible
alongside the existing Linux guests in the wall. Prefer shutting one Windows guest
down when running memory-heavy builds. About 129 GiB of host storage remained
after provisioning.

Twelve host isolation/lifecycle tests pass. Disk isolation uses real qemu-img/qemu-io;
shutdown fault cases use controlled QMP/PID/clock doubles. Separate actual-host
checks recovered a tiny owned QEMU daemon with its QMP socket missing and stopped
an actual swtpm daemon after an injected state-write failure. The latter exposed the
distro AppArmor signal restriction; the helper now uses the supported TPM shutdown
control socket, and that cleanup was verified. A temporary orphan test daemon was
cleaned up by the user.

Code review completed as run `20261008-040555-a69a7cb7`. All five findings were applied
inline: lifecycle serialization, sealed shutdown protection, startup TPM cleanup,
force recovery without QMP, and shutdown eligibility coverage. No findings remain
unresolved. Claude dispatch failed with an OAuth refresh conflict; the review's local
adversarial fallback completed. The runtime resolution record is
`work/windows/review-resolution.json`.

The existing `ctest --preset dev` suite passed 25/25 on the actual host. The earlier
sandbox run could not use graphical/D-Bus/local fixture sockets; those failures did
not reproduce on the host. This VM work changes no C++ app behavior. Windows native
app build/capture/clipboard/share acceptance remains outside this provisioning work
and has not been claimed.
