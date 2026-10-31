# File dialogs (#259)

Every open, save and folder dialog goes through one helper,
`putgd::FileDialog` (`src/ui/FileDialog.{h,cpp}`, the `putgd_file_dialog`
library). `putgd-ui` and `putgd-record` both link it; the resident does not.

| Where | Mode | Starts in | Suggested name, filter |
| --- | --- | --- | --- |
| Review recording → **Save copy…** (`putgd-record`) | save | home folder | `recording.<ext>`; the output format |
| Edit image → **Save a copy…** (Ctrl+S, `putgd-ui --edit-image`) | save | Pictures | `put.gd-edit-<time>.png`; PNG image |
| History → **Save as…** | save | the chooser's choice | the file's name |
| **Edit an image file…** | open | Pictures | Images (`*.png *.jpg *.jpeg`) |
| Settings → General → Local copies **Choose folder…** / **Browse…** | folder | the field's folder | — |
| Create diagnostics package → **Choose folder…** | folder | the package folder | — |

## The desktop's chooser through the XDG portal

On Linux the helper asks `org.freedesktop.portal.FileChooser` on the session
bus. It calls `OpenFile`, with `directory` for folders, or `SaveFile`, passing:
- the window title;
- `handle_token`, `modal`;
- `current_folder`, as NUL-terminated bytes;
- `current_name` when saving;
- `filters` and `current_filter`, as `a(sa(us))` globs.

The desktop's own chooser then runs in its portal backend's process:
- `xdg-desktop-portal-gtk` on Cinnamon, since the xapp backend has no FileChooser;
- `xdg-desktop-portal-kde` on Plasma;
- `xdg-desktop-portal-gnome` (Nautilus) on GNOME.

