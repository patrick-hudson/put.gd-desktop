# U11 GNOME acceptance workspace

Source: `../source.json` and `../source.tar`, commit `97ca3a521714e2717cedc1139976130627bfb8a8`.
Use only the existing `work/lab-runs/current-gnome-a3d70dc` overlay (SSH 22236, VNC 5936). No new VM is created. Keep its output visible on the VNC wall at port 8765. Golden disks are immutable.

Scripts are run from the repository root with the ordinary host Python:

1. `python3 work/u11/native/gnome/build.py` copies and verifies every archived file, then uses CMake Dev/full CTest and Release/DEB. It writes exact commands, logs and a package checksum. The guest directory is `/home/lab/putgd-u11-97ca3a521714`. Do not run a second Ninja in those caches.
2. `python3 work/u11/native/gnome/install.py` requires the completed build receipt and the recorded old resident identity. It checks all old operations are terminal, stops that exact PID through pidfd, preserves the whole synthetic `putgd-lab` directory, installs the DEB, checks all three product binaries and extension bytes, and reboots only this existing VM to load fresh GNOME Shell JS. It saves before/after boot IDs and a VNC witness.
3. Inspect `fresh-shell.png` before native acceptance. The known disposable guest has normal GDM automatic login. If a later idle lock occurs, verify the owned `lab` Wayland session before `loginctl unlock-session`, then restore it through QMP Escape and preserve a witness. Do not alter lock policy or hide the display.
4. `python3 work/u11/native/gnome/run-matrix.py` runs the frozen native lab runner with Qt/GTK synthetic payloads and local-only storage. It preserves every result and stops at the first failure. A known historical GNOME region failure is unresolved: the old runner has only a 500 ms selector delay, not a readiness signal. Never rerun to erase a failure; correlate and cancel only the exact new operation through production `operation.cancel` before other interactions.
5. `interact.py ACTION LABEL ARGS...` provides explicit visible QMP clicks/keys, UI opening or a version-checked configuration patch. Every action appends metadata and a witness, resident identity, helper state and tray availability. It is not a UI oracle: inspect the resulting image. Use distinct labels, because previous witnesses are never overwritten.

Native memory sampling is forbidden until the root agent declares the functional/build wave quiescent. Headless Dev tests, a visible tray icon, UI close lifecycle, native capture correctness and total-family memory are separate gates.

No real credentials, cloud operations, tracked source edits, Git changes or other VMs belong to this unit.

The user requested immediate removal of the old live app during compilation. `stop-old.py` verified PID 59314/start 922289 and all 15 terminal operations, then stopped it and preserved its synthetic state. `preserve-state.json` records the result. The installer now also accepts this explicit record only after verifying the saved identity, all recorded terminal states, preserved state directory, old PID absence, and absence of any current `putgd` resident. It never relaunches old code.
