# Exact candidate builds on Boat

Run the controller on the workstation; compilation, dependency provisioning and
UI catalog rendering run in disposable Boat workers. This is an opt-in build
placement that keeps physical acceptance hosts quiet. It provides package and
offscreen catalog evidence, not desktop, hardware or release acceptance.

Prerequisites: authenticated `boat`, OpenSSH (the Boat CLI's local
`~/.ssh/ascii_box_ed25519` identity), Git, Python 3 and the distro's
`python3-yaml` package. The public candidate commit must be pushed and available
in the local Git object database. The existing `putgd-build-base` named snapshot
must be ready. No local build toolchain, Docker daemon or AWS credentials are
needed by this command.

```sh
git fetch origin
sha=$(git rev-parse origin/nightly)
python3 tools/lab/boat/candidate.py "$sha" \
  --output /home/patrick/work/boat-candidates/"$sha"-run1
```

Pass a full 40-character commit SHA. The output directory must be new and
outside every Git checkout, including ignored `work/` directories. It is made
private. The command rejects insufficient Boat capacity or start allowance
before creating workers; it does not stop another session's workers.

By default the command fans out seven `large` workers (8 vCPU, 16 GB), one per
artifact, from `putgd-build-base`. Each has `--no-env`, an explicit auto-stop
TTL (default 10,800 seconds), and a controller deadline 180 seconds before that
TTL for hydration/build. Collection retains the unused build budget or a
150-second reserve, and cleanup follows even when either phase fails.
Use `--type default` for the 4-vCPU/8-GB size, or `--ttl SECONDS` (600–14,400)
for a deliberate bounded run. A small TTL can expire during cold provisioning.
The command waits for `hydrated: true` before accessing a worker and reads the
SSH host key through authenticated `boat exec`, then uses strict host-key
verification for all SSH/SCP traffic. The private SSH key and agent stay local.

| Artifact | Exact candidate's recipe | Intended rows |
| --- | --- | --- |
| AppImage | Locked Qt/runtime, CI provisioning and `build-appimage.sh` | Mint, Fedora, Aurora and other AppImage rows |
| Ubuntu 24.04 DEB and source archive | Native production package/audit and curated source export | Mint 22.3; Ubuntu 24.04; source |
| Ubuntu 26.04 DEB | Native production package/audit with KDE helper | Kubuntu 26.04 and Ubuntu 26.04 GNOME |
| Debian 13 DEB | Native production package/audit with KDE helper | Debian 13 |
| Fedora 44 RPM | Native production package/audit with KDE helper | Fedora 44 |
| `ui-catalog/` | Ubuntu 26.04 CI dev build, offscreen renderer and surface-map check | Agent surface map |
| Lab-fixture DEB | Ubuntu 24.04 release preset with `PUTGD_LAB_FIXTURE=ON` | Mint/Ubuntu 24.04 guest drivers requiring `putgd-desktop-fixture` |

The controller reads the requested commit's `.github/workflows/package.yml`
and `ci.yml` from Git, takes the native image digests from that matrix and reuses
the CI shell run blocks and in-repository provisioning/package scripts. All
builds use fresh public checkouts with full Git history and tags. Dependencies
and application products are built cold; the pilot's SDK/build tree is not
transplanted into distro containers. AppImage Qt is installed with the CI
install-qt-action's aqtinstall/py7zr versions and the candidate's Qt lock.
The AppImage source bundle, debug-symbol delivery, SBOM/scanning, full CTest and
release publication are separate CI gates; this command does not certify them.
The Ubuntu source archive is the curated application source, not a complete
lab checkout.

For a bounded retry or selected preparation, repeat `--artifact` with any of
`appimage`, `ubuntu24.04`, `ubuntu26.04`, `debian13`, `fedora44`, `ui-catalog`,
`lab-fixture`. Omission builds all seven. A retry uses a new output directory and
retains the failed attempt. Duplicate selections are rejected.

## Collected evidence and cleanup

`manifest.json` records the requested commit, named-template snapshot identity,
placement reason, sandbox IDs, CPU/RAM/provider/SSH placement, image digests,
hydration/build/collection/total wall times, built version identity, artifact
sizes and SHA-256 hashes, failures and deletion receipts. `SHA256SUMS` checks
all successfully collected artifacts, including each catalog PNG. Each artifact
subdirectory also retains its generated shell recipe, bootstrap, build log,
creation events, pinned host key and receipt. Package audit receipts and the
compiled CLI's `--version-json` accompany `build-info.json`; source commit and
cleanliness must match the requested SHA before success is recorded.

```sh
cd "/home/patrick/work/boat-candidates/${sha}-run1"
sha256sum -c SHA256SUMS
```

The controller deletes its workers after collection, including failure and
SIGINT/SIGTERM paths, then verifies their absence from `boat list`. It leaves
named snapshots and unrelated sandboxes alone. An empty list is expected only
when no other session owns a worker. A hard controller kill or unavailable Boat
API can require cleanup: use only the IDs in this run's receipts or `created`
events, inspect ownership, then `boat delete ID --yes` and verify the list.
Auto-stop bounds compute if the controller disappears; it does not by itself
prove deletion. Preserve logs/receipts before retrying and follow
[evidence retention](evidence-retention.md).

## Lab snapshots

The pilot's `putgd-lab-cinnamon` contains the Cinnamon golden and its historical
built tree. `putgd-lab-ubuntu-gnome` is prepared from the same build-base template
with the provisioned Ubuntu GNOME/Wayland golden and a separately identified
built tree. Snapshot preparation does not boot a golden or establish a desktop
pass. Fork a lab snapshot with `--no-env` and a finite TTL, wait for hydration,
and create disposable overlays using the existing [VM lab](vm-lab.md) commands.
The GNOME snapshot's preparation receipt is
`/home/user/putgd-gnome-golden/snapshot-receipt.json`; its immutable disk is
`/home/user/putgd-gnome-golden/image.qcow2`, and the host build tree is
`/home/user/gnome-source`. That tree and fixture DEB target the Ubuntu 24.04
sandbox host. Use the `ubuntu26.04` production artifact for the Ubuntu 26.04
GNOME guest.
Before graphical work, meet the storage, socket, bootstrap, display-wall and
saved-visual requirements in that guide.

Restore goldens directly from the private S3 inventory using short presigned
GETs passed only through mode-0600 curl configuration files. Never place signed
URLs in command arguments, logs, receipts or source; remove the configurations
after transfer. Verify the S3 object's compressed SHA-256, the inventory's image
SHA-256, standalone qcow2 metadata, virtual size, sidecar and `qemu-img check`
before saving a new named snapshot. Do not overwrite a named golden snapshot
without checking its ownership and purpose. Preserve immutable sources.

Aurora Fedora 45 has no S3 golden copy as of the 2026-10-29 inventory check.
[`#665`](https://github.com/patrick-hudson/put.gd-desktop/issues/665) tracks the
blocked `putgd-lab-aurora` preparation. The builder copies were not transferred.
A verified S3 copy and sidecar must be approved/published before that snapshot
can be prepared. See [golden inventory](remote-golden-images.md).
