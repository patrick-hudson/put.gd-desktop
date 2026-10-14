# Replay tray progress on the disposable desktop lab

`tools/lab/tray_progress.py` drives the production JSON CLI, the existing native
desktop fixture and the genuine StatusNotifier host in a pre-provisioned Linux
guest. It creates a new private `/tmp/tp-<id>` config/data/runtime/archive root.
It never provisions credentials, installs packages, unlocks the console or
starts/stops a VM. The operator owns those steps through [the VM lab](vm-lab.md).
This document describes a replay path, **not a record that it has passed**.

The runner returns `protocol-passed-rendering-pending` only after its automated
assertions pass. That is not desktop acceptance. Inspect the QMP hover frames
and the live console for visible icon color, readable tooltip, appropriate
placement and absence of clipped/stale rows. Record the inspector, exact source
and package hashes, profile, screenshot filenames, result and any limitations in
the campaign's acceptance receipt. A host-only Python pass or mocked DBus host
cannot replace this inspection.

## Candidate and guest preparation

Start with Cinnamon/X11 functional **and memory** checks on one candidate. If
either required gate fails, fix or reject it before building/testing its GNOME
or KDE siblings. After Cinnamon passes, independent native GNOME/Wayland and
KDE/Wayland checks may run in parallel on the same source. The approved KDE-first
resident-memory experiment exception in `AGENTS.md` does not change this tray
candidate's normal Cinnamon-first path. Use [VM memory evidence](testing-vm-memory-audit.md)
and [the frozen memory protocols](testing-memory.md); this functional runner
does not invent a replacement memory budget. Wait for build/GUI work to stop
before quiet memory sampling.

1. Inventory host QEMU processes, run/base metadata and reserved SSH/VNC ports.
   Run [capacity preflight](lab-storage.md). Preserve immutable golden disks:
   launch only fresh overlays under `work/lab-runs/` with `tools/lab/lab.py`.
   Never boot or modify a referenced base. Check the complete host QMP socket
   path with `tools/diagnostics/check_socket_paths.py --socket` before launch.
2. Build the exact candidate with normal presets and official SDK provisioning
   from [build.md](build.md). Use C++20 and each distro's Qt 6.4.2+ libraries.
   Include `-DPUTGD_LAB_FIXTURE=ON` in the lab package configuration. S3 uses the
   packaged official AWS runtime/module. Proton additionally needs the pinned
   bundle provisioned before CMake and `-DPUTGD_BUNDLE_PROTON=ON`; see
   [Proton runtime instructions](../tools/proton/README.md). Do not substitute
   another compiler, Qt build, SDK or desktop backend.
3. Build/install a distro-native package in the disposable guest. Mint, Ubuntu
   GNOME and Kubuntu KDE need their own DEB/Qt ABI; Fedora needs its own RPM/Qt
   ABI. A Mint package is not evidence for the other distros. Retain package
   SHA256, embedded app/CLI build manifests, installed app/CLI/UI/helper/module
   hashes and distro package versions. On Debian guests, `lab.py install`
   creates `package.json` and compares installed bytes against the package.
   For a native RPM installation, retain the equivalent receipt schema:

   ```json
   {
     "sha256": "<actual archive SHA256>",
     "filename": "<actual package basename>",
     "installedBinarySha256": {
       "/usr/bin/putgd": "<actual packaged/installed SHA256>",
       "/usr/bin/putgd-cli": "<actual packaged/installed SHA256>",
       "/usr/bin/putgd-desktop-fixture": "<actual packaged/installed SHA256>"
     }
   }
   ```

   Include the remaining product binary hashes when available; the runner
   verifies every path in this receipt, not just these three examples. Native
   package-manager installation must resolve its dependencies normally. This
   runner does not add an RPM installer to `lab.py`.
4. Keep the VM's localhost VNC listener and display wall at
   `http://127.0.0.1:8765/` active. Restore a locked session through its visible
   console using the owned/private protected login input identified by existing
   bootstrap metadata. The operator reads it only in memory and types into the
   visibly focused masked field. Do not print it, put it in argv, photograph it,
   or use `credentials.txt` as a login source. Visually verify the unlocked
   native desktop before acceptance. SSH build/log work is separate evidence.
5. Stop any previous lab `putgd` resident explicitly after its work is settled.
   The replay refuses an already running resident and does not kill one to
   make the precondition pass. Guest consoles remain visible throughout.

Git-observed builds must be clean and carry the full requested commit. An
exported source archive may correctly report `sourceProvenance: supplied` and
`dirty: null`; that unknown dirty observation remains in the receipt. Exact
source/archive producer evidence and package/binary hashes establish the
exported candidate identity. App and CLI embedded manifests must match. An old
process surviving a package update is rejected by its live executable hash.

## Native StatusNotifier probe

