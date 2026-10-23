# Native package full acceptance: drivers

These drivers run the [native package full-acceptance profile](../../../acceptance-matrix.md#native-package-full-acceptance-profile)
on the builders. There are no results yet: the pass waits for the coordinator's go and the exact nightly to accept.

| File | What it is |
| --- | --- |
| [nfa.py](nfa.py) | The driver: [afa.py](../../2026-10-22-rail-391-notify-415/lab/afa.py) (#415's guarded AppImage driver) with the installed package in place of the image, plus the lifecycle subcommands. `nfa.py -h` lists them; every afa.py command passes through, and `nfa.py rerun …` and `nfa.py i326 …` run [rerun.py](../../2026-10-22-appimage-full-acceptance-rerun/lab/rerun.py) and [i326.py](../../2026-10-22-rail-391-notify-415/lab/i326.py) on the same runs. |
| [stage.sh](stage.sh) | Copies nfa.py and what it needs into one folder, for an explicit absolute `rsync` destination on a builder. |

## What nfa.py changes in afa.py

- **The program:** `/usr/bin/putgd` and `/usr/bin/putgd-cli`, with no `LD_LIBRARY_PATH`, `QT_PLUGIN_PATH` or `APPDIR`. `putgd-record --motion` is the installed one.
- **Profiles:**
  - `--profile private` is afa's: the run's own `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` and runtime directory. The data home stays the user's.
  - `--profile default` is put.gd's own profile, as a user has it.
  - Either refuses to start if put.gd's folders already exist, and cleanup removes only what the run created.
- **The click guard:** the guard is widened as in the update profile's `ua_app.safe_handler`. A click needs the stand-in recorder, or a temporary Firefox (`ffdefault.py` or `ffprofile.py`), as the `http` and `https` default. It covers every command that takes real input.
- **Processes:** put.gd's processes are any whose executable is `/usr/bin/putgd*` or in a `putgd` library folder, so item K also catches `putgd-s3-worker`, `putgd-capture` and `putgd-kde-region`.
- **Package commands:** these go only through `nfa.py pkg`. It accepts apt, apt-get, dnf and pkcon with put.gd as the only target, and refuses a bare upgrade, `autoremove` and `dist-upgrade`.
  - It simulates the transaction first (`apt-get -s`, `dnf --assumeno`). It refuses one that would upgrade or remove another package, or touch the desktop stack (mutter, gnome-shell, kwin, plasma, Xorg, Mesa, PipeWire, the portals, the display managers).
  - The command runs with stdin from `/dev/null`, so a dpkg conffile prompt fails visibly instead of passing unseen.
  - Each run records the state before and after, the signature lines and the fetched files.
- **Cleanup:** `cleanup --appimage` is refused, because the "image" here is `/usr/bin/putgd`.

## Per desktop

The run root is `/home/patrick/work/put.gd-nfa` on each builder (or in the VM guest): `tool/`, `assets/`, and `runs/` (`AFA_ROOT`). It's the only path the pass removes at the end. Commands below run from it, with `N=python3 -I tool/nfa.py`, `TAG` set to the accepted nightly, `T` the target (`ubuntu24.04`, `ubuntu26.04`, `debian13` or `fedora44`), and the desktop's VNC port and input mode (uinput on physical Wayland, xdotool on X11, vnc in a VM).

**0. Before anything:**
- The coordinator's go, and the lock acquired and checked.
- Patrick's Firefox, ChatGPT and Codex windows closed by exact PID (record them).
- `$N versions --save runs/versions.json` and `$N state --save runs/state-0.json`. The state must be clean: no package, repository file or key, default profile or autostart entry. Otherwise, stop and ask the coordinator.
- Fetch the assets:
  - `$N fetch $TAG $T assets`
  - `$N fetch v0.1.1-dev.133.20261013004314+gb0ed0461c600 $T assets`, or `appimage` on d69 or when its DEB won't install
  - `$N fetch v0.1.1-nightly.947 $T assets`
- `u1_infra.py` (U1), unless the update pass already ran it on `$TAG`. Then cite it.

**1. L, fresh install from pkg.put.gd:**
- `$N repo-add assets/<accepted package> L`: the repository file and key, recorded in run `L`.
- `$N pkg L update -- apt-get update`, then `$N pkg L install -- apt-get install -y putgd` (dnf: `dnf install -y putgd`).
- `$N setup L --profile default --no-launch …` (it completes the run the package steps started), and `$N identity L $TAG --target $T` (A and J).
- First run: `$N i326 handler-on L`, then a real click on the menu entry (B) starts put.gd on the untouched profile. Picture the first-run setup.
- `$N configure L` (the fixture destination), a region capture with its upload and copied link, then `$N teardown L`.

**2. A–H, J, K on a private profile:**
- `$N setup P --vnc-port … --input …`, then `$N start P`, `$N memory P` (C, quiet host), `tray-menu`, `switcher`, `capture` ×3, `edit --dialog`, `record`/`control` (E), `history`, `delete`, `settings` (H). Plus real clicks for the Other channel copy and **Open software updates**.
- **F:**
  - Switch to the temporary Firefox for this step only: `ffdefault.py on` on d69 and k72, or `ffprofile.py on`.
  - `python3 -I tool/s3creds.py 2 | $N rerun live-s3 P --prefix acceptance/nfa/<desktop>/ --acl public-read`.
  - `$N notify P --label F`, then `$N rerun browser-shot P --label F-browser`.
  - Delete from destination, then `$N s3list acceptance/nfa/<desktop>/` on d69 (boto3).
  - Finally `ffdefault.py off` or `ffprofile.py off`, and check the files are byte for byte the same.
- `$N quit-tray P` (K), `$N i326 handler-off P`, `$N cleanup P --state`.

**3. M1, from dev.133:**
- Purge first (`$N pkg …`), then `$N pkg M1 install -- apt-get install -y ./assets/<dev.133 package>`.
- `$N setup M1 --profile default --keep-template --config-path ~/.config/putgd/putgd/config.json --cli /usr/bin/putgd-cli …`
- `$N seed M1`, quit, `$N grow M1 --to 10000`, start dev.133 again, then `$N snapshot M1 --label before`.
- `$N pkg M1 upgrade -- apt-get install -y ./assets/<accepted package>`, start the accepted build, then `$N snapshot M1 --label after`.
- `$N compare M1 before after`, then `$N history-check M1`.
- **GNOME:** `$N extension M1 --label after-upgrade` (`helper_outdated`). Re-login, with the coordinator's OK, then `--label after-relogin`, `--action disable` and `--action enable` (O).
- **d69:** seed with the dev.133 AppImage on the default profile (`--exe` and `--cli` pointing into its extracted tree), then install the accepted `debian13` DEB.

**4. M2, from nightly.947:**
- `$N teardown M1` and purge, then install `./assets/<947 package>`.
- `$N setup M2 --profile default …`, captures and one upload, then `$N snapshot M2 --label before`.
- With the resident running: `$N pkg M2 update -- apt-get update`, then `$N pkg M2 upgrade -- apt-get install -y --only-upgrade putgd` (dnf: `dnf upgrade -y --refresh putgd`).
- `snapshot --label after` and `compare`.

**5. I, then P and Q:**
- I:
  - `$N login M2 enable`;
  - a real logout and login, with the coordinator's OK and none of Patrick's apps open;
  - then `$N login M2 check --label I`.
- P:
  - `$N pkg M2 remove -- apt-get remove -y putgd` and `$N state`;
  - `$N pkg M2 purge -- apt-get purge -y putgd` (dnf: `dnf remove -y putgd`) and `$N state`;
  - a real login;
  - then `$N login M2 check --label P`.
- Q:
  - `$N autostart M2 --label after-removal`, plus a picture of the startup-applications settings;
  - `$N tryexec M2`;
  - a real login;
  - then `$N autostart M2 --label tryexec`.

**6. Cleanup:**
- `$N teardown M2` (default-profile folders, keychain entry, autostart entry, repository files left by `repo-add`), then `$N state --save runs/state-end.json`. It must match `state-0.json`.
- On f44, restore the `openh264` state.
- Remove the run root's `assets/` and `runs/` once the results are copied out. Release the lock with `--verified-clean`.

Rehearse the whole order once on the Ubuntu GNOME VM overlay before the physical desktops. It's a target anyway, and a driver fix there costs no desktop time.
