# VM lab: input, editor drivers and evidence

Part of [Disposable graphical lab](../vm-lab.md).

## Physical-desktop input agent

Physical build-server desktops have no QMP. `tools/lab/host_input.py` creates
kernel uinput devices, a keyboard and an absolute pointer modelled on QEMU's
usb-tablet, and accepts the runner's QMP subset (`qmp_capabilities`,
`input-send-event` with qcode keys, left/right/middle buttons and 0–32767
absolute axes) on an owner-only Unix socket. Events pass through libinput into
the compositor or X server like hardware input, so global shortcuts, region
selectors and editor windows receive real input. It refuses every other command,
including `screendump`; capture frames from the host's loopback VNC tile.

Start it with authorized `sudo -n` only to open `/dev/uinput`; it drops to the
desktop user before listening, and no udev rule or persistent permission
changes. Forward the socket over the pinned SSH host key, then point a private
run directory's `qmp.sock` at it so `lab.key`, `lab.pointer` and
`lab.input_events` work unchanged. Run `systemd-run` from the checkout on the
desktop host. systemd starts the unit in `/`, so the script path must be
absolute:

```sh
sudo -n systemd-run --unit=putgd-lab-input --collect \
  /usr/bin/python3 "$PWD/tools/lab/host_input.py" serve \
  --socket /run/user/1000/putgd-lab-input.sock --user patrick
ssh -NT ... -o StreamLocalBindUnlink=yes \
  -L "$run/qmp.sock:/run/user/1000/putgd-lab-input.sock" patrick@HOST
```

Stop the unit after the campaign; its devices disappear with it. On Kubuntu
26.04 Plasma/X11 the devices appear in `xinput`, and a runner pointer move to
(960, 540) on the 1920x1080 desktop read back exactly from `XQueryPointer`.
Wayland sessions still need their own verification before use as evidence.
`tests/desktop/test_host_input.py` covers the accepted protocol and rejections.

## Editor acceptance drivers

These drivers run against a run directory that has passed `lab.py run --keep`
with the region scenario, so the resident and the clipboard fixture are live:

| Driver | Covers |
| --- | --- |
| `tools/lab/edit_acceptance.py RUN` | Graphical Edit & upload: hotkey, region-selected source, redaction drawn and moved with the mouse, keyboard text, undo/redo, the other shape kinds (blur, arrow, highlight, rectangle) and an atomic rejection through `editor.apply`, preview, Ctrl+Enter submit, pixel checks on the saved PNG (redactions black, untouched areas unchanged), an independent paste and screenshots. Crop, save copy and cancel are covered by `mcp_editor_e2e.py` below, not here. It drives the editor at its opening Fit zoom: the scale comes from `editor.view` and `lab.locate_scaled_fixture_paint` finds the scaled image from its quadrant boundaries. On GNOME, restart the guest session after installing a new package so gnome-shell loads its put.gd extension (an older one lacks the edit shortcut). |
| `tools/lab/edit_memory.py RUN [SAMPLER]` | Whole-family RSS+swap (`family_sampler.py` in the guest) for generated PNG files opened with `editor.open`: 1080p, 4K and 24 MP edit-and-submit cycles, 100 edits with undo/redo churn on 4K, five open/cancel cycles on 4K, and a 6000×4001 file that must be refused. It does not run the desktop or region capture sources. Gate: incremental peak at most 3R + 96 MiB per case, and the resident returns within 8 MiB of baseline once the fixture takes the clipboard. |
| `tools/lab/mcp_editor_e2e.py EVIDENCE` | Official MCP client through `putgd-mcp` and the target's production `putgd-cli` (a small wrapper running it over pinned SSH): all editor tools, preview image, save copy, submit and cancel. |
| `tools/lab/guest_reboot.py RUN` | Reboot through the session manager when a session-loaded integration such as the GNOME extension must reload; verifies the boot time changed. |

On the `fedora-gnome-wayland` golden the session manager ignores that reboot
request and the `lab` account may run only `dnf` through `sudo`. After
installing the RPM in a run's overlay, `sync` in the guest and reset the
disposable VM through its QMP socket (`system_reset`); GDM auto-logs `lab`
in and the new Shell loads `putgd@put.gd` (seen ACTIVE on 2026-10-18), so the
capture scenarios bind their shortcuts without a new golden. Never reset a
golden; only a run's own overlay.

`tools/lab/physical.py HOSTCFG.json install DEB|RPM|SCENARIO|script DRIVER` runs the
same runner and drivers on a physical desktop. `SCENARIO` is any `lab.py run
--scenario` name, such as `region` or `window`. It swaps the runner's shell,
copy and screenshot helpers for pinned SSH, `scp` and the host's loopback VNC
tile (`tools/lab/rfb.py`, security type None only), reads input from the
forwarded agent above, and labels results `graphical-physical`. Keep the host
config and its run directory under `work/` or the scratchpad; they name only
key and known-hosts paths, never secrets. A package built without
`PUTGD_LAB_FIXTURE` has no `putgd-desktop-fixture`; set the host config's
optional `fixture_bin` to the host directory holding a lab-built one.

Window capture includes the window frame where the desktop provides one. On
Plasma/Wayland, KWin draws that frame server-side and native Qt reports zero
frame margins, so the runner asks KWin's `queryWindowInfo` picker for the
frame whenever the fixture reports a KDE Wayland session. It decides from that
observed session, not the profile name, so physical and VM profiles get the
same check.

On Fedora, `install` takes the RPM: it removes any installed `putgd` with dnf
first, because dnf will not reinstall or downgrade a same-version development
build. Verification extracts the RPM with `rpm2cpio`. A first install of the
GNOME extension loads only after a fresh login. Physical runs also need:

- Close put.gd windows left by earlier steps with `ui.quit`. On GNOME, a leftover
  Settings window covered the fixture.
- Keep the workstation wall's tile for the host view-only. With input enabled,
  moving the mouse over it injects pointer events into the run.

## Evidence and gates

Each named run emits `results.json` and JUnit `results.xml`, independent paste JSON,
actual saved artifacts/PPM conversion, public GET bytes when configured, expected/actual
pixel and hash comparisons, QMP witness frames, package hash, versions, real fixture
session/platform/display layout and requested/actual acceleration. `state.json` contains
the base disk hash and run ID. Run directories include ephemeral private keys and must
never be uploaded whole: export only the explicit evidence files in `common.yaml`,
`artifact-*`, `public-*`, `history-paste.json`, package/version manifests and results.

Profiles are available for Cinnamon/X11, Ubuntu GNOME/Wayland, Kubuntu
Plasma/Wayland and the additional Fedora GNOME/Wayland smoke environment.
Mint, Ubuntu, Fedora and Kubuntu base disks have been installed and checked locally;
the full Wayland action matrix remains pending.
Native Wayland profiles require the installed U7/U8 helper, an actual Wayland
session and the native Qt platform; readiness rejects Xwayland. TCG is explicit and labelled functional-only. VM evidence does not certify the physical-machine performance budgets.
Multihead/scaling/rotation/removal, cancellation, shortcut conflicts, archive failures,
copy suppression, full GUI history/config conflict flows, normal GTK/file-manager
interop, physical Cinnamon and the complete release matrix remain acceptance gates.
A host protocol pass, ISO checksum, live desktop boot or installed package alone does
not satisfy them.