Build the standalone `tools/lab/tray_probe` program in each target distro using
standard system Qt development packages, CMake and Ninja. Ubuntu/Mint packages
include `qt6-base-dev` and `qt6-base-dev-tools`; Fedora uses `qt6-qtbase-devel`.
Its Qt Core/DBus dependency stays dynamically linked. Administrator
authentication for missing packages belongs to the user. App builds still use
the repository presets; this tiny separate probe does not change root CMake.

```sh
cmake -S tools/lab/tray_probe -B build/tray-probe -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/tray-probe
sha256sum build/tray-probe/putgd-tray-probe
ldd build/tray-probe/putgd-tray-probe
```

Copy that verified probe to a known guest absolute path, keep its actual guest
hash and supply it below. The probe reads the real session bus's
`org.kde.StatusNotifierWatcher` host flag and registered-item list. It resolves
the DBus connection Unix PID, selects exactly one `putgd` item for the resident
PID and reads its real `ToolTip` and network-order ARGB `IconPixmap`. It checks
the watcher owner/PID again before returning. It neither registers a mock
watcher nor drives an offscreen tray. Missing hosts or ambiguous registration
fail the replay; on GNOME, the declared native tray integration must already be
installed and active.

## Isolated destination and credential references

The operator provisions a disposable guest keychain before the replay. The
runner reads only the supplied nonsecret settings patch, whose selected profile
must contain `credentials: {"mode":"keychain","reference":"..."}`. It takes
no password, session, token, credential file or secret on argv or through its
settings patch. Do not copy secrets into golden disks, source exports, evidence
or review packets. For this campaign the user authorized live S3 test input and
copying **only put.gd's Proton session data** to disposable guest keychains;
that authorization does not cover other host accounts or keychain entries.
Secret provisioning remains an operator-owned, bounded step.

The settings file is a production `settings.patch` object with a `profiles`
array. Other nonsecret settings may also be included. The selected profile must
have the requested provider and a unique isolated test destination:

- S3: a new `objectPrefix` containing `--isolation-marker`, in the authorized
  synthetic bucket destination. Supply ordinary valid S3 settings, a working
  public URL base and the pre-provisioned keychain reference. For root-only test
  credentials, use an empty `objectPrefix` with `naming.mode: template`,
  `collisionPolicy: rename`, and all four filename templates set exactly to
  `<isolation-marker>-{uuid}.{ext}`. This isolates new conditional root keys; it
  never broadens IAM permissions or replaces existing objects.
- Proton: a newly provisioned synthetic test folder whose `folderName` contains
  the marker, with its real `accountRootUid`/`folderUid` and put.gd keychain
  reference. `folderName` is a display hint; the operator must establish that
  the supplied UID is this newly created folder. Use no sharing password unless
  its exact reference was separately provisioned within authorized scope.

The runner requests shared clipboard results, generated names and rename
collisions; it does not overwrite existing objects. It does not modify bucket
policy, versioning, retention or existing user objects. It verifies the accepted
settings revision and explicitly invokes/polls production `setup.unlockCredentials`
for the selected S3 keychain entry before invoking work. Proton reads its own
session through the bundled helper. Provider capability and diagnostics
inspection are saved before the first action.

## Run and inspect

Use new private output directories. Before starting either app or fixture, the
runner records/checks the guest session socket including its `r/putgd/session.sock`
suffix, the existing host QMP socket, and the native guest Wayland/DBus filesystem
socket paths against Linux's 107-byte UTF-8 limit. It preserves the genuine
desktop environment and converts a relative Wayland display socket to its
absolute native path before isolating `XDG_RUNTIME_DIR`. No ownership check or
frozen performance harness is weakened to fit an artifact directory name.

Determine the tray's head-zero pixel coordinates through the visible native
console/QMP frame. Supply them as `--tray-x/--tray-y`; inspect the hover frames to
verify they hit the app icon. Coordinates are witnesses, not an automatic
rendering oracle. The current declared lab profiles use one native display
head; mixed-display/scaling campaigns remain separate required desktop gates.

```sh
python3 -B tools/lab/tray_progress.py \
  --run work/lab-runs/<fresh-cinnamon-run> \
  --output /tmp/tp-evidence-<new-id> \
  --settings <nonsecret-isolated-settings.json> \
  --profile-id <isolated-profile-id> --provider s3 \
  --isolation-marker <unique-destination-marker> \
  --expected-commit <full-candidate-commit> \
  --expected-package-sha256 <actual-distro-package-sha256> \
  --probe /usr/local/bin/putgd-tray-probe \
  --expected-probe-sha256 <actual-guest-probe-sha256> \
  --tray-x <observed-x> --tray-y <observed-y> \
  --payload-mib 32 --timeout 180
```

Repeat with `--provider proton-drive` and that provider's new folder/profile
patch, then with GNOME/KDE only after the Cinnamon functional/memory gates pass.
Every declared desktop/provider combination needs current-source evidence.
All CLI source imports come from the guest fixture's actual native clipboard.
The host clipboard is never used or shared.

The scenario uses two deterministic synthetic files per operation and drives:

