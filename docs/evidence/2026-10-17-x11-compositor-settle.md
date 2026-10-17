# X11 region capture waits for compositor repaints — 2026-10-17

Issue [#54](https://github.com/patrick-hudson/put.gd-desktop/issues/54): on KDE Plasma
6.6 / X11, region captures contained the selector outline. The tested candidate is
commit `09e1ab7a5457a023d6bfba43666fae86bbbf2c81`, tree
`4d8e360d9200c175a8a92d34747b0ed5a1939fbf`; the baseline is main `1237fbc`.
This record changes no runtime source.

## Cause and measurement

The selector's unmap is processed by the X server, but a compositing manager
presents it later. On Kubuntu 26.04.1 (KWin 6.6.6/X11, Intel HD 530, 60 Hz) a probe
showed that KWin fades a closed ARGB override-redirect surface out over about 500 ms,
with the outline fully opaque for about 23 ms. With `_KDE_NET_WM_SKIP_CLOSE_ANIMATION`,
the outline left the root image 7–51 ms after the unmap, sometimes after
two silent frames. Root Damage reports every composited repaint but can lead or trail
visibility by about one frame, and no client-visible signal identifies the frame that
removes an unmanaged window. Capture therefore waits until root Damage over the selection
has been quiet for 100 ms, bounded at one second. Cinnamon keeps its compositor
screenshot path.

## Results

| Check | Result |
| --- | --- |
| Kubuntu 26.04 build, Qt 6.10.2 | 59/59 tests |
| Mint 22.3 build, Qt 6.4.2 | 59/59 after serial rerun of 4 load-timed S3/storage tests; `x11_isolated` 3/3 |
| New regression tests against main's adapter | Fail (stale outline captured; cancellation timing) |
| Mint 22.3 Cinnamon/X11 VM, lab `region` | Baseline and candidate pass, byte-identical artifact `4512ef7e…` |
| Cinnamon resident memory, 3 alternating pairs | No change beyond the 16 KiB RSS / 8 KiB private `libxcb-damage` mapping |
| Kubuntu 26.04 KDE/X11 physical desktop, lab `region` | 4/4 pass, artifact `4512ef7e…`, only fixture colours |

The Cinnamon VM ran on the Fedora 44 builder from a verified copy of the workstation
`cinnamon-x11.qcow2` golden (sha256 `4a21566d…`), because the Debian builder was
already running three VMs. Physical KDE runs used the uinput host input agent
and the k72 VNC tile. Failed harness attempts are retained: a missing `dpkg-deb`,
an apt downgrade refusal, an editor-branch config in the shared lab XDG directory,
and a cursor left over the fixture.
Private evidence, identities and visuals:
`work/verification/2026-10-17-x11-settle-09e1ab7/`.

The Plasma/Wayland selector shows a similar frame on a physical KWin 6.3 desktop;
that path is separate and is not covered by this change.
