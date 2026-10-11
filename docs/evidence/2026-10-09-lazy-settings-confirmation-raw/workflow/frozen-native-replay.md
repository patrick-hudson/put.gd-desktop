# Clean experimental checkpoint replay

`frozen-native-replay.py` is preparation for the exact four-path lazy Settings
checkpoint after the current `lazy-exp001-native` cohort passes its native gates.
It does not commit anything. Help and syntax checks perform no guest interaction.
Its explicit `prepare` phase freezes the already-clean checkpoint and performs
read-only current-resident inventory; all mutating phases need deliberate execution.

The root owns the checkpoint commit. Its parent must be the recorded baseline45,
its diff must contain exactly `SettingsWindow.cpp/.h`, `test_settings.cpp`, and
`test_ui_application.cpp`, and every source byte/mode must equal the reviewed dirty
cohort. A mismatch aborts. This commit is an experimental identity, not a keep decision
or a main-branch integration. No prior evidence is relabelled.

Use a NEW output directory beneath `work/ui-refinement/native-replay`. After closing
each current helper normally, closing the synthetic fixture visibly, and verifying
zero active operations, the root can run:

```bash
checkpoint_commit=FULL_CLEAN_CHECKPOINT_COMMIT
replay_output=/home/patrick/work/put.gd/work/ui-refinement/native-replay/frozen-exp001-source46
frozen_replay=/home/patrick/work/put.gd/work/view-startup-latency/frozen-native-replay.py
native_replay=/home/patrick/work/put.gd/work/ui-refinement/native-replay/replay.py
python3 "$frozen_replay" --output "$replay_output" prepare --commit "$checkpoint_commit"
python3 "$native_replay" --output "$replay_output" stop --guest cinnamon
python3 "$native_replay" --output "$replay_output" stop --guest gnome
python3 "$native_replay" --output "$replay_output" stop --guest kde
```

The existing runner verifies current PID/start ticks/live binary hash/build manifest,
requires helper exit and zero operations, checks the complete synthetic journal's
terminal states, backs up the entire old profile and live markers, and stops only
that resident. It does not stop/reboot any VM or touch a base disk. All three stop
receipts are required before any build or install. The display wall/VNC stay active.

These three independent builds may run concurrently through separate yielding
command sessions:

```bash
python3 "$frozen_replay" --output "$replay_output" host-build
python3 "$frozen_replay" --output "$replay_output" guest-build --guest gnome
python3 "$frozen_replay" --output "$replay_output" guest-build --guest kde
```

Host uses the existing `exp001` checkout/build cache and system SDK. KDE/GNOME use
their existing `/home/lab/putgd-ui-refinement-lazy-exp001-native/source` Git/build
caches and `/home/lab/putgd-memory-build` SDK. Each remote update verifies all old
reviewed source hashes/modes, clean index, exact four dirty paths and exact patch
bytes. It checks/reverses that patch, requires clean baseline45, fetches the new Git
bundle, checks out the actual checkpoint, verifies clean Git commit/tree/count and
all source hashes/modes, and proves the four product/test hashes unchanged before
and after. No `git reset`, `git clean`, old evidence overwrite, source copying or
metadata-only relabelling is used. New archive/bundle receipts remain separate.

Both routes use normal `cmake --preset dev`, `cmake --build --preset dev`, then
`ctest --preset dev -R` for the five identity/tray/split-service tests, followed by
standard release configure/build and CPack. The same candidate product bytes already
have full40/41-test and native12/12 evidence in the preceding cohort; this repeated
suite checks the new clean identity, tray and process contracts. It is not reported
as a repeated full suite. Release sets `BUILD_TESTING=OFF`, dev sets it `ON`.
Cached supplied archive provenance is explicitly cleared: this is a real clean Git
checkout, and the generated manifest must say `sourceProvenance=git`, `dirty=false`,
and the actual checkpoint commit/tree/count. Each command records argv, controlled
stdin, stdout/stderr, exit code and UTC timestamps in a new attempt directory.

Review the new build manifests, package hashes and JUnit before installing:

```bash
python3 "$native_replay" --output "$replay_output" install --guest cinnamon
python3 "$native_replay" --output "$replay_output" install --guest gnome
python3 "$native_replay" --output "$replay_output" install --guest kde
python3 "$native_replay" --output "$replay_output" start --guest cinnamon
python3 "$native_replay" --output "$replay_output" start --guest gnome
python3 "$native_replay" --output "$replay_output" start --guest kde
```

Installation retains the existing runner's package-only apt simulation, dependency
and extension guards, exact binary/manifest/version checks and profile preservation.
No new dependencies are provisioned. Cinnamon uses the exact host-built package.
Restore any lock through the visible console before graphical acceptance. Inspect
the startup frame, cancel Setup through its visible Cancel button, and record a
new native smoke/matrix, rendered tray and normal helper-exit receipt for this exact
checkpoint. The earlier45dirty receipts are supporting byte-equivalence evidence;
they cannot stand in for actual46 runtime identity or acceptance.

The frozen benchmark may score only the clean checkpoint with fresh output paths
and its actual archive/binary/native receipts. Do not use `--allow-dirty`, hidden
prewarming, or active build/graphical test workloads during physical measurements.

Failures remain failures. A partial cached transition may leave clean45 or clean46
with an incomplete fresh receipt. Do not blindly retry, reverse twice, overwrite the
failed directory, or discard the old patch. Inspect the recorded commands and actual
Git/cache state; the root must deliberately prepare a separate guarded recovery.
Read-only prepare inventory writes attempt receipts into the prior cohort but never
modifies its source manifest, installed marker, visual receipt or payloads.

Prepared validation: Python syntax and embedded remote-script syntax compiled; help
was exercised. No prepare/build/stop/install/start phase was executed by the preparer.
