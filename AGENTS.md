# Working on put.gd

Build with C++20, Qt 6 Widgets, CMake presets, Ninja, and the official AWS C++ SDK.
The minimum Qt API is 6.4.2. Keep Qt dynamically linked to distro libraries.
Use standard system development packages; ask the user to install missing packages
when administrator authentication is required. Do not assemble a substitute toolchain.

Prefer Patrick's Fedora 44 (`192.168.68.70`), Debian 13 (`192.168.68.69`) and
Kubuntu 26.04 (`192.168.68.72`) physical build/test servers for builds, VM hosting
and graphical acceptance.
Their existing GNOME and Plasma sessions are graphical acceptance targets too:
run and verify the app directly on the appropriate physical desktop, visible on
its host wall tile, with saved visuals and exact host/session/source/build identities.
Use local workstation VMs as a fallback when those hosts cannot run the required
profile; record the reason. Connection pins, host selection, visual evidence and
golden-image transfer rules are in `docs/remote-builders.md`. On these three servers,
the user authorizes dependency/tool setup and testing with passwordless
`sudo -n`; preserve existing projects and desktop work. Keep the SSH private key
on the workstation. Verify host capacity, KVM and display access before VM runs;
host preference does not change the required guest versions.
Linux goldens and Windows bases are not kept on the workstation. They live on the
Fedora and Debian builders and in a private S3 bucket. Find, copy and restore
them using `docs/remote-golden-images.md`, and expose
each remote VM on the wall using `docs/remote-vm-displays.md`. Provision new remote
lab VM goldens with guest login `lab` / `putgd-lab`, verifying login visibly.
The user explicitly approved documenting this default disposable-guest login
as the exception to the no-credentials-in-source rule. Preserve immutable sources.

Required Linux testing covers Mint, Ubuntu, Kubuntu, Fedora and Debian, with
Cinnamon plus GNOME/KDE and real X11/Wayland coverage as mapped in
`docs/acceptance-matrix.md`. Prefer the build servers, using their native desktops
and new VMs as needed; fall back locally with a recorded reason. Run any release
gate requiring physical/native hardware on the appropriate physical machine.
Provision missing profiles; never substitute VM or XWayland evidence for such gates.

The implementation contract is in GitHub issue #1 and
`docs/plans/2026-10-07-1926-feat-qt-linux-share-plan.md` when present locally.
Preserve its user-approved choices and explicit non-goals. Implement in dependency order.

Use `cmake --preset dev`, `cmake --build --preset dev`, and `ctest --preset dev`.
See `docs/build.md` for SDK provisioning and sanitizer builds. Keep AWS headers under
`src/storage/s3/`; shared core, GUI, history, and platform contracts remain provider-neutral.
Offscreen tests prove widget/core behavior only. Real desktop support requires graphical
acceptance evidence for each declared compositor. Never report a mock as desktop proof.

Run a new candidate's Cinnamon, GNOME and KDE checks in parallel on the build
machines, each with its own functional and memory gates. Every declared desktop
still requires current-version evidence on that exact candidate before acceptance.
A failure on any desktop blocks acceptance until it is fixed or shown to predate
the candidate; retest a fix on every desktop.

Use synthetic payloads and an isolated test destination. Never commit `credentials.txt`, including with force-add. It is user-provided local
test input only; never include its contents in logs, diagnostics, or review packets.
Never put credentials in source,
configuration, logs, history, command lines, or review packets. Never modify bucket policy,
versioning, or existing user objects during tests. Configuration contains credential references.

The user grants standing permission to send implementation and planning material to
Anthropic for review. This permission does not select an external implementation engine.
Keep review material bounded to the code/specification needed, excluding secrets and payloads.

Branch from `nightly` and open pull requests against `nightly`, the default branch
and integration trunk. Never push to `nightly` or `main` directly; rulesets require
a pull request and the `CI` check. CI selects jobs from the changed paths, and the
`CI` summary is the only required check; a cancelled or skipped-by-failure job is
not a pass. Leave `project(putgd VERSION ...)` alone in feature pull requests.
Promote to `main` only through `tools/releases/promote.py --open` (a `release: vX.Y.Z`
pull request from `nightly`, merged with a merge commit); its merge cuts the next
candidate draft. Stable and candidate publication stay manual after exact-asset
acceptance. Never create, move or delete `v*` tags by hand. Hotfixes target `main`
with a patch bump and are merged back into `nightly` by pull request. The release
runbook is `docs/release.md`; CI coverage is `docs/ci.md`.

