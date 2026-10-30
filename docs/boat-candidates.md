# Exact candidate builds on Boat

Run the controller on the workstation; compilation, dependency provisioning and
UI catalog rendering run in disposable Boat workers. This is an opt-in build
placement that keeps physical acceptance hosts quiet. It provides package and
offscreen catalog evidence, not desktop, hardware or release acceptance.

Nonce leases and size-scaled collection arrived in
[code PR #753](https://github.com/patrick-hudson/put.gd-desktop/pull/753).
The original controller arrived in
[#685](https://github.com/patrick-hudson/put.gd-desktop/pull/685).
Also require authenticated `boat`, OpenSSH (the Boat CLI's local
`~/.ssh/ascii_box_ed25519` identity), Git, Python 3 and the distro's
`python3-yaml` package. The public candidate commit must be pushed and available
in the local Git object database. The `putgd-build-base-v3` named snapshot
must be ready. No local build toolchain, Docker daemon or AWS credentials are
needed by this command.

```sh
set -eu
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
artifact, from `putgd-build-base-v3`. Each has `--no-env`, an explicit auto-stop
TTL (default 10,800 seconds), and a controller deadline 180 seconds before that
TTL for hydration/build. Collection retains the unused build budget or a size-scaled reserve
(150 seconds + staged bytes / 128 KiB/s + 0.05 seconds per file, rounded up),
whichever is greater, capped at the sandbox TTL measured conservatively from
worker startup. An exhausted TTL fails collection with an explicit reason.
Cleanup follows even when either phase fails.
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
  `/usr/bin` takes precedence in `PATH`. No weekly native package snapshot or
  its offline install fallback is available.
- AppImage uses the pinned Ubuntu 24.04 native container rather than GitHub's
  mutable `ubuntu-24.04` hosted VM. It runs `hosted_apt_mirrors.py`, installs Qt
  through fresh aqtinstall 3.3.* / py7zr 1.1.0 rather than the cached Action, and
  exports `QT_PLUGIN_PATH`, `QML2_IMPORT_PATH` and `PKG_CONFIG_PATH`
  (appending Qt’s `lib/pkgconfig` after existing entries, as install-qt-action does). Python is bootstrapped
  before the mirror helper because the minimal container does not preinstall it.
  The retained package inventory identifies the actual distro packages. Hosted preinstalled desktop links can
  differ; inspect `appimage-build.json`'s `rootDesktopEntry.removedLinks`. The
  cold container uses `install-packages.sh` defaults: four attempts, 900-second
  overall and 600-second stall limits per attempt, with distro/manager labels.
  The AppImage CI dependency step instead sets three attempts, 300 seconds
  overall and 120 seconds stalled, with an AppImage label.
- UI catalog uses the package matrix's pinned Ubuntu 26.04 digest; CI's linux
  job uses the floating `ubuntu:26.04` tag. The dependency and build run blocks
  are reused (`PUTGD_UI_CATALOG_REFERENCES` resolved for 26.04); CI renders the
  catalog inside CTest, so the worker renders it directly and runs the surface
  map check. Reference-image comparison is CI's (see [the UI catalog](ui-catalog.md)). The private compiler cache is
  capped at 256 MiB rather than CI linux's 1 GiB.
- The fixture package's Debian **Package** field and filename are
  `putgd-lab-fixture`, not `putgd`. Its package target is empty and
  `PUTGD_UPDATE_CHECK=OFF`; it cannot advertise a production update target.
  It declares `Conflicts: putgd` and `Replaces: putgd` because it ships the
  same application paths. Both the package summary (the first `Description`
  line shown by `dpkg -l`/APT) and long description identify disposable test use.
  Install
  either package in a disposable environment; the two cannot coexist.
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
transport archives and failed collection evidence are retained. Packing and transfer
receive the remaining size-scaled collection budget, capped at the sandbox TTL.
Hash verification and restoration check that deadline between archive members and
1 MiB chunks and on completion; a single local filesystem call can overrun before
the next check. An overrun fails the worker, retains written local evidence and
still attempts worker cleanup (which has its own recorded outcome). Receipts record
`collectionInput` (bytes, file count, filesystem and I/O pressure),
`collectionBudgetSeconds` (after the TTL cap), `collectionTtlCapped`, and
`collectionPhases` (probe, packing, transfer,
verification/restoration). The transport remains uncompressed `tar -cf`; packages
and PNGs are already compressed. This exposes slow storage/network phases
without guessing from an aggregate duration. For a before/after measurement
on identical catalog bytes, add `--benchmark-catalog-scp`: it first copies the same
output with legacy recursive SCP, then records `legacyScpSeconds`, the batched
`collectionSeconds` and an equality check of both per-file inventories. A failed
optional benchmark records `legacyScpStatus: failed` and `legacyScpError` while
still attempting primary archive collection; a partial legacy tree is not compared.
The completed manifest and checksums are saved before final status output;
a closed stdout does not turn a completed run into a missing manifest.

```sh
cd "/home/patrick/work/boat-candidates/${sha}-run1"
sha256sum -c SHA256SUMS
```

The controller deletes its workers after collection, including failure and
SIGINT/SIGTERM/SIGHUP paths. Each Boat/SSH child starts in a separate session so
terminal Ctrl-C reaches the controller; repeated signals request cancellation
without interrupting cleanup. Each run has a random 16-hex nonce and names
workers `putgd-<sha12>-<target>-<nonce>`; concurrent runs of the same SHA/target
are supported. A controller holds a shared cross-worktree `flock` lease under
`~/.local/state/putgd/boat-candidate-runs/<nonce>`. Lease files remain after exit
so startup or `--cleanup` can identify dead local controllers. Never unlink
these files while controllers run, since unlinking splits the lock namespace.

Startup cleanup deletes workers with dead local nonce leases, or workers with
unknown leases and expired Boat creation-time TTLs. A held local lease always
wins over TTL expiry. Unknown leases (including controllers on another
workstation) are preserved until TTL expiry. Legacy
fixed names have no lease and are preserved until TTL expiry. If `createdAt` or
`ttlSeconds` is missing or invalid, unknown and legacy workers are kept; missing
metadata never authorizes deletion. The real API returned both fields in the
[issue #752 live campaign](evidence/2026-10-30-boat-candidate-752-live/README.md).
Orphan sweeps
always delete by the exact eligible ID, preserving a fresh legacy worker even
when it shares an expired worker's name. Each run’s final cleanup uses its own
exact nonce name/ID. When creation reported an ID, deletion starts immediately
without waiting out the create timeout. Without an ID, cleanup observes the longer
of the remaining original 120-second `boat new` window and 30 seconds after the
CLI stops, including a timeout kill, plus up to 120 seconds for deletion. This is
a bounded observation window for late creates; it cannot prove that the service
will never create a worker afterward. With the F1 fix in
[code PR #766](https://github.com/patrick-hudson/put.gd-desktop/pull/766), a create
that never printed an ID records `cleanupVerified: "qualified"`,
`lateCreateRuledOut: false`, and a `cleanupQualification` reason, even when
name-based recovery found and deleted a worker. `cleanup.verified: true` then
means absence was observed during that bounded window. A passing manifest
requires literal `cleanupVerified: true`; qualified names remain in
`ownedSandboxesRemaining` as unresolved possibilities, rather than proof that
a worker is still present. A later sweep or `--cleanup` reconciles any worker
that appears after the observation window. Cleanup failures leave
`cleanupVerified: false` and a `cleanupError` reason.

Deletion receipts retain the API's initial operation status (which can be
`pending`) separately from final `cleanup.finalState: absent`. The controller
polls **`boat list --all`**, including stopped/pending workers, until its owned
IDs/names disappear. It leaves named snapshots and unrelated names alone. An
empty account-wide list is expected only when no other session owns a worker.

SIGKILL, workstation loss or an unavailable Boat API can leave TTL-stopped
sandboxes. Auto-stop bounds compute, but does not delete their disks. Recover
only this commit’s eligible orphan workers: dead local leases or expired unknown/legacy
workers. Preserve held local leases, unexpired unknown/legacy workers and unrelated workers:

```sh
set -eu
printf 'Full 40-character commit SHA from the failed manifest: '
read -r sha
python3 tools/lab/boat/candidate.py --cleanup "$sha" \
  --output "/home/patrick/work/boat-candidates/${sha}-cleanup1"
```

Cleanup needs no checkout of the candidate's workflows or build capacity; it
writes `cleanup.json` with deletion requests, verified absence of deleted workers,
and IDs preserved because they are live or have unknown leases within TTL.
A successful cleanup does not mean preserved active runs are absent. Use a
new output directory each time. Preserve logs/receipts before retrying and follow
[evidence retention](evidence-retention.md).

## Lab snapshots

New lanes use `putgd-build-base-v3`, `putgd-lab-cinnamon-v3` and
`putgd-lab-ubuntu-gnome-v4`. Their [repeatable recipe](boat-snapshots.md) and
[campaign evidence](evidence/2026-10-30-boat-snapshots/README.md) record immutable
IDs, source/new golden hashes, package inventories, cached build commits,
bootstrap pins, cleanup receipts and fresh-fork results. Read the authoritative
`tools/lab/boat/manifests/NAME.json` for the exact image path and package target.

| Snapshot | App package target | Host target |
| --- | --- | --- |
| `putgd-build-base-v3` | `ubuntu24.04` | `ubuntu24.04` |
| `putgd-lab-cinnamon-v3` | `ubuntu24.04` (Mint 22.3 / Noble) | `ubuntu24.04` |
| `putgd-lab-ubuntu-gnome-v4` | `ubuntu26.04` (Qt 6.10) | `ubuntu24.04` |

Build the GNOME guest DEB with `candidate.py COMMIT --artifact ubuntu26.04` in
its pinned target container. A DEB packaged directly from the Ubuntu 24.04
host tree cannot replace it. The snapshots retain audited matching packages
under `/home/user/snapshot-packages/TARGET/`. Each recommended snapshot also
contains a clean built nightly tree; its exact commit is recorded, and it is a
cache to update to the requested candidate, not a release asset.

The build-base persists automatic private tmpfs scratch through
`PUTGD_CI_TMPDIR=/dev/shm` and `TMPDIR=/dev/shm`; the [supported runner](boat-snapshots.md#fast-scratch-and-a-plain-full-ctest-run)
uses scale 3, the CI resource spec and parallel 12, with retries off. Fresh
forks need no manual mount or scratch setup. Cinnamon's new golden disables
Mint Update's autostart through a user XDG override, so verification needs no
per-overlay Update Manager kill.

All original names remain intact, as do the intermediate
`putgd-build-base-v2`, `putgd-lab-cinnamon-v2` and `putgd-lab-ubuntu-gnome-v3`.
They are superseded for new build/Cinnamon/GNOME work. Every selected golden
is below `/home/user/work/put.gd/work/lab-images`; never use the legacy
`/home/user/putgd-gnome-golden/image.qcow2` path from an old snapshot.
Use `manifest.golden.path` and its SHA-256, the inherited bootstrap pin and a
fresh overlay, then reset and run guarded cleanup with the manifest's
`imagesRoot`. See the [Boat image inventory](remote-golden-images.md#probe-equipped-boat-copies).

These are Boat development caches. They establish no physical-desktop,
performance, memory or release acceptance. Before GUI work verify storage,
KVM, socket paths, the VNC tunnel listener and the live wall; retain saved
visuals. Wait for full hydration, use `--no-env` and a finite TTL, and delete
every worker after exporting verified evidence. Never overwrite a named
snapshot or boot, modify or delete a source golden.

Aurora v2 is deferred in [#769](https://github.com/patrick-hudson/put.gd-desktop/issues/769):
its Toolbx/overlay probe route still needs validation without an rpm-ostree
change to a golden. Its original named snapshot is preserved.
