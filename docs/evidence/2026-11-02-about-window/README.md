# About window redesign (#1155)

Pictures of the About window with put.gd's own title bar, the capture scene and
the window fitted to its content.

Every "after" picture shows build `26.1102.3802-dev+g822dea2cddcd`, commit
`822dea2cddcd6a6330551d881cf2018ceb30e2bf`, clean source tree, built from that
commit on the host and run with a private profile on its physical desktop.

| Picture | Shows | Desktop |
| --- | --- | --- |
| [about-before-after.png](about-before-after.png) | The window before and after | Before: Patrick's screenshot of `26.1102.3797`. After: Mint 22.3 Cinnamon X11 (m74) |
| [about-build-details-cinnamon.png](about-build-details-cinnamon.png) | Build details expanded | Mint 22.3 Cinnamon X11 (m74) |
| [about-plasma-wayland.png](about-plasma-wayland.png) | The window on Wayland | Kubuntu 26.04 Plasma Wayland (k72) |

Red boxes and labels are annotations added afterwards.

The behaviour checks in #1155 used real input on a scratch build of the same About
code: expand, collapse, drag and close on Cinnamon X11, Plasma Wayland and GNOME
Wayland (f44).
