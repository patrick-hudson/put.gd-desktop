# U11 KDE acceptance work area

This unit reuses `work/lab-runs/current-kde-a3d70dc` (SSH 22234, VNC 5934).
It never launches another VM or writes a golden image. All graphical interaction
uses visible QMP input and QMP witness frames while the VNC listener remains active.

Root must supply the final committed `../source.json` and `../source.tar` before
`build.py` runs. The source archive and every source file are checked in the guest;
Dev tests and Release packaging use standard CMake presets and the distro toolchain.
The guest build gets its own directory. Prior baseline binaries, receipts and
synthetic profile are retained before package installation. No cloud credentials
or uploads are part of this unit.

Planned order:

1. `python3 work/u11/native/kde/build.py` after root releases the source freeze.
2. `python3 work/u11/native/kde/install.py` after a successful build receipt.
3. Restore any locked desktop visibly and inspect the first-run UI. Configure the
   synthetic local destination; close the helper normally and verify zero exit.
4. `python3 work/u11/native/kde/matrix.py`: twelve native Qt/GTK capture,
   clipboard and cancellation cases, with the UI helper absent before and after.
5. Drive the actual tray menu and all four actions plus Settings, History,
   Activity and About through `steps.py`; preserve each attempt's timestamp,
   QMP commands, witness, resident identity and helper state. Check normal final
   close, fresh helper reopens, draft/conflict behavior and native folder dialog.
6. Prove own-helper and modal-window capture rejection, clipboard survival after
   normal UI exit, and report ongoing-upload/S3/history-deletion gates accurately.
7. Only after root says the functional wave is quiescent may memory sampling run.

Failed attempts remain in their original logs/receipts. A later run does not erase
or reinterpret a prior failure. Offscreen Dev tests and actual KDE acceptance are
reported separately. A screenshot alone does not establish lifecycle, byte or
operation correctness; each assertion is paired with its independent evidence.
