# Boat snapshot recipes and package targets

New development lanes use these snapshots. The adjacent repository manifests in
`tools/lab/boat/manifests/` are authoritative for the IDs, image hashes, package
versions and build identities.

| Snapshot | Host package target | App/package target | Guest |
| --- | --- | --- | --- |
| `putgd-build-base-v3` | `ubuntu24.04` | `ubuntu24.04` DEB | Build worker |
| `putgd-lab-cinnamon-v3` | `ubuntu24.04` | `ubuntu24.04` DEB | Mint 22.3 / Cinnamon X11 |
| `putgd-lab-ubuntu-gnome-v4` | `ubuntu24.04` | `ubuntu26.04` DEB | Ubuntu 26.04 / Qt 6.10 / GNOME Wayland |

The original `putgd-build-base`, `putgd-lab-cinnamon`,
`putgd-lab-ubuntu-gnome`, `putgd-lab-ubuntu-gnome-v2` and `putgd-lab-aurora`
remain intact. The intermediate `putgd-build-base-v2`,
`putgd-lab-cinnamon-v2` and `putgd-lab-ubuntu-gnome-v3` also remain intact;
the three names above supersede their build/Cinnamon/GNOME roles. Aurora v2
was deferred in [#769](https://github.com/patrick-hudson/put.gd-desktop/issues/769)
until its Toolbx tools can be proved visible to the host probes without an
rpm-ostree change. These are Boat development caches, not physical-desktop,
performance, memory or release acceptance.

## Build the correct guest package

The GNOME worker is Ubuntu 24.04, but its guest is Ubuntu 26.04. A host-built
Noble DEB cannot be installed in that guest. Use the candidate builder's native
package target, which selects the target distro's digest-pinned container from
that exact commit's `.github/workflows/package.yml`:

```sh
python3 tools/lab/boat/candidate.py FULL_NIGHTLY_COMMIT \
  --artifact ubuntu26.04 --output NEW_PRIVATE_OUTPUT
# Cinnamon/build-base use --artifact ubuntu24.04 instead.
```

This is route (b) for [#770](https://github.com/patrick-hudson/put.gd-desktop/issues/770).
The manifests record `packageTarget`, `hostPackageTarget`, `packageBuild.method`,
container digest, package SHA-256 and clean build identity. The saved audited
DEB is under `/home/user/snapshot-packages/TARGET/`, alongside
`native-audit.json`. Never substitute libraries or install desktop packages in
a guest to make a mismatched package install. Compare the installed
`putgd-cli --version-json` with the artifact's build-info after starting it.

## Prepare a new worker and built tree

The repeatable implementation is `tools/lab/boat/snapshots.py`. Its lightweight
contracts run with `python3 tests/desktop/test_boat_snapshots.py` and
`python3 tests/desktop/test_boat_candidate.py`. All builds and VMs run on Boat;
only control, editing and these unit tests run on the workstation.

Agree the start budget and keep at most three workers active. Create a preparation
worker with `boat new --from OLD_NAME --type large --ttl 14400 --no-env`,
reuse it for every retry, and wait for `boat info` to report `hydrated: true`.
Record bare-metal placement, capacity, identity and TTL. Pin the worker's
Ed25519 host key obtained through `boat exec`, using strict OpenSSH; do not
use an unauthenticated network scan. Keep private keys on the workstation.
Copy this recipe to `/home/user/snapshot_recipe.py`, fetch the selected nightly,
and detach `/home/user/work/put.gd/src` at its full commit. On the worker:

```sh
cd /home/user/work/put.gd/src
python3 /home/user/snapshot_recipe.py prepare-scratch
python3 /home/user/snapshot_recipe.py host-script --checkout "$PWD" \
  --commit "$SHA" --jobs 4 --prepare > /home/user/prepare-snapshot.sh
bash /home/user/prepare-snapshot.sh > /home/user/prepare-snapshot.log 2>&1
```

Preparation reads the exact checkout's CI package list, including
`libxcb-xinput-dev`. It explicitly chooses Ubuntu's `nodejs/noble` and
`npm/noble` to resolve the old Boat NodeSource/npm conflict. It runs CI's SDK
provisioner with verified warm-source reuse, then
`tools/proton/provision_runtime.py` before configuring the bundled helper.
Qt remains dynamically linked to distro libraries. It changes no compositor
or portal package.

## Fast scratch and a plain full CTest run

`prepare-scratch` persists `PUTGD_CI_TMPDIR=/dev/shm` and `TMPDIR=/dev/shm` in
`/etc/environment`, preserving other settings. `/dev/shm` is the existing
bounded tmpfs; no mount over `/tmp` or manual setup on fresh forks is needed.
The generated runner verifies the filesystem identity and creates a new private
short directory there. CI preflight checks all socket paths and records the
registered tests and test-user context. It runs as the unprivileged `putgd-ci`
account, with private XDG homes and no desktop display variables:

```sh
# Emitted by host-script after configure/build/preflight and private environment setup:
ctest --preset dev --parallel 12 \
  --resource-spec-file "$PWD/tools/ci/ctest-resources.json" \
  --output-junit "$PWD/build/dev/snapshot-tests.xml"
```

Configure uses `PUTGD_TEST_TIMEOUT_SCALE=3`; execution uses
`PUTGD_TEST_TIME_SCALE=3`. This is one plain full suite: no retry wrapper or
repeat option. The runner writes a ledger with `retryEnabled: false` and refuses
an incomplete, duplicated, failed or skipped test set. Retain disk-backed
failures from [#792](https://github.com/patrick-hudson/put.gd-desktop/issues/792);
this workaround makes no claim about the underlying filesystem cause or a
performance acceptance gate. Preserve failed evidence before any further run.

## Provision a new immutable golden

Every standalone image must be in
`/home/user/work/put.gd/work/lab-images/NAME/image.qcow2`.
The source remains immutable and is never booted directly. Inherited verified
copies need no transfer; missing copies come from S3 via the short presigned
URL and mode-0600 curl configuration described in
[the golden inventory](remote-golden-images.md). Never copy S3 credentials to
Boat or include URLs in commands, logs or snapshots.

Forward an isolated workstation agent carrying only the lab bootstrap key.
Use the inherited public key and the same guest's console-verified host pin in
`/home/user/snapshot-bootstrap/`. GNOME and Cinnamon have different host keys.
Fresh verification must reuse those files byte-for-byte. Verify capacity, KVM
access and display ports; keep loopback VNC 5920 visible on a dedicated live-wall
tile and save screenshots. The recipe checks complete QMP/VNC socket paths.

For a new Cinnamon version, run this on Boat with distinct unused names:

```sh
python3 /home/user/snapshot_recipe.py guest \
  --checkout /home/user/work/put.gd/src --commit "$SHA" \
  --controller-checkout /home/user/lab-driver --controller-commit "$CONTROLLER_SHA" \
  --profile cinnamon-x11 --run-name pg1 \
  --source "$CANONICAL_SOURCE_IMAGE" --source-sha256 "$SOURCE_SHA256" \
  --bootstrap-key /home/user/snapshot-bootstrap/identity.pub \
  --known-hosts /home/user/snapshot-bootstrap/known_hosts \
  --evidence /home/user/NEW_EVIDENCE \
  --deb "$AUDITED_UBUNTU2404_DEB" --package-commit "$SHA" --package-target ubuntu24.04 \
  --test-held-overlay --compress-golden --install --name NEW_SNAPSHOT_NAME \
  --destination /home/user/work/put.gd/work/lab-images/NEW_GOLDEN/image.qcow2
```

For GNOME select `gnome-wayland`, `ubuntu26.04` and its audited DEB. The separate
controller checkout allows a documented lab lifecycle fix to be tested while
preserving the exact built/package source; record both commits. The final lab
snapshots use #787's merged controller commit, recorded in their manifests.

The probe contract is the exact `origin/feat/surface-driver-core`
`provision-probes.sh` revision recorded by the recipe, plus the requested
`tesseract-ocr`. It covers xdotool, xclip, wl-paste/wl-copy, Python AT-SPI,
D-Bus, PyQt6 and dbus-monitor. This adds probe dependencies, not an OCR feature.
GNOME's lab policy disables screensaver idle activation and locking, sets
`org.gnome.desktop.session idle-delay` to zero, and disables the power idle
suspend/dim actions. Launch applies and reads back the production preflight
policy; the recipe also saves a read-only check after provisioning.

`guest` launches a fresh overlay, installs/checks probes, installs/starts the
matching package, compares its running version with the build, saves visuals,
and gracefully quits. It removes the disposable authorized key, syncs, resets,
and flattens the stopped overlay to a new standalone qcow2. Compression has a
hard output limit preserving 10 GiB host reserve; partial failures are retained
and never overwritten. It checks the source hash again and writes an adjacent
manifest with source/new hashes and package target. No existing golden is changed.

For an inherited original GNOME snapshot with an unused outside-root copy,
quiesce all owned lab lifecycle work first. The [recorded relocation script](evidence/2026-10-30-boat-snapshots/context/canonicalize-unused-copy.py)
checks the source hash, every discovered backing reference and a privileged
read-only open-file/mapping inventory under the lifecycle lock before renaming
the unreferenced directory into the canonical root. It verifies the same hash
afterwards. Never relocate a referenced base; never change an original saved
snapshot. Preserve the relocation receipt and original inventory.

For Cinnamon the new golden contains the supported XDG user override
`~/.config/autostart/mintupdate.desktop` with `Hidden=true` and
`X-GNOME-Autostart-enabled=false`. Provisioning changes only the fresh overlay's
lab user. It may stop an already-running Update Manager during preparation by
its exact UID/PID/start identity; fresh-fork checks only read the override and
require no Update Manager process, so lanes need no per-run process kill.
The override is recorded as `guestSessionPolicy.mintUpdate` in the manifest.
This addresses the foreground-guard interference seen by #785.

The held-overlay check keeps the exact disk inode open, proves guarded cleanup
refuses and leaves it present, releases only that owned holder, then proves
dry-run and actual cleanup succeed with the canonical `--images-root` and a
completion evidence manifest. Keep reset and retirement receipts.

## Publish and verify a fresh fork

Generate the worker manifest with `manifest --checkout ... --name ...
--source ... --source-sha256 ... --golden ... --output NEW_FILE --deb ...
--package-target ... --controller-checkout ... --controller-commit ...`.
It records the CI package inventory, clean built tree, warm Proton hashes,
bootstrap pins, target package, scratch settings, golden paths/hashes and full
CTest result. Build-base explicitly records its inherited golden as unchanged;
it makes no guest-probe claim. Export the manifest and evidence, then publish
from the workstation:

```sh
python3 tools/lab/boat/snapshots.py publish --worker WORKER \
  --name NEW_NAME --manifest /PRIVATE/manifest.json --output /PRIVATE/publication.json
```

Keep the worker unchanged from manifest generation until ready readback: do
not update its checkout/controller or start another preparation during the save.
Publication rejects every existing name. CLI event streams and lost replies
are resolved through read-only binding readback, never by saving again. If
interrupted, use `receipt` with the same arguments. A snapshot cannot contain
its own later publication ID; the repository manifest and publication receipt
bind that ID to its embedded preparation manifest.

Create a fresh fork from each saved name, wait for hydration and pin its worker
key. Run its inherited `host-script` **without `--prepare`**: configure, build and
one full CTest run without apt installs or Proton/SDK provisioning. Build-base
requires all 114 tests in that single pass, with retries off. Retain any lab
first-pass failure separately from a focused retest and its known-issue record.
No scratch command is run in verification. For lab snapshots run `guest`
without `--install`, `--name` or `--destination`, using the new golden/hash,
inherited bootstrap inputs, matching audited DEB and `--test-held-overlay`.
This checks probes read-only, installs/starts the package and proves guarded
retirement on the fresh fork. The campaign used the tested repository recipe
in a separate `/home/user/fresh_recipe.py` for these guest checks; its hash is
in each receipt. Copy that recipe from the code PR/current checkout, preserving
the inherited recipe and bootstrap files. The host configure/build/test proof
still uses the inherited host runner. For build-base run `host-package` with the audited
Ubuntu 24.04 DEB on the worker's visible desktop as well.

Delete each verification fork as soon as its evidence is exported and checked.
Delete all preparation workers, stop only your tunnels and agent, remove only
your wall tiles, and check every owned ID against `boat list --all`. Preserve
unrelated workers and every named snapshot. Evidence, including failed attempts,
JUnit, identities, screenshots and retirement receipts is linked from the
[snapshot campaign record](evidence/2026-10-30-boat-snapshots/README.md).
