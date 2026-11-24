# Clipboard after quitting

Wlroots compositors (labwc, Hyprland) drop a selection when its owner
disconnects. Stock Budgie has no clipboard manager. Omarchy's clipboard history
records copies but doesn't restore the selection. So without this, a link put.gd
copied would stop pasting as soon as put.gd quit.

On Budgie and Hyprland, the source adapter's destructor asks
`DataControlClipboard::keepSelectionAfterExit`. If the selection is still the
result put.gd copied (its offer carries that copy's private marker type), after
a bounded wait for a copy still in flight, the resident starts `putgd-clipboard-keeper`, detached, with the exact types and
bytes. The resident then exits as usual. Plasma, GNOME, Cinnamon and X11 never
reach this code.

## The keeper

`src/platform/wayland/clipboard_keeper_main.c`, C with only libc and
`libwayland-client`.

1. It reads the content from an anonymous memory file on standard input
   (`ClipboardKeeperFormat.h`). It writes no files, and the process is not
   dumpable, so the content can't reach a core file. It asks the kernel to keep
   the content out of swap; over the user's memory-lock limit it can still swap,
   like any process memory. Results over 64 MiB aren't kept, and quitting then
   loses them.
2. It connects with its own data-control device and looks at the selection:
   - Still put.gd's copy: it waits for the resident to let go (10 s at most).
   - Empty: it gives a clipboard manager 300 ms to restore it.
   - Anything else: another app owns the clipboard, and it exits.
3. If nobody restored the selection, it offers the same types and bytes itself.
4. It exits as soon as the compositor cancels its source because anything else
   took the clipboard. A paste already under way finishes first; one that stalls
   for 5 s is dropped. It also exits when the compositor goes away.

A clipboard manager that restores the selection first (clipman, `wl-clip-persist`)
leaves the keeper nothing to do. A slower one still wins: taking the selection
cancels the keeper.

## AppImage

An AppImage's mount ends with the resident. The keeper therefore runs from a copy
in memory (`fexecve` of a memfd) with `host::environment()`, so it loads the host's
libwayland-client and libc and holds nothing from the mount. It inherits no
descriptors above standard error, so the mount is released at once.

## Limits

`kill -9` of the resident and crashes aren't covered: the handover happens on a
normal exit. The content is held until something else takes the clipboard or the
session ends.

The keeper names itself `putgd-clipboard` (see `/proc/<pid>/comm`).
