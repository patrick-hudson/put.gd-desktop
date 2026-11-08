# Desktop adapters

One page per native desktop integration.

## Guarantees

- Real desktop support requires graphical acceptance evidence for each declared compositor. Offscreen tests prove widget and core behavior only.
- GNOME: the adapter never calls the restricted `org.gnome.Shell.Screenshot` service as a spoofed caller, enables unsafe mode or substitutes a portal picker.
- Plasma Wayland: the adapter never substitutes a screen picker for region selection.
- Budgie 10.10 (labwc): capture uses ext-image-copy-capture on put.gd's own connection; the adapter never uses a portal, a picker, `grim` or `slurp`, and is chosen by the Budgie desktop token, never for bare labwc.
- Cinnamon: the adapter requires the Qt `xcb` backend, RandR, XFixes and a resident in the same X11 user session. Wayland is a separate integration and is not selected by this adapter.

## Documents

- [Budgie](budgie.md): Budgie 10.10 on labwc, the labwc shortcut bridge, window capture and the Budgie 11 status.
- [Cinnamon / X11](cinnamon.md): the X11 adapter, window capture and clipboard behavior.
- [GNOME 50 / native Wayland](gnome.md): the resident's session-bus bridge and the `putgd@put.gd` Shell extension.
- [Plasma Wayland](kde.md): KWin ScreenShot2, KGlobalAccel v2 and data-control.
