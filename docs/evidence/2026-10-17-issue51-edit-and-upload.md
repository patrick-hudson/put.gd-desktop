# Edit & upload — 2026-10-17

[Issue #51](https://github.com/patrick-hudson/put.gd-desktop/issues/51) adds the
on-demand image editor ([design and contract](../image-editor.md)). This receipt
covers development candidate `9dd61d3108e0` (product source as of `613f103`; the
later commit only adds lab drivers). It is Linux-only development acceptance.
No Windows behavior, release-package or full physical-matrix claim is made.

## Automated

The full Dev suite passed 63/63 on the Mint 22.3 workstation (Qt 6.4.2) and
63/63 on native Kubuntu 26.04.1 (Qt 6.10.2) and 64/64 on native Debian 13
(Qt 6.8.2, with LayerShellQt). Fedora 44 (`909f8ee`, an earlier candidate)
passed 63/64;
the one failure, `cli_integration`, fails the same way on builds without the
editor.

## Graphical and memory gates (KVM, 1280x800)

Cinnamon ran first. Each desktop used the region scenario's baseline, then
`tools/lab/edit_acceptance.py` and `tools/lab/edit_memory.py`
([drivers](../vm-lab.md#editor-acceptance-drivers)):

| Desktop | Package | Graphical | Memory | Resident idle |
| --- | --- | --- | --- | --- |
| Mint 22.3 Cinnamon / X11, Qt 6.4.2 | `0aa7b948…d0e3` | 21/21 | 5/5 cases within target | 28.6 MiB |
| Ubuntu 26.04.1 GNOME / Wayland, Qt 6.10.2 | `90d9a08b…30ad` | 21/21 | 5/5 | 61.1 MiB |
| Kubuntu 26.04.1 Plasma / Wayland, Qt 6.10.2 | `90d9a08b…30ad` | 21/21 | 5/5 | 32.1 MiB |

The 21 graphical checks are:

- **Opening the editor:** the shortcut opens the editor on a mouse-dragged region selection, and the operation stays `editing` with no items.
- **Hand editing:** a mouse-drawn redaction has the expected geometry; keyboard text entry, undo and redo work; plain Enter does not submit.
- **Agent commands:** agent commands apply atomically, an invalid batch is rejected whole, and a preview is returned.
- **Uploaded result:** the artifact is a PNG of the right size. Every redacted pixel is black, and no source pixel survives inside a mask. Untouched pixels match the fixture. An independent client's paste matches the artifact.

Memory figures are the whole-family incremental peak (RSS+swap) against the
3R + 96 MiB target. Here R is the image's 32-bit raster size.

| Case | Target | Cinnamon | GNOME | KDE |
| --- | --- | --- | --- | --- |
| 1920x1080 file, edit, submit | 119.7 MiB | 99.1 | 101.6 | 110.2 |
| 3840x2160 file, edit, submit | 190.9 MiB | 169.3 | 170.7 | 171.2 |
| 24 MP file, edit, submit | 370.7 MiB | 317.1 | 316.8 | 321.1 |
| 3840x2160, 100 edits plus undo/redo | 190.9 MiB | 92.8 | 94.5 | 102.0 |
| 3840x2160, open/cancel five times | 190.9 MiB | 92.1 | 97.3 | 101.1 |

Opening a 24 MP file took 3.3–3.8 s. After every case the resident returned
within 0.2 MiB of its baseline once the independent fixture took clipboard
ownership. While an X11 resident still owns the clipboard, it holds the last
edited PNG (up to 48 MiB for 24 MP on Cinnamon). That memory is charged in
the figures above, not excluded. An over-admission 6000x4001 file failed with
`edit_input_too_large` and stored nothing.

![Cinnamon editor after mouse, keyboard and agent edits](issue51-edit-and-upload/cinnamon-agent-edits.png)
![GNOME Wayland editor after agent edits](issue51-edit-and-upload/gnome-vm-agent-edits.png)
![KDE Wayland editor on a region selection](issue51-edit-and-upload/kde-vm-editor-open.png)

The persistent Cinnamon banner in the first screenshot came from the previous
candidate's over-admission check. Before `613f103`, failure toasts showed the
clipboard status instead of the error. A D-Bus capture on this candidate
shows `This image is too large to edit; use a smaller copy`. Cinnamon keeps
the older warning banner on screen until it is dismissed.

## MCP

`tools/lab/mcp_editor_e2e.py` drives the official MCP client through `putgd-mcp`
and the target's production `putgd-cli`. It covers:

- tool discovery
- single-editor raising
- every shape kind, crop, update/remove and undo/redo
- atomic rejection and the MCP image preview
- save-copy, including its no-overwrite refusal
- submit, with the artifact hash equal to the saved copy
- cancel, which stores nothing

Results on `9dd61d3`: 13/13 on each of the following.

- Cinnamon (VM)
- The physical Kubuntu 26.04.1 Plasma 6.6.6 X11 desktop at 1920x1080
- The physical Debian 13 Plasma 6.3.6 Wayland desktop (KWin 6.3.6, Qt 6.8.2)
  at 1920x1080

The physical runs used the uinput agent and VNC frames through
`tools/lab/physical.py`. On Debian Wayland the agent's absolute pointer input
was first checked by hovering a panel icon and seeing its tooltip.

On physical Debian, region-sourced editing opened correctly, but its graphical
check stopped at `editorImageLocatedUniquely`. The captured pixels contain the
selector's frame and dimmed edge. A plain region capture in the same session
shows the same frame, so this is the selection-outline bug of
[issue #54](https://github.com/patrick-hudson/put.gd-desktop/issues/54) on a
physical Plasma/Wayland compositor, not an editor defect. Result copying there
reports `clipboard_unavailable`: KWin 6.3 does not offer `ext-data-control-v1`,
and the KDE adapter targets Plasma 6.6 ([KDE notes](../desktops/kde.md)).

After the physical run the idle resident showed 118 MiB RSS but 30 MiB PSS
and 14 MiB private dirty. About 94 MiB is shared clean library pages: Plasma's
platform theme loads Qt Quick and the Mesa/LLVM GPU driver into the resident.
2026-10-08 evidence shows the same mapping before the editor existed, and no
`putgd-ui` helper was left running.

## Not covered

- The physical Fedora 44 GNOME desktop, and physical-host memory gates.
- Region-sourced editing on physical KDE (X11 and Debian Wayland). It is
  blocked by the outline bug in issue #54; file and desktop sources work there.
- Windows, which is portable code only, and live remote uploads. All runs used
  local output.

Private evidence: `work/verification/2026-10-17-editor-{cinnamon,gnome-vm,kde-vm}/gate-9dd61d3/`
and `.../mcp-9dd61d3/`, plus `work/verification/2026-10-17-editor-{kubuntu,debian}-physical/`.