1. Two upload operations with one upload worker, requiring active color, a
   queued tooltip row, genuine counters and eventual idle. S3 counters have a
   matching body-byte total/percentage; Proton encrypted SDK bytes have no
   denominator, remain separate from source size and may decrease on SDK retry.
2. An explicit same-ID result copy pasted into the independent native guest
   fixture. Pasted links must equal original receipts. S3 additionally retrieves
   public bytes and checks exact length/hash; URLs are read in guest memory
   through the production CLI and never interpolated into SSH/process argv.
   Proton proves confirmed upload/share receipts and native link paste; it does
   not claim an independent decrypted public download from its share page.
3. Two concurrently admitted operations after configuring two workers.
   Core activity rows must show both active operations. Proton serializes helper
   requests for the same keychain reference, so this proves concurrent app
   admission/activity, not parallel SDK transfers for one account.
4. Cancellation after a real transfer counter is observed, terminal cancelled
   state and idle, then explicit production `operation.retry` with the original
   operation, retained artifact and publication IDs. Unknown publication is
   reconciled by that retry; a new action never conceals it.
5. A local-only file batch whose new private archive root is intentionally a
   regular file. Its real save must fail. Replacing that owned blocker with a
   directory permits `operation.retryLocalSave` on the same operation; actual
   saved bytes must match independent source hashes and stay local-only.
6. Resident quit/restart, a new matching process identity, exact preserved
   history receipts and the original idle icon hashes/empty tooltip.

CLI and SNI reads are separate, and the product batches observations. Counter
assertions use before/after CLI reads plus a two-second recent observation
window. Rounded tooltip bytes must fit real observed byte bounds and their own
integer percentage. No estimate from elapsed time or source size is accepted.
The SNI phase must have a matching production observation. Source filenames,
paths and links are prohibited in tray text.

The runner records finalizing/sharing as `observed` or `not-observed` and retains
phase screenshots only when actually seen. A phase that completes between
samples does not become a passing phase gate by inference. Local retry may
finish before a tooltip observation because its CLI call awaits completion;
the saved-byte/terminal checks still prove that local-save case. For phase
coverage that remains required, replay a larger synthetic workload and inspect
the visible console, retaining every earlier attempt. A run too fast to show
queued/concurrent work or cancellation fails instead of fabricating those
observations. Preserve its full output, record the corrected command, and use
a new output/root/destination marker before retrying. Do not alter a frozen
memory harness or relax a gate to get a pass.

## Native host restart, cleanup and evidence

The replay restarts the **resident app**. Native tray-host restart is a separate
manual gate because Cinnamon/GNOME/KDE host lifecycles differ and some restart
paths terminate the graphical session. While a large synthetic upload is active,
use the desktop's supported visible host/session recovery workflow where
available. Retain old/new watcher owners, the unchanged app PID, live SNI
properties after re-registration and QMP active/idle hover frames. Confirm that
the newly registered host displays the current activity and eventual idle,
without reopening a view. If a desktop cannot provide this workflow safely,
record that gate as blocked. Do not stand up a fake watcher or restart a
Wayland compositor blindly to claim host-recovery evidence.

Each run preserves parameters, socket preflight, source identities, package and
running-process identity, native readiness, capabilities/diagnostics, redacted
CLI observations, SNI icon hashes/colors and tooltip rows, scenario outcomes,
actual local synthetic bytes, QMP hover/source/paste frames and a SHA256 manifest.
The app/fixture and VM are left running and visible for inspection. Failures
retain their roots and known operation IDs; the operator inspects those exact
operations before recovery or cleanup. Do not infer that a failed driver means
an upload had no effect.

`cleanup-preparation-private.json` contains exact newly created history IDs and,
for S3, actual original destination/effects/acknowledgement from `delete.prepare`.
It is a private operator handoff and may contain URLs/references; exclude it
from review packets and default diagnostic exports. The runner never calls
`delete.confirm`, sharing revoke or account-file deletion. The operator reviews
and acknowledges only that exact ordered S3 selection through production
services, then records each actual outcome. Proton `delete.prepare` must return
`unsupported_capability`; its synthetic account files remain in the unique test
folder because safe revision-guarded deletion is unavailable. Link revocation
is a separate mutation and does not remove those files.

Windows S3 acceptance uses independent overlays and the native Windows route in
[windows-vm-lab.md](windows-vm-lab.md). This Linux SNI/QMP runner is not Windows
tray proof. The currently pinned Proton runtime supports Linux x86-64 only;
Windows Proton acceptance is blocked until a supported runtime/package exists.
Neither those platform routes nor a service-session exporter is added here.

Pure host contracts can be run independently without builds, guests or service
access:

```sh
python3 -B tests/desktop/test_tray_progress_lab.py
```

They check the replay's socket, identity, privacy, color, accounting and
nonsecret-settings assertions. They do not establish a probe build, live S3 or
Proton success, a desktop functional pass, human rendering inspection or memory
acceptance. Record those actual executions separately.
