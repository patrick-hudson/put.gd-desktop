# VM lab: package and run

Part of [Disposable graphical lab](../vm-lab.md).

## Package and run

Configure the app with `cmake --preset dev -DPUTGD_LAB_FIXTURE=ON` and build with
`cmake --build --preset dev`. Package into a new output directory so earlier
versions cannot be mistaken for the current build:

```sh
set -eu
package_output=$(mktemp -d "$PWD/build/packages-lab-XXXXXXXX")
cpack --config build/dev/CPackConfig.cmake -G DEB -B "$package_output"
set -- "$package_output"/*.deb
test "$#" -eq 1
test -f "$1"
lab_deb=$1
sha256sum "$lab_deb"
```

Record this produced package path/hash and its build manifest; use `"$lab_deb"`
for installation. Preserve CPack's result and do not select a package after a
failed command. Build identity and installed/running binary hashes must match the
candidate under test, regardless of the filename or version label.

The fixture is a separate development executable, never an implementation adapter.
The package contains the application, CLI, desktop entry, opt-in autostart template,
private official AWS runtime, dependency lock and licenses. Qt, its platform plugins,
QtKeychain and SQLite driver come from normal guest distro packages. Development installs use apt’s `--reinstall` and compare installed application binary
SHA256 values with the requested package, even when its version is unchanged. The installed
`putgd` binary resolves private SDK libraries with RPATH; `putgd-launcher` scopes that
runtime directory for desktop startup. Complete release lifecycle, checksums and
final notice auditing require the [release runbook](../../release/release-runbook.md).

How `install` stages the package and recovers from a failure is in [install staging](install-staging.md) and [install tracking](install-tracking.md).

After inventory and clean golden image shutdown, choose a new run path and
verify that it does not exist. On a builder, replace the `--golden` path with
that host's inventory path. On the workstation,
[restore `cinnamon-x11`](../remote-golden-images.md#restore-to-the-workstation)
first. The example consumes the existing verified base; it never boots that
base directly. Verify the profile's reserved SSH/VNC ports
are free on the actual host before launching:

```sh
set -eu
lab_run_id=$(python3 -c 'import uuid; print(uuid.uuid4().hex[:12])')
lab_run="$PWD/work/lab-runs/cinnamon-$lab_run_id"
test ! -e "$lab_run"
python3 tools/diagnostics/check_socket_paths.py --socket "$lab_run/qmp.sock" "$lab_run/vnc.sock"
python3 tools/lab/lab.py profiles
python3 tools/lab/lab.py launch --profile cinnamon-x11 \
  --golden work/lab-images/cinnamon-x11.qcow2 --run "$lab_run" \
  --bootstrap-key work/lab-bootstrap/identity \
  --known-hosts work/lab-bootstrap/known_hosts
python3 tools/lab/lab.py install --run "$lab_run" --deb "$lab_deb"
python3 tools/lab/lab.py run --run "$lab_run" --scenario region --keep
```

`install` takes `--deb` for package profiles or `--appimage` for profiles with
`"install": "appimage"`; it rejects the other kind. `start --run RUN` starts the
installed resident in the native session with the same `putgd-launcher
--background` command the scenarios use, waits for the production CLI to
answer, and checks the running executable against the install receipt; use it
for smoke and `cli` checks on guests without the lab fixture.

Session commands run put.gd with private `XDG_CONFIG_HOME=~/putgd-lab/config`
and `XDG_DATA_HOME=~/putgd-lab/data`. For AppImage profiles,
`~/putgd-lab/data/applications` is a link to the session's own applications
directory (its `XDG_DATA_HOME`, else `~/.local/share`). AppRun writes the KWin
permission entries and the window-icon entries there, and the compositor reads
only the session's directory
([#523](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-23-appimage-icon-523/README.md)). put.gd's own data
stays private. An `applications/` directory that already holds files is left
as it is.

`run` stops the VM on pass, mismatch, timeout or crash and preserves the private run
directory. After a successful scenario exports its hashed evidence bundle and
finished journal event, it automatically retires the stopped, unreferenced overlay.
Failed runs and incomplete evidence collections retain their disks. A later pass
in the same guest also retains the disk if an earlier scenario failed, lacked
complete collection, or did not finish; retire it
with explicit reviewed completion evidence after investigation. Use `--keep`
for interactive investigation or a campaign that needs the guest again. `reset`
only stops an owned VM and retains its writable disk and diagnostics; a fresh run
directory creates an independent overlay. Repeat with another new run path to prove two independent runs. Do not reuse
a running golden disk. The guest app uses isolated `~/putgd-lab/config` and `data`
settings/history directories. Evidence does not need a source checkout in the guest.

Other implemented `--scenario` values are `window`, `desktop`, `clipboard-text`,
`clipboard-image`, and `clipboard-files`. Selector checks are `region-cancel-escape`,
`region-cancel-right-click`, and `region-cancel-zero-area`: they require cancelled/no-result
completion, no artifacts, and an unchanged independent text clipboard source.
Use fresh overlays for scenario isolation.
QMP drives the real hotkey while the independent fixture is focused. The fixture
paints fixed RGB regions, owns real native clipboard sources (Ctrl+Alt+Shift+T/I/F), and
records actual Ctrl+V MIME results. Region dimensions/interior pixels, full frame/desktop
geometry and known interior pixels, detached text, image and ordered file bytes are
compared independently. Wrong target, crop, bytes or paste fails and preserves a witness.
Host guest clipboard bridging is never enabled.

For publication, provision a keychain credential reference in the guest separately.
Supply a non-secret settings patch using `--settings work/lab-bootstrap/destination.json`.
It defines the isolated S3 test profile and action profile IDs. Only `keychain` reference
credentials are accepted by this lab route. The runner appends a unique run prefix to
that test destination, saves local copies, retrieves public URLs without authentication,
and compares actual public bytes with retained artifact SHA256 and newline URL paste.
It never changes bucket policy, retention or versioning. Do not use an ordinary bucket
profile or existing user objects as test fixtures. Credentials and payloads are absent
from golden disks and review packets; intentional synthetic evidence contains only the
lab's synthetic captures/text/files.

`--history` with `--settings` restarts the installed resident, inspects the new history
IDs, copies the stored link and pastes it into the independent fixture. Results retain
exact `delete.prepare` IDs, effects and acknowledgement. This preparation is read-only.
Use the production CLI interface to inspect before explicitly acknowledging deletion:

```sh
python3 tools/lab/lab.py cli --run work/lab-runs/cinnamon-001 --method capabilities
python3 tools/lab/lab.py cli --run work/lab-runs/cinnamon-001 --method history.detail \
  --params '{"id":"EXACT_NEW_SYNTHETIC_HISTORY_ID"}'
```

For deletion, review `delete.prepare` from the production CLI, then call `delete.confirm`
with its identical ordered IDs and acknowledgement. Retain returned operation ID and
poll `delete.inspect` for actual outcome; preserve the local archive and history record.
GUI deletion/search/details acceptance remains a separate graphical gate.

For manual scenarios use `key --run RUN ctrl shift 1`, `input --run RUN 'JSON_EVENTS'`,
`screenshot --run RUN --head 0`, `collect --run RUN`, and `reset --run RUN`.
Input uses QMP `input-send-event` objects. Query output capabilities are stored in
`qmp-features.json`; witnesses identify device `video0` and the requested head. `collect`
exports only whitelisted package/kernel versions and production sanitized diagnostics.
Private app/QEMU logs stay private; review them before including any diagnostic bundle.
