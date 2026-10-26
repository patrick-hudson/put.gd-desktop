# Headless native GPU and VM geometry lab

The executable pilot tools are in [#583](https://github.com/patrick-hudson/put.gd-desktop/pull/583).
They exercise native Intel DRM outputs and separate VM scanouts. The
[2026-10-25 evidence](evidence/headless-multi-output-20261025/README.md) records
what actually ran, including failures and restoration. The KDE selector fix is
separate in [#582](https://github.com/patrick-hudson/put.gd-desktop/pull/582).
Neither PR accepts the complete [#380](https://github.com/patrick-hudson/put.gd-desktop/issues/380)
matrix or changes [#381](https://github.com/patrick-hudson/put.gd-desktop/issues/381).

## Admission and preparation

Follow [remote builders](remote-builders.md), the coordinator's per-host schedule,
and the outgoing verified-clean handoff. A free desktop lock does not release a
host's heavy or quiet reservation. Inspect existing applications before GUI
work; keep the original display, SSH access and wall service. Do not change a
physical desktop's idle, lock or power policies.

Acquire and heartbeat the physical desktop lease from the workstation:

```sh
python3 tools/lab/desktop_lock.py status HOST --check
python3 tools/lab/desktop_lock.py acquire HOST --owner OWNER --session SESSION \
  --purpose 'Bounded native multi-output trial and exact restoration' --minutes 20
python3 tools/lab/desktop_lock.py heartbeat HOST --session SESSION
```

Run the remaining native commands **on that builder**, in a short private
directory. Use new output names for every attempt. Before an isolated app/input
run, resolve and check every socket, including `runtime/putgd/session.sock`:

```sh
python3 tools/diagnostics/check_socket_paths.py --socket \
  /home/patrick/work/mh38/n1/qmp.sock \
  /home/patrick/work/mh38/n1/runtime/putgd/session.sock
```

Normal distro tools are sufficient. Debian/Kubuntu/Mint provided `modetest` in
`libdrm-tests` and `drm_info` in `drm-info`; Fedora 44 provided them in
`drm-utils` and `drm_info`. Record the original package inventory and remove only
what the trial added. No replacement Qt toolchain, Snap or virtual-output driver
is needed for this native path.

Save the compositor's current output inventory, persistent monitor files and
application-window inventory before hotplug. Keep file bytes as well as hashes.
DRM connector names and compositor names can differ (`HDMI-A-2` versus
`HDMI-2`); inspect both. Select compositor modes from the live mode IDs, rather
than assuming that a rounded refresh string is a valid ID.

## Guarded Intel trial

`tools/lab/headless_output.py` supports the inspected i915/debugfs path. It saves
EDID overrides and force states for every connector, checks encoder aliases,
and arms a user-systemd rollback timer **before** its first write. Its restorer
is copied into the run so a later checkout edit cannot change rollback behavior.
The deadline must precede lease expiry. It does not take DRM master or edit boot
configuration.

The revised timer explicitly sets `AccuracySec=1s` and `RandomizedDelaySec=0`.
Admission reserves that accuracy plus 60 seconds for restoration and repeats its
deadline check after setup. This bounds configured timer coalescing, not host
scheduler delays; heartbeat and verified restoration still control handoff.

```sh
python3 tools/lab/headless_output.py inspect --card card0 --output before1
python3 tools/lab/headless_output.py edid --output qhd60.edid --refresh 60
python3 tools/lab/headless_output.py enable --card card0 \
  --connector HDMI-A-2 --keep HDMI-A-1 --edid qhd60.edid \
  --run t1 --session SESSION --seconds 300
```

Those connector/card names are examples. The observed Intel hosts exposed
DP/HDMI aliases: DP-1 shared encoder 98 with HDMI-A-1, while HDMI-A-2 used encoder
114. The helper refuses a target sharing an encoder with an active output.
Disconnected connector names alone establish no independent physical ports.

The generated 256-byte EDID includes QHD60, a 1080p fallback and an HDMI VSDB.
The earlier recipe without the VSDB was rejected at QHD60 on these machines;
QHD30 and the revised QHD60 recipe worked. Preserve rejected recipes/attempts.
After forcing, verify distinct active CRTCs/pipes and the native compositor's
actual enabled outputs. A connected DRM object alone is insufficient.

## Keep every active head visible

Use [the wall workflow](remote-vm-displays.md). Retain the original VNC server;
renew only a pilot-owned stream after topology changes. A combined stream must
match the **actual** logical bounding rectangle. `tools/lab/wall_probe.py`
requires known synthetic coverage and independently changing pixels on every
head: at least 99% coverage and at least 1% changed pixels (minimum two). A stale
size or one mostly frozen head blocks admission. The native adapter also checks
the actual saved final frame and deletes it if that gate fails.

The native adapter also requires at least two fullscreen heads before launching
put.gd. Its `--vnc-port` currently requires one verified combined stream. GNOME
can instead expose individual monitors: the Fedora pilot retained the original
head-0 stream and selected head 1 through a second native portal session, with
both streams visible on separate wall tiles. That proves native GPU/display
feasibility; it does not exercise the combined-stream app adapter.

Campaign attempts and fixture generations use unique IDs, not local basenames.
Readiness is bound to the launched PID/attempt and a still-live process identity;
actual drawing, compositor fullscreen acknowledgement, dimensions and placement
are required within a deadline. Native fixture/capture artifacts stay inside the
owned run tree, and permission entries follow the compositor's `XDG_DATA_HOME`.

The RFB reader must retain pixels outside partial update rectangles. Fix
`421214b4` adds this behavior and resize invalidation, with wire-level regression
tests. Preserve the tool hash/ancestry: earlier readers could manufacture black
areas even when QMP and the real wall contained complete live frames. This does
not retroactively turn a failed run into a pass.

## Capture and recording campaigns

`tools/lab/multi_output.py` drives a prepared guest; `multi_output_native.py`
adapts it to a leased physical desktop. The fixtures use native GTK, distinct
output palettes and independent clipboard paste. The driver invokes the
production CLI, records operation IDs and package/running-binary identities,
and saves action/failure frames. Success notifications are disabled only in the
isolated test configuration so one capture's toast cannot contaminate the next.

```sh
python3 tools/lab/multi_output_native.py --session SESSION \
  --run /home/patrick/work/mh38/n1 --appimage EXACT_APPIMAGE \
  --sha256 EXACT_SHA256 --vnc-port COMBINED_PORT \
  --case inside-0 --case inside-1 --case span --case desktop

python3 tools/lab/multi_output.py RUN --output NEW_CASE_DIRECTORY \
  --case inside-0 --case inside-1 --case span --case window-0 \
  --case window-1 --case desktop --case escape --case right-click
```

`--native-package` verifies an **already installed** exact DEB/RPM against its
archive; it does not install one. An AppImage running on a native desktop is
still AppImage evidence, not a native-package media row.

`record-region` compares private MP4 static/moving controls and recording-bar
placement. The revised helper additionally uses distro GStreamer Python
introspection/decoders to decode eight half-second RGB samples and checks exported
dimensions, head-specific spatial checker colors and the known moving-bar phase.
Missing decoder prerequisites fail the case. Lossy-video RGB tolerance does not
change the exact still-image oracle. Scaled/spanning video references currently
fail closed. Motion repeats across the whole fixture so a narrow region cannot
miss a single sweeping bar. A new static fixture is restored even on a recording
error, so later still-image cases remain meaningful. It saves copies and never
invokes Upload. Historical motion-only results are qualified in the evidence
review audit; they are not new-helper live passes.
`output-change` accepts a reviewed native-session command file, retains its
bytes/result and checks cancellation, no artifacts and the clipboard sentinel.
Restore and refresh inventory/wall evidence outside that case before continuing.

Integral-density profiles use exact RGBA comparison, including transparent
gaps. **The current nominal-density fractional oracle is incomplete.** A crop
entirely inside one output preserves that output's original pixel grid: a
2560-pixel image over 1707 logical units can yield 241 pixels for a 160-unit
selection. A fullscreen GTK window's buffer can also be 2561 pixels while the
physical screen is 2560. Compositor resampling changes checkerboard edge colors.
Retain these dimensions and native references; do not lower the threshold or
classify an oracle mismatch as a product failure without resolving the mapping.

## Disposable guests and cleanup

Use immutable remote goldens and fresh overlays through [VM lab](vm-lab.md).
Apply the acceptance lane's [common guest preflight](https://github.com/patrick-hudson/put.gd-desktop/pull/584)
for idle/lock/blanking/suspend
before the first acceptance stage; use check-only mode between stages and fail
on drift. Verify actual ScreenSaver state, logind state, an unlocked active
console and fresh output frames. Session-policy checks must derive the real
desktop homes; the app's isolated `XDG_CONFIG_HOME` is unsuitable. Never apply
guest policy changes to a physical desktop or golden image.

With that helper available, apply once and check without `--apply` at later
stage boundaries:

```sh
python3 tools/lab/lab.py preflight --run RUN --apply --stage before-geometry
python3 tools/lab/lab.py preflight --run RUN --stage after-geometry
```

Keep its helper hash and failed readiness attempts. The pilot's settled GNOME
stage used v3; later helper startup changes do not retroactively replace that
receipt. A preflight pass establishes session readiness, not product correctness.

`multi_output_vm_wall.py RUN --output NEW_DIRECTORY` checks every enabled
per-head VNC listener against native inventory and motion after a topology
change. Expected extent comes from current native mode/rotation plus an independent
QMP scanout frame; the VNC stream cannot choose its own expected rectangle.
Disabled heads are explicit; X11 tablet mappings are refreshed only for enabled
outputs. Keep all workstation tunnels/tiles throughout graphical work.

Restore native compositor settings **and** kernel state, then prove cleanup:

```sh
python3 tools/lab/headless_output.py restore --run t1
# Restore the saved compositor configuration and verify its exact bytes.
# Check native outputs and fresh frames on the retained original wall stream.
python3 tools/lab/desktop_lock.py release HOST --session SESSION --verified-clean
python3 tools/lab/desktop_lock.py status HOST --check
```

The kernel receipt does not certify compositor-file restoration. Stop only owned
residents, fixtures, input units, supplemental VNC servers and tunnels; retire
their sockets/tiles and verify preserved applications/services. Save a clean
completion visual. Export and verify VM evidence before normal reset and guarded
overlay cleanup. A custom failed campaign uses an explicit failed completion
manifest; reset can remove `qemu.pid`, so exclude that transient file from the
post-reset retirement manifest while preserving the original archived receipt.
Never delete a golden or substitute disk deletion for evidence retention.

The successful EDID trials are real GPU/compositor evidence. They do not prove
two physical panels, physical-panel DPI or a cable unplug, and require no
purchase. Requested negative positions were normalized by the tested Linux
compositors; retain requested and actual coordinates separately. Unit coverage
of negative coordinates is not native desktop proof.
