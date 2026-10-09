# Native application initialization locator

Status: scaffolding prepared; not built or run. This compares initialization
cost, not working product variants or tray functionality. No memory saving has
been established. Root's ongoing physical baseline must finish before any
configure, build or run command below is executed.

Use the actual **Cinnamon session first**. Build and check it there before any
GNOME/KDE build or test. If Cinnamon fails, fix or reject the locator first.
Only after that gate may separate guest checks run in parallel; full native
acceptance is still necessary for any eventual product implementation.

The three standalone C++20 executables use the system compiler, CMake, Ninja
and dynamically linked distro Qt, with a Qt 6.4.2 API minimum:

- `init-core`: QCoreApplication, Qt Core and DBus.
- `init-gui`: QGuiApplication, Qt Core, Gui and DBus.
- `init-widgets`: QApplication, Qt Core, Gui, Widgets and DBus.

They preserve the caller's desktop/theme/font environment. They create no
windows, tray, menu, clipboard owner, shortcut, file, product service or name
registration. They emit fixed JSON after application construction and before
normal event-loop exit, five seconds later by default. Only `--idle-seconds`
(integer 2–60) and `--session-bus` arguments are accepted; Qt override flags are
rejected. No application style, palette, fonts or graphics backend are forced.

With `--session-bus`, a timer connects to the existing session bus one second
after construction and emits another fixed phase marker. It calls no desktop
method, exports no object and registers no well-known service name. Ordinary
bus authentication/connection still occurs. GUI/Widgets native integrations
may already contact the bus during their constructors; this explicit stage is
not proof of their first bus use. All binaries link DBus even without the flag,
so constructor-only means **no explicit locator bus connection**, not absence
of the DBus library.

## Future configure and build

Run in `/home/patrick/work/put.gd/work/resident-native-init-locator` only after
the current baseline ends. These are normal standalone CMake presets; no
substitute compiler/runtime or additional package is supplied.

```bash
cmake --preset dev
cmake --build --preset dev
```

Preserve the actual configure/build command stdout, stderr, return codes and
toolchain identity beside the run receipts. No configure/build has executed
for this scaffolding yet. Do not run guest builds while physical observations
are active.

## Future Cinnamon checks and observations

Direct smoke checks, all expected to emit `constructed` then `idle-finished`
and exit normally with code zero:

```bash
./build/init-core --idle-seconds 5
./build/init-gui --idle-seconds 5
./build/init-widgets --idle-seconds 5
```

Optional bus smoke check adds `session-bus-connected` between those stages:

```bash
./build/init-core --idle-seconds 5 --session-bus
```

The runner imports only the existing, hash-pinned production performance
observer helpers: `identity`, `same_identity`, `children`, `memory`, `sha256`,
`write_json`, `system_metadata` from `bench_ui_process.py`, and grouped
`footprint` from `bench_idle.py`. It does not invoke either product workload
runner. An import performs no app/display action. Its fresh output directory
preserves six serial runs in balanced order:
Core → Gui → Widgets → Widgets → Gui → Core. No retry-to-green is automatic.
Run only when other builds and graphical tests are idle:

```bash
python3 run.py --output results/cinnamon-constructor-01 --idle-seconds 5
python3 run.py --output results/cinnamon-session-bus-01 --idle-seconds 5 --session-bus
```

These output names are examples for a first attempt. Never overwrite them;
use a new numbered directory for a subsequent attempt. Each observation
records constructor-phase memory and settled memory 0.5 seconds before the
configured normal exit. Bus runs add a post-connection snapshot. Raw stdout
JSONL, stderr, actual argv, monotonic observation offsets, process start-time
identity, RSS/PSS/private memory, grouped mappings, normal exit codes, source
and binary hashes, observer hashes, CMake cache and toolchain hashes are saved.
Loaded shared-object hashes and system metadata are collected **after** all
sampled runs, reducing observation disturbance. The inherited environment is
unchanged; only the named non-secret desktop/Qt fields are recorded.

The C++ constructor duration begins before constructing the application object;
it excludes dynamic-loader and process-spawn time. Memory snapshots occur
after the stdout marker and do not freeze the event loop. The observer adds
some work; five-second runs do not establish cold-idle budgets, p95 latency or
product savings.

A bare Core application is not a put.gd resident estimate. A working resident
still needs tray registration/menu/notifications, shortcut and clipboard
lifetimes, capture authority, settings/journal/credentials and IPC. The current
resident already uses native Unix sockets; the transient CLI still links
QtNetwork. Any communications saving must be attributed separately. Keeping
native Qt fonts/dialogs in the existing on-demand UI remains mandatory.

Required follow-up before a product experiment: locate the native
StatusNotifier/menu closure on the actual Cinnamon host, verify late tray-host
availability and restart, then define a bounded approved scope and matched
whole-family workload. This locator exports no tray item and makes no tray
parity claim.
