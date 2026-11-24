# VM lab: overlay retirement

Part of [Disposable graphical lab](../vm-lab.md).

## Retire completed overlays

Run lifecycle commands on the actual QEMU host. A sandbox with a filtered process
view cannot certify that a VM stopped. Linux launch/reset and overlay cleanup share a host lock. Cleanup also checks actual
process/open-file identities and QEMU backing chains, identified by their image
headers rather than filename extensions. It holds a native QEMU image lock
through final verification and unlink, preventing a competing writer from
opening the disk. The process inventory must also rule out ordinary read-only
file descriptors and mappings, including those held by root or another user.
If a process is unreadable, cleanup retries the same read-only inventory with
`sudo -n -- /usr/bin/python3 -I -B`; it does not run the lab controller, import
checkout modules at elevation or delete files as root. The elevated program is
the scan function's source, captured once when cleanup is imported. A regular
user can't read `/proc/1/fd`, so in practice every host takes this path: boat's
`user` runner and all five builders already allow `/usr/bin/python3` through
`sudo -n` without a prompt. No sudo rule is installed or changed. The direct
scan alone is enough only where every process is readable, such as cleanup run
as root.
Unavailable sudo, unreadable elevated inventory, changed process identities,
`hidepid` and nested PID namespaces refuse cleanup; a native image lock alone
cannot establish that an opaque process has no read-only descriptor.
Incomplete image inventories also refuse cleanup. It refuses running,
referenced, symlinked or mismatched disks, changed evidence and unrecorded
completion. It only removes `disk.qcow2`; screenshots, logs, parameters, results,
keys, firmware/TPM state and historical metadata remain in place. The separate
`overlay-cleanup.json` records the exact disk identity and evidence digest, with
a durable prepared/completed transition and safe retry after interruption.

For a passed, fully exported standard scenario retained with `--keep`, first stop
the VM, then preview and apply cleanup:

```sh
python3 tools/lab/lab.py reset --run "$lab_run"
python3 tools/lab/lab.py cleanup --run "$lab_run" --dry-run
python3 tools/lab/lab.py cleanup --run "$lab_run"
```

Legacy journals without explicit collection-completeness records require an
explicit completion manifest after review. An automatic cleanup refusal is recorded in `overlay-cleanup-status.json`; it
does not turn a passing scenario into a test failure or silently erase its disk.
Resolve the reported ownership/dependency/evidence problem and retry explicitly.

Custom campaigns and reviewed failures need an explicit completion manifest.
Export all required failure context, source/package identities, parameters,
results and witnesses first. Keep them private. In that evidence directory save
`completion.json` containing `run` (absolute run directory), `runId` (Linux state
`id`), `completed: true` and `outcome: "passed"` or
`"failed"`. A separate manifest has `schemaVersion: 1`,
`kind: "vm-overlay-cleanup-evidence"`, those same four fields, and a `files`
object mapping relative evidence paths, including `completion.json`, to their
SHA256 hashes. Members must be owned regular files within the manifest directory,
with no symlink components or parent traversal. This records that investigation
is complete; it never changes a failed acceptance result to a pass.

```sh
python3 tools/lab/lab.py cleanup --run "$lab_run" \
  --evidence-manifest "$completion_manifest" --dry-run
python3 tools/lab/lab.py cleanup --run "$lab_run" \
  --evidence-manifest "$completion_manifest"
```

Do not place evidence manifests or private run files in release exports.
Retirement removes the old guest filesystem permanently; future tests use a
fresh run directory cloned from the preserved golden.


### Retire a pre-existing run outside the canonical root

The cleanup guard deliberately refuses an old run outside this checkout's
`work/lab-runs`. Do not bypass the guard, use a symlink, or delete its disk by
hand. Complete the following on the actual QEMU host, after its campaign and
investigation finish:

1. Export and checksum its evidence, original `state.json`, exact QEMU identity,
   backing chain and old absolute run path. Stop the exact owned VM with
   `lab.py reset --run OLD_RUN`, then verify that process has exited and no
   process holds the overlay. Inventory backing chains and confirm no other
   overlay references a disk inside the run before relocating it. Preserve all
   golden disks and their paths.
2. Choose an unused, short, symlink-free direct child of this checkout's
   `work/lab-runs`. Move the stopped run directory there as a complete bundle.
   Preserve the original state as an evidence file; update only the active
   `state.json`'s `overlay` field to the new absolute `disk.qcow2` path. Preserve
   the run ID, golden path/hash and historical results. Record both paths and
   the unchanged overlay/backing identities in relocation evidence. Never
   resume this retired run or rewrite historical acceptance records.
3. Export and verify that relocation evidence. Create the explicit completion
   manifest described above using the **new** absolute run path and original
   run ID, and include the original state and relocation record as hashed
   members. Run canonical `cleanup --run NEW_RUN --evidence-manifest MANIFEST
   --dry-run`, then the same command without `--dry-run`. For a golden in
   another checkout, supply its owned canonical `--images-root .../work/lab-images`.
   Retain the cleanup receipt alongside the evidence.

The guard still checks ownership, actual processes, all backing dependencies,
image locks and completion evidence after relocation. A stopped VM alone is
insufficient authorization to retire its disk.
