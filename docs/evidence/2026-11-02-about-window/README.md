# About window redesign (#1155)

Pictures of the About window with put.gd's own title bar, the capture scene, no
outline, and the window fitted to its content.

Every "after" picture shows build `26.1102.3803-dev+gf820f1679065`, commit
`f820f1679065b39d36a9f125065f0e0fe7132edc`, clean source tree, built from that
commit on the host and run with a private profile on its physical desktop.

| Picture | Shows | Desktop |
| --- | --- | --- |
| [about-before-after.png](about-before-after.png) | The window before and after | Before: Patrick's screenshot of `26.1102.3797`. After: Mint 22.3 Cinnamon X11 (m74) |
| [about-build-details-cinnamon.png](about-build-details-cinnamon.png) | Build details expanded | Mint 22.3 Cinnamon X11 (m74) |
| [about-plasma-wayland.png](about-plasma-wayland.png) | The window on Wayland | Kubuntu 26.04 Plasma Wayland (k72) |

Red boxes and labels are annotations added afterwards.

The drag and close checks in #1155 used real input on a scratch build from before
its browser-feedback fix (`20c4f61a7`), on Cinnamon X11, Plasma Wayland and GNOME
Wayland (f44). The commit above, the first without the window outline, was opened and expanded on
Cinnamon X11 and Plasma Wayland.