This also works unchanged inside a Flatpak sandbox (#83).

The helper handles the request asynchronously:
- It subscribes to the request's `Response` signal at the handle derived from
  its token before it calls, so an early answer is not missed. A different
  handle returned by an old portal is followed.
- `0` returns the `file://` URI as a local path.
- `1` is a cancel.
- `2` ("ended in some other way") is a cancel when it arrives 250 ms or more
  after the portal accepted the request. xdg-desktop-portal-gtk (Cinnamon) and
  xdg-desktop-portal-gnome 46 send it when the person presses Escape or closes
  the chooser's window. The frontend also sends `2` when a running backend
  fails after accepting; a backend that fails to start makes the call itself
  error instead. An earlier `2` is therefore a failure and opens the fallback
  (#781). The chooser takes about 300 ms to get focus, so a person can't
  normally dismiss it inside the window.
- The cost: a backend that fails 250 ms or more after accepting looks like a
  cancel. The chooser disappears and put.gd does nothing else; asking again
  starts the backend afresh. The `putgd.filedialog` log line records
  `portal response 2 after N ms since accepted (closed by the person, or the
  backend failed late)`, and the portal writes `Backend call failed` to the
  user journal. Both appear in a diagnostics package.
- A non-local URI is a failure that returns nothing.
- Cancelling from put.gd, or the owning window going away, calls `Request.Close`.

**Parent window:** X11 sessions pass `x11:<xid>` of the owning top-level
window, so the chooser stays transient for it. On Wayland the parent is empty.
An xdg-foreign handle needs the window's `wl_surface`, which Qt exposes only
through private API: QtWayland's `winId()` is a counter, not the surface. The
backends still show the chooser above the window, as the GNOME and Plasma
pictures show.

**Modality:** while the chooser is open, the window behaves as it did behind
`QFileDialog::exec`. An application-modal widget that is never mapped blocks
its input, the same mechanism Qt uses for its own native dialogs. Close
requests are refused in the meantime.

**Memory:** no Qt platform theme is involved. The resident and the editor keep
Qt's `generic` theme and `putgd-record` and `putgd-capture` keep
desktop-settings awareness off ([memory-strategy.md](memory-strategy.md)). The
helper adds a D-Bus call to a process that already links QtDBus. With the
chooser open, `putgd-record` grew by at most 0.12 MiB, against 5.4 MiB for
Qt's widget dialog. The resident is unchanged ([evidence](evidence/2026-10-21-file-dialogs-259/README.md)).

## Fallback: Qt's dialog in the Theme tokens

Qt's widget `QFileDialog` opens instead when:
- there is no session bus;
- the portal or its FileChooser interface is missing;
- a folder is asked for and the FileChooser is older than version 3;
- the call errors, or isn't accepted within 8 s;
- the portal answers `Response` 2 within 250 ms of accepting the request, or
  any code other than 0, 1 and 2;
- the portal exits while its chooser is open.

The dialog is styled from the Theme tokens in both schemes:
- palette, labels, the list and detail views, header, path and name fields;
- combo boxes, buttons, scroll bars and menus;
- IconArt toolbar glyphs (`back`, `forward`, `up`, `folder-new`, `view-list`, `view-detail`).

It opens at 820×520 within the screen, with the Size, Type and Date columns
sized from the font. It follows a scheme switch while open.

On Linux it is always Qt's widget dialog (`DontUseNativeDialog`, #863). The
views helper keeps the desktop's Qt platform theme, so without that option the
fallback was that theme's native chooser: GTK's through `libqgtk3` on GNOME,
which on Ubuntu 26.04 GNOME 50 never mapped and left only Qt's invisible modal
stand-in, and KDE's on Plasma, in neither case styled.
`PUTGD_FILE_DIALOG=fallback` skips the portal; the lab and the tests use it.

On Windows the helper opens Qt's dialog directly. With the Windows platform
plugin that is the native Windows dialog.

## Driving the dialogs

The dialogs are driven the way their buttons open them. Each answer carries the
chooser's state:
- `open`, `mode` (`open`, `save` or `folder`), `title`;
- `backend`: `portal` or `fallback`;
- `parentWindow` while the portal's chooser is open;
- `fallbackReason`: `disabled`, `no_session_bus`, `portal_version`,
  `portal_failed`, `portal_exited`, or the D-Bus error name;
- `last`: `status` (`chosen`, `cancelled` or `failed`), `backend`, `mode`,
  `path`, `fallbackReason`, `error`.

A file picked in the chooser goes through the same path as a person's choice.
The methods that save to a given path without a dialog remain:
`recording.saveCopy` and `editor.saveCopy`.

| Session method | MCP tool | Dialogs |
| --- | --- | --- |
| `recording.review {id, action: saveCopyDialog \| closeFileDialog}`; `inspect` reports `fileDialog` | `drive_recording_review` | Review's Save copy…; other review actions wait while it is open |
| `editor.fileDialog {id, action: open \| inspect \| cancel}` | `editor_file_dialog` | the editor's Save a copy… |
| `ui.fileDialog {dialog, action: open \| inspect \| cancel, field?, id?}` | `drive_file_dialog` | `editImage`; `historySaveAs`, for the selected file or the row with history or operation `id`; `outputFolder`, with `field` `root`, `images`, `videos`, `text` or `other`; `diagnosticsFolder` (open the package dialog first) |

Each step also logs a `putgd.filedialog` line naming the backend and, for the
fallback, the reason. Each portal `Response` is logged with its code and how long
after the portal accepted the request it came; a `2` also says which reading
applied.

## Tests

- **`file_dialog_integration`** runs against a fake `org.freedesktop.portal.FileChooser`
  on a private bus that can activate nothing (`dbus-run-session
  --config-file=tests/fixtures/dbus-session-isolated.conf`). It refuses to run
  where the real portal could be started. It covers:
  - the request options and the URI-to-path conversion;
  - cancel, the modal stand-in, and `Request.Close` on cancel and on destruction;
  - a late `Response` 2 in open, save and folder modes as a cancel with no fallback;
  - every fallback trigger, including an immediate `Response` 2 and an error reply;
  - the fallback's contrast and surfaces in Dark and Light, and a scheme switch while open.
- **`file_dialog_x11_parent`** checks the `x11:<xid>` handle under Xvfb.
- **`gui_review_window`, `gui_settings` and `mcp_integration`** cover the commands above.

### Native cancellation preconditions (GNOME 50, #847)

Start cancellation checks in an existing directory. Nautilus 50 can draw a
missing-folder alert inside the chooser when `current_folder` does not exist.
The compositor still reports the chooser's top-level focused, while the
alert's **OK** button has keyboard focus. The first Escape dismisses that
alert without cancelling the chooser or sending its `Request.Response`.
A second Escape cancels the chooser with Response `1`.

The surface driver's dialog fixture prepares its private `out/diagnostics`
directory before opening the diagnostics chooser. With no alert, one real
Escape cancels with Response `1` and preserves the selected diagnostics folder,
settings and operation list. This is synthetic fixture setup; the app does not
create a diagnostics directory merely to open its chooser. The fixture removes
the directory through its normal owned cleanup.

For a cancellation investigation, retain the focused control as well as the
compositor window, the matching frontend/backend request and response, and saved
before/after frames. An open request after an alert was dismissed does not
establish a lost portal response. See the [live trace and recipe
evidence](evidence/2026-10-30-diagnostics-folder-847/README.md).
