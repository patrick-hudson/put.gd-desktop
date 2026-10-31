# Bun 1.4.4 native modification/relink replay

Run on d69 or another authorized builder, using a new private directory. This
adapts the published 1.4.2 supplement to the current source packet. The original
sealed packets and historical receipts remain unchanged. No account, keychain,
Proton session, user payload, host directory mount or Docker socket enters the
build container. Marked executables are proof artifacts only.

Download and verify the transport from
[the Bun 1.4.4 source release](https://github.com/patrick-hudson/put.gd-desktop/releases/tag/proton-sources-0.23.0-crypto2.2.0-bun1.4.4).
Join the two parts in numeric order and verify the combined archive and all six
packets against `../notices/SOURCES.json`. Obtain the official bootstrap Bun
executable identified in `../runtime-lock.json`.

```sh
python3 tools/proton/native_relink/prepare.py \
  --assets /absolute/path/to/verified/packets \
  --bootstrap /absolute/path/to/official/bun \
  --output /absolute/new/private/replay
```

`prepare.py` verifies all three selected packet hashes and the bootstrap hash
before creating its output. The stage contains the current Bun/helper source
packets, the unchanged historical supplement's portable fixtures, and these
current build/marker/proof scripts. The image installs version-pinned LLVM
23.1.5 distro packages and four checksum-pinned Rust nightly 2026-09-15
components inside the disposable environment. No host compiler is installed.
LLVM 23.1.5 satisfies upstream's 23.1.x toolchain range. Supporting distro
packages are recorded; this is not a hermetic distro snapshot.
Upstream probes an optional `bun-ninja` download. In the network-disabled
container that probe fails and the build uses distro Ninja; preserve that log
instead of treating the probe as a successful download or enabling the network.

Before creating the container, check the resolved Docker endpoint's Unix socket
length. The default endpoint resolves to `/run/docker.sock` (16 UTF-8 bytes);
Linux's limit is 107 bytes. This proof creates no put.gd session socket.

```sh
cd /absolute/new/private/replay
docker build --tag putgd-bun144-proof:replay image > image-build.log 2>&1
docker create --name putgd-bun144-proof --user ubuntu --init --network none \
  --cpus 2 --memory 12g --memory-swap 12g --pids-limit 2048 \
  --cap-drop ALL --security-opt no-new-privileges putgd-bun144-proof:replay
tar --owner=1000 --group=1000 --numeric-owner -cf - -C stage . |
  docker cp -a - putgd-bun144-proof:/p3
docker start putgd-bun144-proof
docker exec putgd-bun144-proof nice -n 10 python3 /p3/run_build.py > native-driver.log 2>&1
docker exec putgd-bun144-proof nice -n 10 python3 /p3/run_helper_proof.py > helper-proof.log 2>&1
```

The named `ubuntu` user has UID/GID 1000. On d69, `docker cp -a` rejects a
container configured with the numeric `1000:1000` user before staging; the named
user avoids that Docker lookup failure without changing build privilege.

The recorded run began with two C/C++ jobs. Once d69's other heavy jobs finished,
both Ninja processes were intentionally interrupted, their logs retained, and
the same prepared tree resumed with `resume_build.py` and four CPUs/C/C++ jobs.
Rust retained two jobs and the container retained its 12 GiB/no-swap limit.
The resume script never assembles source or reapplies the marker patch. A fresh
recipient replay can simply leave the initial two-job build running to completion.

The preflight checks direct compiler identities and the preferred standard
library's `Cargo.lock`. Cargo must report its exact submodule commit
`7941be6fb416b4cd9666aef7b858dfea25587a8c`, rather than the Rust repository commit.
`rust-cargo-submodule.json` retains the upstream identity response.

The offline build restores the pinned sources and native patches, verifies and
merges Rust's vendored standard-library inputs, and builds local JavaScriptCore
with system ICU. The proof patch modifies Bun's Rust revision handler,
JavaScriptCore C++ and TinyCC C. The revision handler calls both linked native
marker functions. The proof then requires the markers and actual TinyCC
compilation/execution from both the source-built Bun and the helper compiled
with its explicit `--compile-executable-path`. It also runs the current helper
tests, compiled no-account protocol checks, complete stdout-drain regression,
and the historical portable Argon/OpenPGP interoperability fixture.

Preserve `/p3/proof`, `/p3/logs`, `/p3/phase.json`, actual compiler and distro
package identities, image/container inspection, failed attempts and commands
before removing the disposable container. Hash the exported evidence and retain
the proof executables privately on the builder. Source modification/relinking
does not establish native byte identity, live Proton behavior, resources,
desktop acceptance, or stable-release acceptance.


The 2026-10-31 d69 run passed for production helper
`9fd4d72f86352cfcf2c8b6ba1db4102ed6f7b22660077698c700989e9fda8586`.
See [the execution receipt](evidence/native-proof.json),
[its prerequisite installer audit](evidence/ci-before-native.json) and
[the replay input comparison](evidence/recipe-validation.json).
The native executable reports `--version` 1.4.4 and upstream release-local
`--revision` metadata `1.4.4-canary.1+38afee6c8`, alongside the three executed
source markers. Its exact source commit is the pinned Bun 1.4.4 commit; this
proof does not substitute that marked native executable for the production Bun.


Helper proof commands default to a 300-second deadline each, configurable with
`PUTGD_PROTON_PROOF_TIMEOUT_SECONDS`. A timeout fails with the command stage and
writes `proof/command-timeout.json`; it cannot produce a successful receipt.
Each command owns a separate process group, which is killed on timeout so the
smoke driver's compiled helper exits too. The binary stdout fixture uses the
same configured deadline and keeps stderr separate for its empty-stderr check.
Its stdout remains pipe-backed and is drained with `communicate`, preserving
the large-output backpressure regression before its bytes are written to a log.
The long native source build uses its separate build driver.

The first [bounded-command replay](evidence/bounded-replay.json) records the
initial deadline wrapper against the retained source-built runtime. It passes the same
suite and reproduces all three recorded primary proof executable hashes; the original complete
native build and its receipts remain unchanged.
The [process-group replay](evidence/bounded-process-group-replay.json) records
the intermediate runner and its configured deadline. Its file-backed stdout
diagnostic is superseded by the [pipe-backed replay](evidence/pipe-backed-replay.json),
which verifies the final runner and the complete large-output pipe regression.
