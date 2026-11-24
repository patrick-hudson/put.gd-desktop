# VM lab: multi-head and visible guests

Part of [Disposable graphical lab](../vm-lab.md).

## Multi-head guests

A guest can have up to four virtual monitors. Set `display_heads` (1-4) in a profile, or pass
`--heads N` to `launch`. `--layout` (or a profile `head_layout` list) gives each head's size and
place in head order, as `WxH+X+Y` entries that may use negative origins and gaps:

```sh
python3 tools/lab/lab.py launch --profile cinnamon-x11 --golden GOLDEN --run "$lab_run" \
  --bootstrap-key KEY --known-hosts KNOWN_HOSTS --vnc-display 60 \
  --layout 1920x1080+0+0,1280x1024-1280+0
python3 tools/lab/lab.py layout --run "$lab_run"
```

- **QEMU:** the profile's `virtio-vga` (or `virtio-gpu-pci`) device gets `max_outputs=N`
  and id `video0`. Each head has its own loopback VNC listener on consecutive displays:
  `--vnc-display 60` with three heads listens on 5960, 5961 and 5962. A single-head launch
  keeps its exact previous command line.
- **Connected heads:** virtio-gpu reports only head 0 as connected until a display client
  sends size information for a head. Right after QEMU starts, `launch` sends each head an RFB
  SetDesktopSize request (the layout's size, or 1280x800), so the guest boots with N connected
  heads and that size as each head's preferred mode. Without it a second output stays off, as
  #108 found, and `xrandr --setmonitor` splits of one output are not a substitute.
- **Run metadata:** `state.json` and the `launch` result list every head's port under
  `heads`, plus `head_layout` and the QEMU answer to each size request.
- **Layout and verification:** `layout --run RUN [--layout SPEC]` re-sends the head sizes,
  arranges the outputs in the native session and verifies them. It uses `xrandr` on X11,
  `kscreen-doctor` on Plasma and Mutter's DisplayConfig D-Bus API on GNOME, with head N on
  connector `Virtual-(N+1)` and head 0 as the primary output. It passes only when the guest
  reports exactly N enabled outputs in the requested arrangement, each X11 RandR monitor is
  its own output, and each head's QMP `screendump` has the head's size. Results go to
  `layout.json` in the run directory.
- **Origins:** X11, Mutter and KWin all place the desktop's top-left corner at 0,0, so a
  negative origin becomes the same arrangement shifted, with head 0 still primary (for
  example a monitor left of the primary). Mutter also rejects gaps between monitors; X11
  and KWin keep them.
- **Input per head:** each head has its own `usb-tablet` bound to that head's console, with
  serial `putgd-headN`, so a VNC client on head N sends head-relative coordinates. On X11,
  `layout` maps each tablet to its output with `xinput map-to-output`. `input --run RUN
  --head N 'JSON_EVENTS'` sends QMP input through head N's tablet. QMP input without
  `--head` still goes to the unbound tablet, which spans the whole desktop.
  Wayland guests can't map them yet: KWin and Mutter scale every QEMU tablet over the whole
  desktop, KWin ignores `outputName` and `calibrationMatrix` for them (and stores one setting
  for all three identically named tablets), and a udev calibration rule needs guest root.
  There, pointer input on a multi-head guest lands at the head-relative position scaled to
  the whole desktop; keyboard input is unaffected.
- **Screenshots:** `screenshot --run RUN --head N` saves head N's frame.

## Visible guest preflight

[Code PR #584](https://github.com/patrick-hudson/put.gd-desktop/pull/584) adds the
common disposable-guest visibility preflight. `lab.py launch` applies it after
authenticated SSH and before returning; `start` also applies it. Graphical
scenarios use check-only preflights both before and after the workload, so policy
drift after setup is refused rather than silently repaired. Custom
acceptance drivers must use the same boundary checks:

```sh
python3 tools/lab/lab.py preflight --run work/lab-runs/RUN --apply --stage before-campaign
python3 tools/lab/lab.py preflight --run work/lab-runs/RUN --stage before-idle
# Run the unchanged measurement driver, with the visible wall maintained.
python3 tools/lab/lab.py preflight --run work/lab-runs/RUN --stage after-idle
```

The host authenticates the owned running QEMU process, disposable overlay and
forwarded SSH endpoint before sending the helper. The guest rejects non-QEMU/KVM
machines and derives configuration homes from the actual desktop process. Do not
read desktop policy through `guest_session`'s product-specific XDG homes.

GNOME and Cinnamon disable idle activation, locking, dimming and inactive sleep
on both power supplies. Plasma verifies lock and all AC/Battery/LowBattery
power profiles and reloads the live services. A user service holds a verified
logind idle/sleep inhibitor; initial acquisition may take up to five seconds.
Initial GNOME/Plasma service readiness is bounded to 30 seconds; Cinnamon
ScreenSaver is demand-activated and must return inactive from `GetActive`.
Initial complete-output readiness is bounded to 15 seconds per head, retaining
every blank attempt. Check-only boundaries neither repair drift nor wait for
missing services, inhibitor or visible frames.
X11 additionally disables and reads back screen saving/DPMS. Native session type,
active seat, unlocked state, inactive screensaver, configured outputs, complete
QMP frames and VNC extents must agree. A uniform RGB color is not a visible
desktop witness, even when its component channel values differ. These checks do not replace a geometry
campaign's independent all-head fresh-motion oracle or human visual review.

New provisioning seeds for generic, Aurora and neon guests must contain
`guest_idle.py` beside `bootstrap.sh`. Bootstrap installs the helper and a desktop
autostart entry for future disposable logins. The normal launch/start checks still
verify the effective live policy before acceptance. Existing immutable goldens
are never edited to retrofit it: apply the helper in the fresh owned overlay.
For an older golden, install the companion helper with `--install-autostart
--user lab` inside its fresh owned overlay before any planned later login;
`launch` applies live policy but does not itself install that autostart file.
An existing unrelated helper/unit is a collision and is preserved; inspect it
instead of overwriting it. The user inhibitor ends when the disposable guest is
retired through normal guarded cleanup.

Any failed policy, active screensaver, locked session, missing output or blank
witness blocks acceptance. Once the run directory is verified as owned,
non-symlinked and not writable by other users, early ownership/readiness/profile
failures also retain `guest-preflight-*.json`. Unsafe receipt directories are
refused without writing or contacting a guest. Available visuals are preserved. Restore a locked or blanked guest through its visible console before
retrying. Preserve the failed attempt. Never apply these settings to a physical
user desktop or treat an SSH-only check as complete graphical evidence.

A failed launch attempts a bounded guest `sync` before stopping its owned QEMU
process, retaining `failure-shutdown.json`. The generated run public-key append
is newline-separated and synced before preflight, so a failed initial launch
does not rely on guest page-cache persistence. This does not authorize stopping
an unrelated guest or altering existing keys.
