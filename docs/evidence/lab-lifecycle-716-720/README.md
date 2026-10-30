# Lab lifecycle fixes #716 and #720

Code PR: [#727](https://github.com/patrick-hudson/put.gd-desktop/pull/727).
Documentation PR: [#729](https://github.com/patrick-hudson/put.gd-desktop/pull/729), to merge after the code.
This is package-install and disposable-overlay lifecycle evidence, not product desktop or release acceptance.

## Follow-up after independent validation

The campaign below is historical evidence at `0227cfa2`, not the final source
of #727. It did not prove install compatibility with an unchanged existing
golden: its overlays had been manually bootstrapped with the new helper.
Independent validation found that current goldens lack that helper. The final
controller instead stages and hash-verifies it through the run's SSH connection
and existing passwordless apt rule, preserving keys and immutable goldens.
Staging disables repository sources and list cleanup; normal apt work remains
under the supervisor. It also adds held-job diagnostics, early missing-supervisor
failure, both socket-path checks, symlink-safe root output, and legacy-run
retirement guidance. The final-source follow-up acceptance and private evidence
archive identity are recorded in the [code PR's acceptance comment](https://github.com/patrick-hudson/put.gd-desktop/pull/727).
This does not upgrade the earlier runs into final-source evidence.

## Original campaign

The original controller/supervisor source is recorded in [source-identity.json](source-identity.json).
The input is the named snapshot's existing 116 MiB Debug DEB,
`0.1.1~dev.907+g0071acafc975`, SHA-256
`f5bd05537d8e2931f464fe0a54a52dd638f6fd3b32b2134dfef5785812d1c5ec`.
It is not relabelled as a current product build. Installed binary hashes are retained in each package receipt.

## Unit and existing contracts

On Debian 13 `192.168.68.69`, a private source export ran
`python3 tools/ci/contracts.py lab --output /tmp/lab716-contracts-review`:
26 suites, 432 tests, no failures. The [structured report](lab-contracts.json)
retains every command and exit code. No build or VM ran on the workstation.

The real-process supervisor tests prove that a timeout waits for an adopted
child after its original parent exits, checks `dpkg -s putgd` and `dpkg --audit`,
and retains partial output. The abort test kills its exact descendants while
an unrelated process stays alive. Other tests cover incomplete package state,
unavailable dpkg queries with durable recovery receipts, safe lock release,
no duplicate launch after a lost SSH reply, RPM rejection before guest work,
noncanonical paths creating nothing, canonical paths reaching launch, and
symlink rejection. `tools/lab/overlay_cleanup.py` was unchanged from the base in that original campaign.

## Boat runs

[Host identity](boat-identity.json): named `putgd-lab-cinnamon` snapshot fork
`bx_76ecpr6w`, `--no-env --ttl 7200`, hydrated before QEMU use,
`machineProvider: baremetal`, `/dev/kvm` present. Each run used a new overlay
under `/home/user/t716/work/lab-runs`, 2 guest vCPUs / 6144 MiB, Cinnamon/X11.
The sandbox host key was read over authenticated `boat exec` and pinned for SSH.
Bootstrap private key stayed on the workstation; its isolated agent was forwarded.
The loopback VNC listener remained visible through workstation wall port 6096.

These are genuine first installs from the immutable golden, with no `putgd`
package installed beforehand. The disposable overlays deliberately cap apt HTTP
and HTTPS downloads at 128 KiB/s to reproducibly exceed the old 180-second
budget; no timeout is fabricated and no sleep hook replaces apt/dpkg work.
The package resolves its real Qt 6.4.2/GStreamer dependencies from distro repositories.

- `c1`, controller `1d975c86a952e7700b1d5b21ab8232d8c5400f24`, default
  900-second budget / 600-second recovery: **403.49 seconds**, apt exits `[0,0]`,
  `install ok installed`, clean audit, no remaining owned PIDs, installed bytes
  verified. [Curated receipt](run-c1.json).

- `c2`, controller `4e8cc39cd06d3f4cd5233138d640d93ea0922e5d`, CLI
  `--install-timeout 180 --install-recovery-timeout 900`: **371.16 seconds**.
  The receipt records `timedOut: true`, apt exits `[0,0]`, no abort and no
  remaining owned PIDs. The controller waited for the original install and
  verified package/binary state without starting a replacement. [Curated receipt](run-c2.json).

- `c3`, original campaign controller/supervisor commit
  `0227cfa28565086dd79d2ac8f93407d0429f3513`, CLI
  `--install-timeout 180 --install-recovery-timeout 900`: **406.83 seconds**.
  Recovered after the 180-second budget, apt exits `[0,0]`, clean dpkg state,
  no remaining owned PIDs, requested binary hashes verified, job lock released.
  The package was absent before this fresh install. [Curated receipt](run-c3.json),
  [storage reserves](storage-c3.json), [installed-state visual](installed-c3.png).
  A separate saved QMP witness and all startup visual attempts remain private.

[Real invalid-path proof](noncanonical.json): `launch --run /tmp/x` failed in
61 ms, did not create `/tmp/x`, and left the canonical directory listing unchanged.
Canonical `c3` launch succeeded on that exact source revision. [Resolved socket paths](socket-paths.json) are
41 and 33 UTF-8 bytes, below 107. Host/guest storage reserves passed; the raw receipts remain in the private archive.

## Failure context and retention

The first Boat create was rate-limited and allocated nothing; a later retry
created the single sandbox above. A large source-archive transfer was interrupted
by its exact owned scp PID and replaced with a small driver archive. An initial
socket-check invocation repeated `--socket` and only retained its last argument;
the corrected command passes all paths after one `--socket` before subsequent
launches. The first console typing attempt preceded terminal readiness and produced
`o: command not found`; it was corrected at the visible prompt. Password input was
read only in workstation memory from the existing protected local input and typed
only into the verified masked prompt; no credential value was saved or uploaded.
The first manual installed-state console query had a pipe-key translation error;
the corrected plain `dpkg -l putgd` query and both visual attempts are retained.
Initial blank desktop frames and the successful later frame are retained in the
launch preflight artifacts. No failure is presented as graphical product evidence.

Bulk apt logs, original manifests, all saved QMP visual attempts, and exact
source/run metadata remain in the private evidence archive outside release source
exports. [Retention receipt](retention.json) records SHA-256
`f969698ffbecab62e175b81a968aaf8d65cb68fba845df3848516107a4b1c3c5`;
a full restore verified all 126 indexed files before sandbox deletion. Workstation
RFB screenshots (including failures), test logs and the visual index are retained
beside that archive in the ignored private work directory.
The curated summaries here retain dpkg outcomes, process identities, checksums,
and retirement receipt identities. Raw run files and custom completion manifests
are private and are not included in release source exports. Named snapshots and immutable golden disks are preserved.

All three overlays were retired through guarded cleanup. The sole created sandbox
was deleted with a completed Boat deletion receipt; its wall tile, tunnel and
isolated agent were removed. [Lifecycle cleanup summary](lifecycle-cleanup.json).
No named snapshot was changed or deleted.