Record what was actually built/tested and any blocked acceptance gates. Commit explicit
owned paths, preserving unrelated pre-existing changes. Do not add recording or OCR
features: future video support currently means finalized video files in the shared pipeline.
The user-approved Edit & upload image editor (#51) is the only editing feature; its
contract, privacy boundaries and agent control are in `docs/image-editor.md`.
New windows reuse the redesign theme and IconArt glyphs, and every interactive
capability stays controllable through session methods, `putgd-cli` and the MCP helper.

Agent source, workflow and acceptance maps are in `docs/agents.md`. The optional Python
3.12+ official-SDK stdio helper is under `tools/mcp/`; follow `docs/mcp.md` for locked
setup and its separate integration suite. It forwards only to the production CLI.
Inspect actual capabilities/configuration before acting; retain operation IDs and prepare
exact history IDs/effects before acknowledging deletion. U6 lab and package interfaces
are implemented; use `docs/vm-lab.md` and `docs/build.md` for their actual commands.
Full U9 acceptance still requires current-version real Cinnamon evidence and the
complete fresh-agent walkthrough gates recorded in `docs/verification.md`;
headless parity or an earlier partial walkthrough alone cannot establish those gates.

Keep graphical lab interactions visible through each VM or physical desktop's
localhost VNC listener and the workstation's SSH tunnels.
The user watches the live display wall at http://127.0.0.1:8765/; do not disable or
redirect guest display output during graphical tests. Distinguish SSH build/log work
from graphical acceptance, and restore an idle/locked test session through its
visible console before using it as acceptance evidence.
Capture visual results for every graphical acceptance run, including failures,
and retain screenshots or recordings with the tested host/session, source/build
identity and functional artifacts. A live tile or SSH log alone is not a saved
visual result. Never capture a revealed credential or protected login value.

Before launching any isolated Linux test, calculate the UTF-8 byte length of
every resolved Unix socket path, including the harness-added runtime directory
and `putgd/session.sock` suffix. The Linux limit is 107 bytes excluding the
terminating NUL. Use a short, new, private output directory and reject an
overlong path before starting the app. Do not change a frozen performance
harness or weaken socket ownership checks to accommodate a long artifact name.
Preserve failed attempts and record the corrected command before retrying.
Use `python3 tools/diagnostics/check_socket_paths.py --ui-output RUN_1 RUN_2`
for the UI harness, or `--socket` for complete paths in other tests.

Agents own lab-session unlocking. Inspect the existing run/bootstrap metadata
for its protected local login input before asking the user for credentials.
Verify that input is an owned, private regular file, then read it only in memory
and type into the visibly focused masked login field through the VM console.
Never include its value in commands, tool output, logs, screenshots, configuration
or review material; never use `credentials.txt` as a desktop login source.
Verify the unlocked desktop visibly before acceptance, and ask the user only
if the protected login input is actually unavailable or rejected.

Windows VM bases under `work/windows/bases/` are sealed: never boot, modify,
move, overwrite, or delete them while test overlays reference them. Run every
Windows test in an independent copy under `work/windows/runs/`, created with
`tools/lab/windows.py clone`; preserve the base disk, UEFI variables, TPM state,
identity and manifest together. Provision a new base directory for updates.
See `docs/windows-vm-lab.md` for lifecycle commands and verification evidence.

Apply the same immutable-base rule to provisioned Linux golden disks under
`work/lab-images/`: never boot or modify them directly, and never move, overwrite
or delete a disk referenced by overlays. Linux tests use fresh disks under
`work/lab-runs/` through `tools/lab/lab.py`; provision a new golden disk for updates.
See `docs/vm-lab.md`. Goldens and Windows bases each have Fedora, Debian and S3
copies. Never delete any copy without Patrick's approval and two other copies
verified against the same SHA-256.

Retire completed disposable VM overlays after their evidence is exported and
verified. Normal successful `tools/lab/lab.py run` calls do this automatically;
`--keep`, failed runs and incomplete collections retain their disks for
investigation. After custom/manual campaigns finish, use the guarded `cleanup`
command in `docs/vm-lab.md` with a completion evidence manifest. Never remove a
running overlay, a golden or a disk still referenced by another image. Preserve
all host-side evidence, run identities and retirement receipts.

Evidence retention is documented in `docs/evidence-retention.md`. Preserve merged
acceptance summaries, exact identities, reproducible parameters, failure context
and cleanup receipts. Compress cold bulk evidence only with verified checksums
and a restore index; never delete test source as a substitute for archiving its
results. Local work archives remain private and outside release source exports.
