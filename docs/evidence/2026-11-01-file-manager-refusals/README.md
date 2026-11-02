# File manager refusals (#1138)

Guest frames from the CI packages of #1138 at `922be7369`
(`26.1102.3774~dev+ge7d882e8c680`). Boxes mark the change.

| Picture | Guest | What it shows |
| --- | --- | --- |
| `plasma5-appimage-refused.png` | Plasma 5.27.12, X11 | AppImage started from Dolphin: notification through `busctl` |
| `plasma5-appimage-no-server.png` | same, plasmashell stopped | the same refusal through `kdialog` |
| `upgrade-over-3384.png` | same, DEB upgraded over a running `26.1031.3384` resident | the "just updated" text from a real right-click |
| `no-server-dialog.png` | same, plasmashell stopped | a right-click refusal through `zenity` |
| `gio-unavailable-recent.png` | Ubuntu 24.04 GNOME X11, `gio` hidden | Nautilus Recent, AppImage |
| `gvfsd-fuse-stopped.png` | same, `gvfsd-fuse` killed with an SMB mount up | Nautilus on the share, AppImage |
