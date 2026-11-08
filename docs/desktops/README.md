# Desktop adapters

One page per native desktop integration.

## Guarantees

- Real desktop support requires graphical acceptance evidence for each declared compositor. Offscreen tests prove widget and core behavior only.
- GNOME: the adapter never calls the restricted `org.gnome.Shell.Screenshot` service as a spoofed caller, enables unsafe mode or substitutes a portal picker.
- Plasma Wayland: the adapter never substitutes a screen picker for region selection.
- Budgie 10.10 (labwc): screenshots and recordings use ext-image-copy-capture on put.gd's own connections, never a ScreenCast portal, a picker, `grim` or `slurp`; shortcuts may use the GlobalShortcuts portal where the desktop offers it. The adapter is chosen by the Budgie desktop token, never for bare labwc.
- Hyprland: the adapter never uses a portal picker, grim or slurp, never calls `hl.unbind` or a bind handle's `:remove()`, and never writes the user's Hyprland configuration files; accepted on Omarchy 4.5 only.
- Cinnamon: the adapter requires the Qt `xcb` backend, RandR, XFixes and a resident in the same X11 user session. Wayland is a separate integration and is not selected by this adapter.

## Documents

- [Budgie](budgie.md): Budgie 10.10 on labwc, the labwc shortcut bridge, window capture and the Budgie 11 status.
- [Cinnamon / X11](cinnamon.md): the X11 adapter, window capture and clipboard behavior.
- [GNOME 50 / native Wayland](gnome.md): the resident's session-bus bridge and the `putgd@put.gd` Shell extension.
- [Plasma Wayland](kde.md): KWin ScreenShot2, KGlobalAccel v2 and data-control.
- [Hyprland (Omarchy)](hyprland.md): ext-image-copy-capture, Hyprland IPC and runtime Lua binds, capture permissions.
