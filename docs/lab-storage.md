# Lab storage and retention

Prefer the [remote Fedora/Debian hosts](remote-builders.md), then fall back to
local VMs with a recorded reason. Apply the checks below separately on every
actual build/VM host. `storage_preflight.py --host-path` measures the machine
running the controller, not an SSH builder elsewhere. Run the controller on the
remote QEMU host for its host/guest checks and export the receipt. Include copied
goldens, transfer staging, overlays, build outputs and retained evidence in that
host's capacity budget; workstation free space does not establish remote capacity.

Before a build or graphical campaign, record available space on the host and
each guest. Require at least 10 GiB host reserve, 2 GiB guest reserve, and more than 1,024
free inodes. Add the anticipated build size to the reserve before starting a
new guest build; measure a previous comparable build rather than assuming the
minimum reserve is enough. Budget that estimate for every guest's growth on the
shared host filesystem too. Run a separate preflight with `--host-path` for each
filesystem if VM overlays live on different volumes. A failed check blocks expansion until cleanup or
capacity provisioning succeeds. Record another check after the campaign.

The read-only `storage_preflight.py` accepts repeated `--run` directories,
`--expected-build-gib`, and an exclusive `--output` JSON receipt. It uses the
existing private SSH identity and strict host verification. It prints numeric
filesystem capacity only; SSH stderr is withheld. It installs no packages and
changes no desktop settings, disks, or sessions.

Run from the repository root with a new private output directory. For a test
that uses an already-built package, leave the build estimate at zero:

```sh
capacity_output=$(mktemp -d work/lab-capacity.XXXXXX)
python3 tools/lab/storage_preflight.py \
  --run work/lab-runs/p2g-a1 --run work/lab-runs/p2k-a1 \
  --output "$capacity_output/preflight.json"
```

Before building, add `--expected-build-gib` with the measured estimate for one
guest build. The tool adds that estimate to every guest home reserve and sums
it across guests for the shared host reserve. Guest `/tmp` has a separate
2 GiB reserve. Exit2 means capacity or identity verification failed; do not
start the campaign. Keep the receipt, correct the problem, and run again with a
new output filename. Hosts with multiple overlay filesystems require separate
invocations with matching `--host-path` values.

Retain the current guest build and at most one explicitly identified rollback
build. Before deleting an obsolete build, archive its build identity, source
commit/tree, CMake cache, dependency paths, JUnit/CTest output and acceptance
receipts, with a SHA256 manifest. Confirm source availability in host Git and
verify there are no current SDK, RPATH, symlink, active cwd, executable, mapping
or open-file references. Remove only the exact checked generated directories.
Keep current source, SDK installation, packages, profile/history, unresolved
publication records, protected login input and unknown files. Build retention
does not authorize deleting user data or unresolved test artifacts.

Screenshot evidence can dominate host usage. Once a campaign is obsolete and
has no active owner, losslessly gzip raw PPM frames and verify decompressed bytes
against the original SHA256 before unlinking the original. Fsync the compressed
file and append/fsync a receipt first. Keep PNG sidecars available for direct
inspection. The receipt records original and compressed hashes, byte sizes,
allocated sizes, original permissions and mtime. Keep active campaign artifacts
untouched. Compression preserves evidence; it is not permission to discard
failed attempts or change a frozen test harness.

`restore_evidence.py --manifest MANIFEST --root EVIDENCE_ROOT --original PATH`
restores one archived frame. It validates both hashes and exact decompressed
size, refuses existing originals with mismatched bytes or metadata, creates the restored path
exclusively, preserves mode/mtime and keeps the compressed copy. Restoration
requires free space for the original plus 64 MiB. Do not bulk-restore all frames
onto a nearly full filesystem.

Host-only contracts run without guests or credentials:

```sh
python3 tests/desktop/test_storage_preflight.py
python3 tests/desktop/test_restore_evidence.py
```

The existing Linux CI lab-contract step runs these alongside its protocol
contracts and retains commands, exit codes and logs. This checks tooling safety;
it does not establish graphical desktop acceptance or actual capacity in a VM.

Never infer a disk is disposable from its VM name or stopped status. Inventory
every qcow2 backing chain and live QEMU open disk reference before retiring a
disk. Current golden images may themselves depend on an older run directory.
Protect the complete transitive backing graph. Do not boot, edit, move, overwrite
or delete referenced bases; Linux and Windows immutable-base rules still apply.
Guest filesystem deletion can free guest space without reclaiming host qcow2
allocation. The current Linux drive launch lacks discard forwarding; do not
attempt live conversion, rebase or base modification to reclaim that allocation.

Future provisioning should create new standalone golden images and evaluate
discard-enabled disposable overlays. Validate the complete new chain, package,
identity and graphical behavior before migration. Retire old chains only after
all overlays and process references have been removed. This is a follow-up;
the storage cleanup below changed no VM images or desktop power settings.

## 2026-10-12 cleanup

GNOME was 99% full with 618 MB available. Three obsolete source/build candidates
were archived and reference-checked, reclaiming 4,441,849,856 bytes; free space
rose to 5,059,821,568 bytes. KDE's five obsolete build directories reclaimed
4,892,082,176 bytes; free space rose to 8,867,090,432 bytes. Current source108,
SDK, packages, profiles and original failures were preserved.

Host compression covers 5,052 obsolete raw PPM frames under retired UI43 and
current-a3d70dc campaign directories, preserving their numeric receipts and PNGs.
Refer to `work/s3-debug/storage-compression-result.json` and its per-file JSONL
manifest for completed totals and exact restoration identities. These private
local receipts are distinct from source-controlled release evidence.

Read-only image inventory found that the retired GNOME and KDE disks still
back the provisioned current golden images and guests. They were protected.
Testing paused during cleanup and compression; interrupted and failed native
attempts remain recorded. Resume the unchanged suites after capacity and idle
checks, rather than relaxing timing gates to accommodate storage contention.
