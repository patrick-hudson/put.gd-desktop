# Exact candidate builds on Boat

Run the controller on the workstation; compilation, dependency provisioning and
UI catalog rendering run in disposable Boat workers. This is an opt-in build
placement that keeps physical acceptance hosts quiet. It provides package and
offscreen catalog evidence, not desktop, hardware or release acceptance.

Prerequisites: merge [code PR #685](https://github.com/patrick-hudson/put.gd-desktop/pull/685)
first so `tools/lab/boat/candidate.py` is present. Also require authenticated `boat`, OpenSSH (the Boat CLI's local
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
verification for all SSH/SCP traffic. OpenSSH uses `-F /dev/null`, the pinned
known-host file and explicit options disabling agent/X11/port forwarding,
proxies and connection sharing. The private SSH key and agent stay local.

| Artifact | Exact candidate's recipe | Intended rows |
| --- | --- | --- |
| AppImage | Locked Qt/runtime, CI provisioning and `build-appimage.sh` | Mint, Fedora, Aurora and other AppImage rows |
| Ubuntu 24.04 DEB and source archive | Native production package/audit and curated source export | Mint 22.3; Ubuntu 24.04; source |
| Ubuntu 26.04 DEB | Native production package/audit with KDE helper | Kubuntu 26.04 and Ubuntu 26.04 GNOME |
| Debian 13 DEB | Native production package/audit with KDE helper | Debian 13 |
| Fedora 44 RPM | Native production package/audit with KDE helper | Fedora 44 |
| `ui-catalog/` | Ubuntu 26.04 CI dev build, offscreen renderer and surface-map check | Agent surface map |
| `putgd-lab-fixture` DEB and standalone helper | Ubuntu 24.04 release preset with `PUTGD_LAB_FIXTURE=ON`, empty `PUTGD_PACKAGE_TARGET`, updates disabled | Mint/Ubuntu 24.04 guest drivers requiring `putgd-desktop-fixture` |

The controller reads the requested commit's `.github/workflows/package.yml`
and `ci.yml` from Git, takes the native image digests from that matrix and reuses
the CI shell run blocks and in-repository provisioning/package scripts. All
builds use fresh public checkouts with full Git history and tags. Dependencies
and application products are built cold; the pilot's SDK/build tree is not
transplanted into distro containers. AppImage Qt is installed with the CI
install-qt-action's aqtinstall/py7zr versions and the candidate's Qt lock.
Each receipt's `ciDifferences` lists the remaining boundaries:

- Native packages use CI's pinned container digests. All workers build cold on
  Boat without Actions cache restore/save, hosted preinstalled packages or
  Actions upload steps. The compiler cache is private and capped at 256 MiB;
  `/usr/bin` takes precedence in `PATH`.
- AppImage uses the pinned Ubuntu 24.04 native container rather than GitHub's
  mutable `ubuntu-24.04` hosted VM. It runs `hosted_apt_mirrors.py`, installs Qt
  through fresh aqtinstall 3.3.* / py7zr 1.1.0 rather than the cached Action, and
  exports `QT_PLUGIN_PATH` and `QML2_IMPORT_PATH`. The retained package inventory
  identifies the actual distro packages. Hosted preinstalled desktop links can
  differ; inspect `appimage-build.json`'s `rootDesktopEntry.removedLinks`.
- UI catalog uses the package matrix's pinned Ubuntu 26.04 digest; CI's linux
  job uses the floating `ubuntu:26.04` tag. Dependency/build/render run blocks
  are reused; the full CTest suite is separate.
- The fixture package's Debian **Package** field and filename are
  `putgd-lab-fixture`, not `putgd`. Its package target is empty and
  `PUTGD_UPDATE_CHECK=OFF`; it cannot advertise a production update target.
  `lab-package-control.txt` and `update-info.json` record these choices.
  `native.py` deliberately does not audit this DEB: its production policy
  forbids fixture files and requires the package name `putgd`. The fixture
  still gets CPack metadata checks, clean commit/binary identity checks and
  per-file hashes; it is never staged for release.
- All artifacts have an untagged development identity for the selected SHA.
  Source rebuild, split debug staging, SBOM/scanning, publication and desktop
  acceptance remain separate gates.
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
sizes and SHA-256 hashes, CI differences, failures and deletion receipts.
Runner summary/env/output files and aqt scratch data stay under a container
`/tmp/putgd-runner.*` directory, outside the exported artifacts. `SHA256SUMS` checks
all successfully collected artifacts, including each catalog PNG. Each artifact
subdirectory also retains its generated shell recipe, bootstrap, build log,
creation events, pinned host key and receipt. Package audit receipts and the
compiled CLI's `--version-json` accompany `build-info.json`; source commit and
cleanliness must match the requested SHA before success is recorded.

Collection packs the staged output into one tar archive on the worker, transfers
it once over pinned SSH/SCP, verifies its SHA-256, and validates all members before
restoring them. Absolute paths, parent traversal, duplicate paths, links and
special files are rejected. Per-file inventories still bind the restored bytes;
transport archives and failed collection evidence are retained. Packing, transfer
and extraction share the finite collection budget. For a before/after measurement
on identical catalog bytes, add `--benchmark-catalog-scp`: it first copies the same
output with legacy recursive SCP, then records `legacyScpSeconds`, the batched
`collectionSeconds` and an equality check of both per-file inventories.
The completed manifest and checksums are saved before final status output;
a closed stdout does not turn a completed run into a missing manifest.

```sh
cd "/home/patrick/work/boat-candidates/${sha}-run1"
sha256sum -c SHA256SUMS
```

The controller deletes its workers after collection, including failure and
SIGINT/SIGTERM/SIGHUP paths. Each Boat/SSH child starts in a separate session so
terminal Ctrl-C reaches the controller; repeated signals request cancellation
without interrupting cleanup. At startup and after an interrupted create it
recovers workers by their exact name `putgd-<sha12>-<target>`, including a worker
whose ID was never printed. Do not run two controllers for the same SHA/target
at once: those fixed names form one recovery namespace.

Deletion receipts retain the API's initial operation status (which can be
`pending`) separately from final `cleanup.finalState: absent`. The controller
polls **`boat list --all`**, including stopped/pending workers, until its owned
IDs/names disappear. It leaves named snapshots and unrelated names alone. An
empty account-wide list is expected only when no other session owns a worker.

SIGKILL, workstation loss or an unavailable Boat API can leave TTL-stopped
sandboxes. Auto-stop bounds compute, but does not delete their disks. Recover
only this commit's seven fixed candidate names, preserving unrelated workers:

```sh
python3 tools/lab/boat/candidate.py --cleanup "$sha" \
  --output "/home/patrick/work/boat-candidates/${sha}-cleanup1"
```

Cleanup needs no checkout of the candidate's workflows or build capacity; it
writes `cleanup.json` with deletion requests and verified final absence. Use a
new output directory each time. Preserve logs/receipts before retrying and follow
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
