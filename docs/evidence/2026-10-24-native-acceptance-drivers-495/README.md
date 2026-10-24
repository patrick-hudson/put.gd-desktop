# Native acceptance driver hardening (#495) — 2026-10-24

Latest candidate `61439983e2c2d7b050b846e2f8bd723234247aed` contains three
narrow optional-review corrections and awaits focused independent review.
All accepted live packets and the producer proof below remain at `72afd00b`;
no new live/GUI/logout/m74 workflow is claimed for `61439983`. The accepted
exact-72 growth handoff and all its guards are unchanged. This documentation
PR stays draft for the final source/interface review and exact-head CI.

This is the documentation companion to the maintained driver in
`tools/lab/native_acceptance.py` in [code PR #554](https://github.com/patrick-hudson/put.gd-desktop/pull/554).
Merge #554 before [documentation PR #552](https://github.com/patrick-hudson/put.gd-desktop/pull/552).
The [structured record](summary.json) distinguishes historical observations,
driver checks and exact-build desktop evidence. Saved k72 results belong to
`826c255a` and `6bbbd506`; the f44 protocol check belongs to `64a3257b`, and the
bounded m74 observations to `64a3257b` / `853a35d2`. These precede the five-finding
safety resolutions. This record does not establish final driver safety, a new
native-package pass or release acceptance.

## Historical evidence and its limits

The [2026-10-23 native-package pass](../2026-10-23-native-full-acceptance/README.md)
tested `v26.1023.1058-nightly`, source `05991d9d`. Its drivers, result files and
visuals remain historical inputs. [Issue #495](https://github.com/patrick-hudson/put.gd-desktop/issues/495)
records six findings from that pass:

| Finding | What the old run proved or exposed | Required replacement evidence |
| --- | --- | --- |
| Tray target | The tint finder selected taskbar/panel items. k72 entered panel edit mode; m74 opened Add a new panel. Both were cancelled and panel settings were unchanged. | Resolve the registered StatusNotifierItem by explicit resident PID, exact executable and expected title. Use its D-Bus actions and owned dbusmenu's dynamically discovered exact Quit action. Reject coordinates and `TRAY_XY`. |
| Plasma session/cache | d69 lacked even Dolphin in its application cache; rebuilding sycoca in plasmashell's environment restored discovery and capture. | Identify the current graphical session and cache health; retain any refusal, repair and recheck. A generic SSH environment is insufficient. |
| Login start | An SSH-started `systemd-run --user` resident can survive graphical logout while the user manager remains alive. Early attempts therefore did not prove a new login start. | Record and quit the exact owned resident before logout. Bind before/after evidence to the real graphical session, boot and process start identities. |
| Removal and autostart | The old snapshot crashed after the CLI was purged; the first plasmashell PID could belong to the previous login. | Take removal-tolerant snapshots and compare the actual current session, including no resident after removal and a real login. CLI absence is an observed state, not a successful CLI query. |
| History shape | Migration preserved 10,000 entries copied into five operations. The UI grouped 2,000 files as one capture. | Seed distinct operations with consistent entry relationships and verify the resulting operation/entry counts and realistic History grouping. Old timings do not measure that new workload. |
| Keychain lifecycle | M1 uploads used session credentials; a second profile preserved a keychain reference and reported `credential_setup_required`. | Prove actual keychain write/read/use across restart or login with an unlocked backend. A retained reference, session credentials or a locked-backend refusal cannot establish this lifecycle. |

The old outcome tables and failure context are unchanged. In particular, the m74
P failure (#490) remains a failure in that record. Later host setup or driver fixes
do not retroactively turn those attempts into passes.

## Latest command interface (`61439983`, focused review pending)

Candidate source: `61439983e2c2d7b050b846e2f8bd723234247aed`,
`tools/lab/native_acceptance.py` plus `tools/lab/native_keychain.py`. The driver
SHA-256 is `7026a15a0ebcb5f6e25a0feb2679a8f6ee241624e8ed8c7bdd950b790a9a6333`;
the [JSON](summary.json) also records the helper and test hashes. This is
unmerged harness source, separately identified from the published product
package under test. The product CLI capabilities are unchanged.

This interface delta is under focused review. Preserve source-specific evidence
and every fixture/ownership/receipt guard. Accepted growth execution/proof remains
scoped to exact `72afd00b` until the final handoff; its guard bodies are unchanged. In particular, current localhost configuration
alone does not establish that stored journals/destinations are safe or that the
selected database is stopped.

Start with `python3 tools/lab/native_acceptance.py --help` and the selected
subcommand's `--help`. Mutating CLI commands require
`--output NEW_PRIVATE_RECEIPT`: its parent must be an owned private directory,
and the file is exclusively reserved before session discovery or any action.
An existing or invalid output refuses before effects. Failed command receipts
remain usable; retain them and their original safe error.
Graphical/session commands run on the desktop host under its lock. The tool does
not launch the app, install packages, log out or unlock a desktop.

Session selection uses the active local logind session and current shell
generation. On GNOME, the compositor's environment can omit `WAYLAND_DISPLAY`.
The `64a3257b` fallback treats the user-manager value only as a hint: the socket
must be owned, inside the current runtime and within the Unix socket limit, and
its `SO_PEERCRED` PID/UID must match the current logind-bound compositor with an
unchanged process generation. A stale manager export is not accepted on trust.

Run the following commands from the driver checkout on the execution host. Each
`--output` target must be new. Use the exact resident PID/executable captured by
the owned launch, and the actual profile runtime; do not select a resident by
the first matching process name.

```sh
python3 tools/lab/native_acceptance.py --output "$session_receipt" session
python3 tools/lab/native_acceptance.py --output "$tray_receipt" tray inspect --pid "$pid" --exe "$exe"
python3 tools/lab/native_acceptance.py --output "$menu_receipt" tray menu --pid "$pid" --exe "$exe"
python3 tools/lab/native_acceptance.py --output "$activation_receipt" tray activate --pid "$pid" --exe "$exe"
```

`inspect` authenticates the registered SNI against the resident's PID, exact
executable, process generation and put.gd title. `menu` inspects the owned
dbusmenu topology and discovers the enabled visible Quit action; it reports
`menuRendered: false`. `activate` invokes the owned SNI's `Activate` action,
which opens History. Quit uses the same identity guard and dynamically selected
action, without hardcoded menu IDs:

```sh
python3 tools/lab/native_acceptance.py --output "$quit_receipt" tray quit --pid "$pid" --exe "$exe"
```

These commands send no pointer input. Saved visible activation/Quit results can
prove those behaviors, but neither menu inspection nor D-Bus success proves a
rendered context menu or a physical tray-icon click. Those release-profile checks
remain separate graphical evidence.

For Plasma, build the small KService probe on the remote builder with distro
Qt 6 and KF6Service development packages. From `tools/lab/sycoca_probe`:

```sh
cmake --preset dev
cmake --build --preset dev -j2
```

Then, from the driver checkout root:

```sh
python3 tools/lab/native_acceptance.py --output "$cache_receipt" plasma-registration \
  --probe tools/lab/sycoca_probe/build/dev/putgd-sycoca-probe
```

Use a new receipt with `--repair` only if repair is needed. The driver queries
KService in the current session's menu/cache/locale environment, rebuilds sycoca
only when registrations are missing, and rechecks. A missing probe is a blocked
check; it does not establish that the cache is empty.

For a post-login presence check, enable the intended login entry through the product's
existing interface, then prepare before the real logout:

```sh
python3 tools/lab/native_acceptance.py --output "$before_login" login-prepare \
  --pid "$pid" --exe "$exe" --cli "$cli" --runtime "$runtime"
```

Preparation verifies the IPC peer, quits the exact resident and refuses surviving
profile processes. Perform the coordinated real logout/login through the visible
desktop, then check:

```sh
python3 tools/lab/native_acceptance.py --output "$after_login" login-check \
  --before "$before_login" --runtime "$runtime" --expected present
```

The receipt is bound to the same host/runtime and selected package executable;
the check requires a new graphical session and rejects old process generations.
A successful `61439983` response reports `postLoginStateVerified: true`,
`expected: present|absent` and `autostartProven: false`. It proves process
presence/absence after login, not desktop autostart origin: a new SSH/systemd
user-scope start can satisfy the same state. Independent launch-origin evidence
is required for an autostart claim.
For removal, prepare while the installed resident/CLI still exist, perform the
reviewed package removal, then a real logout/login, and use `--expected absent`
with that preparation receipt. A same-session snapshot does not replace this
login gate. Removal-tolerant state capture is:

```sh
python3 tools/lab/native_acceptance.py --output "$snapshot_receipt" snapshot \
  --runtime "$runtime" --cli "$cli" --db "$db"
```

It records `cliExists`, `cliStatus`, processes and available database facts even
when the CLI or data is absent. An `ok` receipt here means facts were collected;
it is not itself a package-removal verdict.

The history-growth command is:

```sh
python3 tools/lab/native_acceptance.py --output "$receipt" grow \
  --fixture-root "$fixture_root" \
  --db "$fixture_root/data/putgd/history.sqlite" \
  --runtime "$runtime" --to 10000
```

Set these paths to the actual disposable fixture on the execution host; use a
fresh receipt and the same short private runtime used to launch that fixture.
`grow` does not create a configuration or seed captures. The reviewed implementation
requires an owned private root, the exact `ROOT/data/putgd/history.sqlite` and
private data directory, plus `config/putgd/config.json`. Use fresh synthetic
seeds with session-only credentials. Numeric loopback HTTP(S) destinations need
an explicit port (`127.0.0.1` or `::1`); `localhost` is not accepted by this guard.
It inspects persisted JSON journals/receipts/state as well as current config,
rejecting external/unknown/default-provider routes, non-loopback URLs,
keychain/embedded credentials, upload/multipart/cleanup recovery state,
deletion/sharing journals and unknown tables. Existing external fixtures must
not be rewritten into apparent localhost fixtures.

The `72afd` compatibility exception accepts only the full exact inert default
`StorageReceipt` for a terminal operation with `failed`, `local_only`,
`cancelled`, `skipped` or `conflict` upload status and no requested remote/share
action. Preserve every serialized field; do not strip `receipt` from the seed.
Published/unknown/active states or an altered default do not qualify, and the
exception does not apply to `shareReceipt`. Populated receipts still require a
complete isolated destination, and recovery-state validation remains strict.

Schema 5–7 terminal journals need matching `item_state` rows. Stop the fixture
normally first: checks cover associated data-home/open-database owners across
runtimes, plus `putgd` and the full `putgd-*` helper prefix, including deleted
executables; unreadable owned helpers refuse absence. The guard holds
`resident.lock` through the transaction and commit, rechecking owners under the
lock. It refuses every preexisting lock; never break or delete one to make the
test proceed. The selected runtime's socket must be absent; do not remove
another session's socket to satisfy the gate. Permission does not extend to
any predecessor source or bypass any of these checks.

Each seed history row must already have its own operation. The old expanded
five-operation fixture is rejected. Copies receive unique operation/item IDs,
one matching journal item and spaced past timestamps while retaining the small seed
payload/preview paths, settings and statuses. The transaction checks integrity
and the requested independent operation/item counts. It performs no uploads.
A synthetic shape check does not establish UI performance or migration behavior
on a product build; those need their own run identities and results.

The maintained tool covers the focused #495 checks. It does not establish a
complete A–Q package campaign by itself; a future campaign still needs reviewed
orchestration for the remaining scenarios and exact-asset evidence.

The old `nfa.py` pass-through, `nk.sh`, `find-tray`, tint/coordinate tray activation
and `quit-tray` recipes are retired for new campaigns. Do not use `--x`, `--y` or
`TRAY_XY` as an escape hatch. The preserved [old driver guide](../2026-10-23-native-full-acceptance/lab/README.md)
explains the 1058 run; it is not the current runbook.

Before any isolated Linux launch, use a fresh short private output/runtime path
and check every complete socket path, including `putgd/session.sock`, with
`python3 tools/diagnostics/check_socket_paths.py --socket COMPLETE_PATH` (or
`--ui-output` for UI-harness output directories). Reject paths over 107 UTF-8
bytes before launch. Follow [remote-builder locking and visibility](../../remote-builders.md)
and [VM lifecycle rules](../../vm-lab.md). This documentation thread runs no GUI
campaign; the code owner coordinates execution and desktop locks.

## Narrow optional-review follow-up (`61439983`)

The four-file delta adds the explicit post-login scope above, validates canonical
keychain reference and original default collection **before any Unlock/Prompt**,
and changes the probe preset from schema 6 to 5. Invalid or foreign receipts
must refuse without unlocking even another approved lab collection. The stored
reference/default comparison does not wait until after an unlock side effect.

The saved 39-test k72 log passes in 2.937 s. Running the two new regression
methods against `72afd` reproduces six unsafe-Unlock subcase failures plus one
missing-output-scope error, as expected. Source/test and before/after/probe log
hashes are indexed in `review4/manifest.json`; all prior live evidence remains
at its original source. No live keychain, graphical or logout rerun is claimed
for this delta.

Preset listing, configure and build passed with actual distro CMake 4.2.3.
[CMake's official 3.24 documentation](https://cmake.org/cmake/help/v3.24/manual/cmake-presets.7.html#format)
identifies preset schema 5 as supported from 3.24. This is a compatibility
statement, not a claim that CMake 3.24 was executed. Focused review is pending.

## Review resolutions and serializer compatibility (`72afd`)

Independent round 2 source review resolved the five original findings. The
`c538` resolutions reserve output before actions, validate persisted
network state, bind growth to the exact database and its resident lock, inspect
the complete associated helper family, and preserve failed-store recovery.
The saved 35-test log passes on k72 in 1.550 s. A separate private Qt witness
reports that an actual `QLockFile` and the growth guard exclude each other in
both directions, Qt can acquire after release, and no lock remains. Witness
source/binary hashes and result JSON are indexed. These are safety/protocol
checks, not a product or graphical acceptance run.

Round 2 also found that real `OperationItem::toJson()` always emits a nonempty
default receipt, even for failed/local-only items. `72afd00b` accepts only the
inert case described above. Its saved 37-test log passes in 2.754 s with complete
product-shaped records. A private standalone Qt witness compiled the actual
production serializers and emitted failed/local-only items with receipts
untouched. The validator accepted those records; the actual growth CLI then
created 10,000 independent operations/items/journals/states from a fresh
synthetic schema-7 fixture, integrity `ok`, zero uploads. Production serializer,
witness source/binary, emitted-record and CLI-receipt hashes are indexed.
This is current `72afd` serializer/unit/CLI proof; the `c538` desktop and 35-test
records remain predecessor evidence. The current `72afd` Cinnamon packet below
has its own receipts. Independent round 3 cleared this exact source, including
25 accepted and 16 rejected validator cases; these are distinct from the 37
suite tests. Final source-specific k72/f44 packets are recorded below.

For a failed keychain store, `c538` persists the canonical synthetic recovery
reference before creating the item and retains the original safe error and
cleanup status. It attempts exact owned-item cleanup; if recovery is needed,
use a new output with `keychain cleanup --before FAILED_RECEIPT`. Cleanup is
idempotent when the exact item is already absent. Provider-added metadata such
as `xdg:schema` is permitted only while required application/reference ownership
still matches. Retain failed and recovery receipts; a successful cleanup does
not turn the failed store into a roundtrip pass. The fresh public-CLI observations
below do not approve safety; historical observations are not transferred.

## Predecessor public-CLI observations (`c538`)

The fresh k72 run at 15:19Z uses `c538f71e89afcc6d1e6c599c3c1cb9c3a97e7ced`,
driver SHA `2f317d31a85741c7a24382849f309433a2581ee127f28c52de3a859a74069258`,
and the installed 1185 product in the separately identified native Wayland
session `906`. Each public CLI step has a newly reserved individual receipt.
Owned SNI `:1.6230`, resident PID `145684`, activated History; dynamic Quit
stopped it. Preparation stopped a second IPC-peer-verified resident and the
same-session login check correctly refused. Backend store/read/cleanup ran in
separate CLI processes; the store receipt retains its durable pre-creation
recovery reference. Post-purge snapshot succeeds with `cliExists: false`.

The socket/cover preflights pass (39 bytes, 100% cover). Newly captured activation
and after-Quit images again match the exact bytes of the linked k72 visuals;
their independent `final-c538` receipts and hashes are indexed. This is fresh
capture provenance, not reused evidence. No real logout/login, physical click,
rendered context menu or product QtKeychain lifecycle is claimed.

The private public-CLI wrapper is `work/495/final_probe_review2.py`, SHA
`93a9bd25f81122c5600db8a0ac3f53fc4d74d0d7774f31681556260106f8294a`.
Recorded completed invocation:

```sh
cd /home/patrick/work/n495
python3 work/495/final_probe_review2.py /home/patrick/putgd-lab/n495/final-c538 c538f71e89afcc6d1e6c599c3c1cb9c3a97e7ced
```

Stdout is `/home/patrick/putgd-lab/n495/final-c538.log`; preserve that completed
directory. The wrapper's original bytes are retained privately. Lock
`codex-495-r2` was released cleanly at 15:19:51Z with empty cleanup lists;
package, runtime and synthetic probe were removed. This predecessor packet
keeps its original scope; exact-72 permission does not authorize c538 growth.

## Current Cinnamon public-CLI observations (`72afd`)

The fresh physical m74 run uses exact driver source
`72afd00bf2dbdaaa7e4bd3f847dc54f1da715f0e`, main SHA `347934c2…`, helper SHA
`c5b80a68…`, on Mint 22.3, Cinnamon `6.6.9+zena` X11 and distro
`libqt6core6t64 6.4.2+dfsg-21.1build5`. The installed `ubuntu24.04` DEB remains
product 1185 / `9dfac15c`; its independently verified #379 checksum is
`2094fd4db8cd915b02f1cdd16eb5c99bb8191948931cd2f558a89c59be5b3f47`.
Identity JSON records the exact installed resident/CLI/UI binary hashes.

Session `c89` and current Cinnamon PID `625683` are retained in individual,
newly reserved public CLI receipts. The owned SNI activated History and dynamic
Quit stopped the resident. Preparation quit an IPC-peer-verified second
resident; the same-session check refused. Separate-process backend
store/read/cleanup succeeds with `productKeychainRoundtripProven: false`.
Post-purge snapshot records the absent CLI without crashing. This is no
positive real logout/login, cross-login keychain or product QtKeychain proof.

The exact new [activation crop](pictures/m74-72afd-activation.png), SHA
`e90ce38a1d9eb42df41fc2d24ff13b1a534afaa52fc751368419be985d984833`, and
[after-Quit cover](pictures/m74-72afd-after-quit.png), SHA
`6fca3e74cf3cb4d5b4d926b7d1e2921464ac13025a6beafc8ecd2d5784b20a91`,
were hash-verified and visually inspected: only the owned app and magenta cover
appear. The socket/cover preflights passed; the after-Quit frame is 100% cover.
There was no pointer input, physical tray click or rendered-menu proof.

Private wrapper `final_probe_review2.py` retains SHA `93a9bd25…`; the recorded
command from `/home/patrick/work/n495` was:

```sh
python3 work/495/final_probe_review2.py /home/patrick/putgd-lab/n495/final-72afd 72afd00bf2dbdaaa7e4bd3f847dc54f1da715f0e
```

Stdout is `/home/patrick/putgd-lab/n495/final-72afd.log`. Lock
`codex-495-final-mint` was released cleanly at 15:32:08Z with empty cleanup
lists; the owned package, runtime and synthetic probe were removed and the
preexisting VM left running. This is bounded functional evidence, not a
performance/release acceptance pass. Independent review clearance applies only
to the exact driver and guarded uses described above.

## Final exact-source KDE and GNOME packets

On physical k72, exact `72afd` reran the public CLI in native Plasma 6.6.6
Wayland session `906`. New individual receipts and newly captured frames record
owned SNI inspection/activation/dynamic Quit, peer-bound preparation and
same-session refusal. The new frames are byte-identical to the linked reviewed
k72 visuals, with independent `final-72afd` run/hash provenance. Backend
store/read/cleanup and the actual-purge snapshot succeed. Current-session
KService inspection is healthy (235 applications, Dolphin and put.gd present);
no repair was needed or run. Native product 1185 / `9dfac15c` remains separate
from the driver source. The same public wrapper SHA `93a9bd25…` ran from
`/home/patrick/work/n495`:

```sh
python3 work/495/final_probe_review2.py /home/patrick/putgd-lab/n495/final-72afd 72afd00bf2dbdaaa7e4bd3f847dc54f1da715f0e
```

Stdout is `/home/patrick/putgd-lab/n495/final-72afd.log` on k72. Its
`codex-495-final-kde` lock ran 15:41:57–15:43:03Z and released cleanly after
package/probe/runtime removal. The export manifest's 17 members were
independently verified.

On physical f44, exact `72afd` performs session/backend protocol verification
only: Fedora 44, GNOME `50.6-1.fc44`, Qt `6.11.2-3.fc44`, Wayland session
`1100`, current GNOME Shell PID `144305`, verified socket
`/run/user/1000/wayland-0`. Main/helper hashes match the reviewed source.
Backend status starts locked; the approved existing lab collection is unlocked
through the guarded protocol and a unique synthetic store/read/cleanup succeeds
in separate CLI processes. No app, package, cover, pointer input, logout or
capture ran. This is no GNOME tray or graphical-app acceptance claim.

The f44 `codex-495-final-gnome` lock ran 15:43:25–15:43:58Z and released cleanly,
with its unique probe removed. Its export manifest's seven members were
independently verified. All final Cinnamon/KDE/GNOME release receipts have empty
cleanup lists, and their recorded times verify sequential locks. Preexisting
collectors and the original VM were preserved. Full source/product/session,
artifact/visual, failure and cleanup identities remain in [summary.json](summary.json).

## Saved k72 results

Two separately recorded runs used the installed `ubuntu26.04` DEB of
`26.1024.1185-nightly`, product commit
`9dfac15c1ced201d13a2b70a8afb0c253213768b`, tree
`4e96912274d9220ba9476bd1b50333f6cf738f4f`. The exported package independently
hashes to `2f370159c4cd5ebefbe446dc5461daf3c991095bbbe0e7220bb443277fe294ce`.
They ran on physical k72 (`kubuntu`), Kubuntu 26.04.1 (Ubuntu 26.04.1 LTS), Plasma
6.6.6 on native Wayland, Qt 6.10.2 and KF6Service 6.24.0, logind session `906`,
current `/usr/bin/plasmashell` PID `129168`. The exported package's extracted
`putgd` hashes to `714c046908fe3b24f707323e7dce1e0257767ca30371548a9996bdbd633f1c83`
and `putgd-ui` to `c973b5e195c710bb73eaa6551e7c723658a1e7f8eea5e87db0432ff580e9de71`.
Exact boot/session/process start identities, distro package versions, probe
binary hash, private receipt paths and SHA-256s are in [summary.json](summary.json).

| Run | Driver | What was observed |
| --- | --- | --- |
| `k72/final-826c` | `826c255a4023daad88d07afb2b82d558f86c9d69`, SHA `8ef8180e…` | Authenticated SNI `:1.6059`, resident PID `141786`; activation opened History, dynamic Quit stopped it. A second resident was peer-verified and quit by login preparation; a check in the same session correctly refused. |
| `k72/final-6bbb` | `6bbbd506aeef7f8aa4a97c21664c7013ff427627`, SHA `1375c3cc…` | Fresh repeat after earlier local corrections, before independent round 1; SNI `:1.6139`, resident PID `142921`; the same behaviors and same-session refusal, with independent receipts. |

Both runs selected Quit action `12` from the current owned menu; that value is
an observation, not an ID embedded in the driver. No pointer input was sent and
`menuRendered` was false. Both socket preflights passed at 39 UTF-8 bytes; cover
prechecks were 100%. The retained images happen to be byte-identical across the
two runs; each run's own receipt records its hash, and no acceptance credit is
transferred between sources.

Execution used the private functional wrapper `work/495/final_probe.py`, SHA-256
`becbd6aa88281103f3b07755d16c744b1877518c02f83478d3b1cb28128fdf42`.
It is separate from the maintained tool and the published product. The recorded
invocation for the completed `6bbb` run was:

```sh
cd /home/patrick/work/n495
python3 work/495/final_probe.py /home/patrick/putgd-lab/n495/final-6bbb 6bbbd506aeef7f8aa4a97c21664c7013ff427627
```

Its stdout is `final-6bbb.log`; preserve the completed output directory. Use a
new private directory and the selected source identity for another run, after
the required preflight and lock. Wrapper bytes and each run's independent
receipts are indexed even though the resulting PNG bytes match.

| Saved visual | Scope |
| --- | --- |
| [k72 activation](pictures/k72-activation.png) | History after authenticated SNI activation; only the owned app and magenta cover are visible. SHA-256 `992a031d596d7e6e7522a74e48b31aca6ccd7e9ed532e4e0d62a47bf4117bafe`. |
| [k72 after Quit](pictures/k72-after-quit.png) | Only the magenta cover remains after owned dynamic Quit; process termination is established by the paired driver receipt. SHA-256 `72da30a440ce90373d3b394810fcb2b0f8bbf3d6cfdd734e3960ef1d1aeae7bb`. |

The documentation owner verified hashes and inspected the exact saved image
bytes. These are activation/Quit visuals, not physical tray-click or rendered
menu proof. Raw bulk evidence remains private on k72 and in the code owner's
export; the JSON indexes the reviewed files.

On the historical `6bbb` run, the KService probe found 235 applications, including Dolphin
and put.gd. The cache was healthy, so no repair ran. The repair branch is not
claimed as a desktop-tested result. The saved 18-test log passes in 1.259 s;
its synthetic-fixture SQLite `ResourceWarning` diagnostics are retained in the
private log. The actual harness `grow` command reports 10,000 history rows,
independent operations/items, operation journals and item states, integrity
`ok`, and zero uploads. This uses a synthetic schema-7 fixture, not 10,000 real
captures or a product performance/migration run.

The `826c` snapshots before and after an actual purge both succeeded: the later
receipt records `cliExists: false`, no profile process and absent database.
This proves collection tolerates removed binaries; it did not include a real
logout/login and is not a current-`6bbb` removal acceptance claim. Neither k72
run claims a positive new-session login-start or post-removal login check.

Both locks were released cleanly: `codex-495` at 14:51:37Z, then
`codex-495-final` at 14:54:13Z, with zero `left`, `notMine`, `preexisting` or
unmatched entries. The JSON preserves
the release receipt hashes/paths. Owned runtimes were removed and packages
purged. The later candidate fixes inventory of executables marked `(deleted)`;
the earlier evidence remains bound to its original driver.

The later `64a3257b` candidate adds authenticated GNOME display fallback. Its
saved k72 test log passes 19 tests in 1.261 s; the saved 18-test and graphical
records above remain evidence for `6bbb`.

On f44, `64a` performed a session-only protocol check at 14:58:21Z: native
Wayland session `1100`, current `/usr/bin/gnome-shell` PID `144305`, whose
environment lacked `WAYLAND_DISPLAY`. The manager hint
`/run/user/1000/wayland-0` was accepted only after the actual socket peer matched
that compositor's PID/UID and unchanged generation. Both safe JSON receipts were
hash-verified. No app, package, cover, input, logout or screenshot was involved;
this establishes session-protocol verification, not GNOME tray or graphical
acceptance. Lock `codex-495-gnome` was released cleanly at 14:58:34Z, with all
cleanup lists empty; its receipt and hash are indexed.

## Saved m74 observations (historical `853a`)

The bounded slot ran from 15:00:12Z to 15:02:02Z under `codex-495-mint` on
physical Linux Mint 22.3, Cinnamon 6.6.9 X11, session `c89`, current Cinnamon
PID `625683`. Product `1185` / `9dfac15c` remained separately identified.
Source `853a35d21bf56c64eb07944dd360f4bc96a7c718` used main-driver SHA
`8f1a8f49…` and keychain-helper SHA `6380532f…`; the full hashes and installed
native binary identities are in the JSON. The Qt package query omitted its
version; the later `72afd` identity captures it explicitly without rewriting
that earlier receipt.

The owned SNI `:1.4843`, resident PID `634132`, activated History and dynamic
Quit stopped it. A second resident was peer-verified and quit during preparation;
the same-session check refused. Post-purge snapshot collection succeeded with
`cliExists: false`. No real logout/login or pointer input occurred. The socket
preflight passed at 39 bytes and the cover precheck was 100%.

The reviewed exact-byte [activation crop](pictures/m74-853a-activation.png) and
[after-Quit cover](pictures/m74-853a-after-quit.png) contain only the owned app
and magenta cover; hashes are indexed. They do not prove a physical tray click
or rendered context menu. Both the original synthetic entry and the fresh probe
were removed, the package purged and the owned runtime removed. The release
receipt has empty cleanup lists. These are source-specific observations before
the five review findings are resolved; they supply no final safety/readiness
credit.

On m74 the `64a` helper stored successfully but refused read and cleanup because
GNOME Secret Service added `xdg:schema`. That failure is retained. The correction
required the exact application/canonical reference while allowing provider
metadata, removed the original owned item, then completed a fresh `853a`
store/read/cleanup sequence in separate processes. Receipts hash the main
driver; the separate source identity supplies the helper hash. This correction
does not by itself resolve the five independent safety findings.

## Keychain results and remaining prerequisites

The keychain command exercises a unique synthetic Secret Service item:

```sh
python3 tools/lab/native_acceptance.py --output "$key_status" keychain status
python3 tools/lab/native_acceptance.py --output "$key_store" keychain store
python3 tools/lab/native_acceptance.py --output "$key_check" keychain check --before "$key_store"
python3 tools/lab/native_acceptance.py --output "$key_cleanup" keychain cleanup --before "$key_store"
```

Run `check` in a separate process; for a claim across login, retain the same store
receipt and check after the separately verified real login. `--unlock-lab` may be
added to the lifecycle commands only for the approved existing lab collection:
`ksecretd` / `putgdlab` or `gnome-keyring-daemon` / `putgd lab`. It requests Secret
Service Unlock/Prompt, types no password and refuses any other provider/label.
It never resets a collection or changes the default alias. A refused prompt is
an unmet prerequisite, not a lifecycle pass; retain the refusal and any later
correction. Cleanup targets only the unique synthetic item from the store receipt.

The preserved k72 predecessors include two locked-backend refusals and later
store/read/cleanup success under different per-phase driver hashes. The historical
`6bbb` repeat has separate store, read and cleanup receipts with the same final
driver hash and provider `ksecretd`; the unique synthetic entry roundtripped and
was removed. No collection was reset and no default alias changed. This is not
evidence of persistence across a new login. It is backend evidence only: the
driver explicitly reports
`productKeychainRoundtripProven: false`. Product QtKeychain setup, use and
restart/login remain separate proof, even when the backend roundtrip succeeds.

The original keychain problem described the 2026-10-23 pass. The later
[builder lab-keyring setup](../../remote-builders.md#proton-sessions-on-the-builders)
records default lab keyrings on all four hosts. Inspect their current availability
and use an isolated test entry; do not infer a put.gd keychain lifecycle pass from
that setup or modify existing Proton sessions. Record locked/missing backends as
prerequisites until the real lifecycle is exercised.
