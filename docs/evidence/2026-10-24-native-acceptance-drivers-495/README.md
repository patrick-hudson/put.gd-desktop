# Native acceptance driver hardening (#495) — 2026-10-24

This is the documentation companion to the maintained driver in
`tools/lab/native_acceptance.py` in [code PR #554](https://github.com/patrick-hudson/put.gd-desktop/pull/554).
Merge #554 before [documentation PR #552](https://github.com/patrick-hudson/put.gd-desktop/pull/552).
The [structured record](summary.json) distinguishes historical observations,
driver checks and exact-build desktop evidence. Saved k72 results belong to
`826c255a` and `6bbbd506`. The maintained candidate has since advanced to
`64a3257b`; its bounded m74 functional run remains pending. This record does not
establish a new native-package or release acceptance pass.

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

## Maintained commands

Current driver source: `64a3257b20d7aa33cc25d35c07a628becff4036c`,
`tools/lab/native_acceptance.py` plus `tools/lab/native_keychain.py`. The driver
SHA-256 is `8f1a8f49fd3f72d574493ebcb43e7ae51db253da7889a0ca51ef7f68ba8ecf4c`;
the [JSON](summary.json) also records the helper and test hashes. This is
unmerged harness source, separately identified from the published product
package under test. The product CLI capabilities are unchanged.

Start with `python3 tools/lab/native_acceptance.py --help` and the selected
subcommand's `--help`. Receipts use `--output NEW_PRIVATE_RECEIPT`: its parent
must be an owned private directory, and the receipt must not already exist.
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

For a login-start check, enable the intended login entry through the product's
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
`grow` does not create a configuration or seed captures. It requires an owned
private root and database, `config/putgd/config.json` with loopback-only profile
endpoints, and schema 5–7 terminal operation journals with matching `item_state`
rows. Stop the fixture normally first: no resident/helpers may remain in that
runtime and its socket must be absent. Do not remove another session's socket to
satisfy the gate.

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
| `k72/final-6bbb` | `6bbbd506aeef7f8aa4a97c21664c7013ff427627`, SHA `1375c3cc…` | Fresh repeat after review fixes, SNI `:1.6139`, resident PID `142921`; the same behaviors and same-session refusal, with independent receipts. |

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

On current `6bbb`, the KService probe found 235 applications, including Dolphin
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
store/read/cleanup success under different per-phase driver hashes. The current
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
