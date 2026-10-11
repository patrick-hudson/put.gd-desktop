# U11 Cinnamon native acceptance replay

This directory owns only the existing `current-cinnamon-a3d70dc` overlay. Its VNC
listener remains on localhost:5938 throughout all graphical interaction. Do not
launch another guest, touch its golden disk, or reuse these tests as physical
memory evidence.

The root supplies `../source.json`, `../source.tar`, and the successful host build
receipt. Verify the package source commit before installation. The historical
host baseline under `work/memory-surfaces/integrated-current/host/source/` remains
untouched for a later matched memory comparison.

The phases are explicit so no memory observation accidentally overlaps build or
functional work:

```sh
python3 work/u11/native/cinnamon/native.py install --source work/u11/native/source.json --deb EXACT_PACKAGE_FROM_ROOT
python3 work/u11/native/cinnamon/native.py start
python3 work/u11/native/cinnamon/native.py cases
python3 work/u11/native/cinnamon/native.py views
```

Installation preserves the previous synthetic XDG directory only after checking
all accepted operations are terminal. Every installed executable is hashed and
its independent version output matched against the package manifest. Each later
snapshot checks the live resident's full source commit. All normal closes require
both helper disappearance and supervisor `lastExit.status=normal,code=0`.

`step LABEL click X Y [right]`, `step LABEL key ...`, and `step LABEL shot` record
visible QMP input and screenshots for manual acceptance. Inspect each candidate
screenshot before choosing coordinates. The script refuses to overwrite witness
labels and command receipts. Use new labels for corrective attempts; retain failed
attempts and explain their disposition. Restore a locked test session with the
recorded exact `loginctl unlock-session` recipe followed by visible QMP input and
a fresh witness. Never change the lock policy or VNC output.

The `traycase` phase takes the icon/menu coordinates from an inspected candidate
menu and invokes the real tray by QMP. It reuses the unchanged desktop runner's
independent pixels/artifact/paste oracle. Only the runner's one action-trigger
key gesture is replaced with visible menu clicks; this is not a product mock. The
base runner still records configured hotkeys, so the additional tray receipt is
the authoritative actual-trigger record:

```sh
python3 work/u11/native/cinnamon/native.py traycase region --icon-x X --icon-y Y --menu-x X --row-y Y
```

Repeat for window, desktop and clipboard. Before each action, the helper must be
absent. The 12-case ordinary matrix independently exercises the actual shortcuts.
Open all four views from the visible tray as a separate GUI-trigger check. The
`views` phase then performs two production-CLI cycles and visible normal closes;
its first cycle verifies each focused helper view rejects active-window capture.

Draft/conflict, native folder picker, native fonts and modal ownership are separate
visible checks. Use only synthetic non-secret text, retain accepted configuration
revisions, and distinguish ordinary window Close (draft retained) from Cancel
(draft discarded). Check real clipboard paste after final helper exit. Never
write credential input or log private settings.

After the other functional cases, `continuity.py` makes one bounded real local
clipboard-file attempt with a 512 MiB streamed synthetic file. It verifies actual
configured limits and free space, observes the accepted operation while the helper
closes by visible Alt+F4, then checks archive hashes and resident identity. It
claims continuity only if the operation was still active after helper exit. A
fast operation is recorded as inconclusive; there is no blind retry, fake provider,
injected delay or S3 claim. The fixture and archive payload stay in the disposable
VM and are not part of exported evidence. File creation/hash use bounded blocks.

The final receipt must distinguish demonstrated gates from remaining limitations.
Every attempt carries operation/test IDs and screenshots. Export only explicitly
selected sanitized receipts, manifests, XML and witnesses; never the VM run folder,
SSH private keys or whole synthetic payload directories. Do not begin memory
sampling until the root confirms the cross-desktop functional wave is quiet.
