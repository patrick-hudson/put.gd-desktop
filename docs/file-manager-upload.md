# Upload with put.gd from a file manager

Right-click one or more files or folders in a file manager and choose
**Upload with put.gd**. They upload right away to the capture destination,
or the only destination when there is no capture one ([Destination](#destination)),
and are shared and copied exactly as a capture is (#1105): with a share link,
the link is copied; a private upload makes no link and leaves the clipboard
alone. The usual finished or failure notification appears unless **Show
desktop notifications** is off. History manages the uploads afterwards.
There is no confirmation step and no extra "uploading" notice.

The design, its council review and the decisions behind it are in
[GitHub issue #1105](https://github.com/patrick-hudson/put.gd-desktop/issues/1105).

## Where the entry appears

| Desktop | File manager | Entry | DEB/RPM | AppImage |
|---|---|---|---|---|
| GNOME, Unity, Flashback, Budgie | Nautilus 43+ | top level (plugin); AppImage: Scripts submenu | plugin `libputgd-nautilus.so` | Nautilus script |
| GNOME desktop icons (DING) | — | Scripts submenu | per-user script | per-user script |
| KDE Plasma 5.27 and 6 | Dolphin, Konqueror | top level | KIO service menu | per-user service menu (not on Plasma 5.27) |
| Cinnamon, Budgie | Nemo | top level | `.nemo_action` | per-user `.nemo_action` |
| XFCE | Thunar | Send To | Send To file | per-user Send To file |
| MATE | Caja | top level (plugin); AppImage: Scripts | plugin `libputgd-caja.so` | Caja script |
| LXQt, LXDE | PCManFM-Qt, PCManFM | top level | FileManager-Actions file | per-user file |
| Pantheon | Files | top level | Contractor `.contract` | per-user `.contract` |
| COSMIC | COSMIC Files epoch-1.0.10+ | top level | per-user `context_actions` entry | per-user `context_actions` entry |
| UKUI | Peony | top level, folders and desktop | plugin `libputgd-peony.so` | not supported (use the DEB) |

Not supported: Deepin, COSMIC Files before epoch-1.0.10, the AppImage on UKUI
and on Plasma 5.27, Enlightenment's EFM and Trinity. Flatpak is not a target.

**Thunar** shows the entry under Send To for every file and folder; the Send
To file has no `MimeType` key because Thunar matches types exactly. **Pantheon
Files** may also show it on a background right-click, which uploads the
current folder. On a DEB/RPM GNOME desktop with DING, Nautilus shows the
top-level plugin entry plus the same action under Scripts; the script is the
only way into DING's menu.

## Turning it on

- **DEB and RPM:** installed with the package, always on. Files, Caja and
  Peony load their plugins only when they start, and Files keeps running after
  its last window closes, so the entry may appear only after a restart. The
  first time put.gd runs after installing, its notice names each file manager
  that needs one and offers **Restart Files** (or Caja, Peony), which closes
  all of that file manager's windows.
- **AppImage:** the first time it runs, put.gd writes per-user entries and
  shows "Upload with put.gd is now in your file manager's right-click menu",
  once per installation. **Settings → General → File manager → File manager
  menu** turns them off (removing only the files this AppImage wrote) and on
  again; `preferences.fileManagerMenu` holds the switch.

Per-user files:

| Format | Path |
|---|---|
| KIO (mode 0755) | `$XDG_DATA_HOME/kio/servicemenus/putgd-upload.desktop` |
| Nemo | `$XDG_DATA_HOME/nemo/actions/putgd-upload.nemo_action` |
| Thunar | `$XDG_DATA_HOME/Thunar/sendto/putgd-upload.desktop` |
| FileManager-Actions | `$XDG_DATA_HOME/file-manager/actions/putgd-upload.desktop` |
| Contractor | `$XDG_DATA_HOME/contractor/putgd-upload.contract` |
| Nautilus | `$XDG_DATA_HOME/nautilus/scripts/Upload with put.gd` |
| DING | `$HOME/.local/share/nautilus/scripts/Upload with put.gd` |
| Caja | `$XDG_CONFIG_HOME/caja/scripts/Upload with put.gd` |
| COSMIC | `$XDG_CONFIG_HOME/cosmic/com.system76.CosmicFiles/v1/context_actions`, backup `context_actions.putgd-backup` |

The AppImage's stable launcher (`putgd-upload`), icon copy (`putgd.svg`),
ownership record (`<installation>.owned`) and version stamp
(`<installation>.stamp`) live in `<dataRoot>/file-manager/`. The launcher
finds the current AppImage the way login startup does, so it survives updates
and renamed images. When the AppImage is deleted, the integration's own
systemd path watch removes the files it owns and its COSMIC entry, whether or
not Launch at login is on.

COSMIC keeps every user action in one shared list. put.gd recognises its
entry by its command, parses the list before writing, keeps a backup, parses
the result back and replaces the file atomically; other entries are never
touched. If the list can't be parsed, nothing is written and a notification
says so. A DEB/RPM install writes its DING script and COSMIC entry at every
start when the desktop is present, so enabling DING or installing COSMIC
later still adds them. Removing the package leaves both behind: the script
removes itself the next time it runs, and the COSMIC entry's command fails
harmlessly.

When a DEB/RPM and the AppImage are both installed, the AppImage skips each
per-user file whose system counterpart exists, so there is one entry.

## What happens

The entry runs `putgd --upload -- <paths or URIs>` (DEB/RPM through
`/usr/bin/putgd-launcher`; the AppImage through its stable launcher, then
AppRun's `--upload` fast path). That short-lived client:

1. **Resolves** each argument before any Qt object exists: an absolute path
   stays as given; a URI (sftp, smb, an MTP phone, Recent and Starred) becomes
   its GVfs FUSE or KIOFuse path. DEB/RPM builds use GIO in-process. The
   AppImage decodes `file://` itself and asks the host's `gio info` for the
   rest, in an English UTF-8 locale with a 30 s deadline. Anything without a
   local path is refused with one "These items have no local path"
   notification. Network locations through GVfs need `gvfs-fuse` (a
   recommended package of the DEB and RPM).
2. **Leaves the file manager's cgroup** when it was started from a
   D-Bus-activated file manager's transient `.service`, by re-running under
   `systemd-run --user --scope`, so closing the file manager can't stop it.
   The AppImage's launcher does this on the host before the image mounts.
3. **Hands the selection to the resident.** Without one, it starts put.gd once
   (an exclusive start lock in `$XDG_RUNTIME_DIR/putgd/start.lock`) in its own
   scope, from `$HOME`, so it never keeps a USB stick busy or lives in the file
   manager's cgroup. A resident started this way doesn't open Settings when
   there is no tray. Every `systemd-run` call passes
   `--expand-environment=no`, so a file named `${USER}.txt` stays itself.
4. **Waits until put.gd holds its own copy** (the operation leaves `accepted`
   and `acquiring`), so KIO's temporary copies and slow GVfs reads stay
   available, then exits.

The resident's `upload.files` refuses a request from another installation
(`installation_mismatch`: a menu entry of the DEB never uploads through a
running AppImage, or the other way round). It checks every path before
anything is accepted. Each must be an existing, readable regular file or
folder; empty files, names the naming policy rejects (control characters,
newlines, backslashes under original names), files over the Largest file
limit or the destination's largest object, a batch over the Largest batch
limit and a disk that can't hold the import are refused, naming every bad
file. Nothing is uploaded then. A file that becomes unreadable while it is
copied fails the whole operation, naming the file.

After an upgrade of the DEB or RPM, the old resident keeps running and doesn't
know `upload.files`; the entry then says "put.gd was just updated. Quit put.gd
from its tray menu or log out and in, then try again."

## Destination

File-manager uploads always upload; they never fall back to a local-only
save. The destination is resolved once when the request is accepted and stored
with the operation, so a retry or a restarted resident uses the same one:

1. the capture default destination (**Settings → Capture → Capture output**)
   when it is an upload with a destination;
2. otherwise the only destination, when there is exactly one;
3. otherwise the request is refused with `destination_required` and Settings
   opens on Destinations.

Sharing follows the capture default's setting (destination default, share
link or private). Keep a local copy still applies. Links are copied as for a
capture.

## Folders

**Settings → General → File manager → Folders** (`preferences.fileManagerFolders`):

- **Upload as a .zip** (default, `zip`): each folder becomes `<folder>.zip`,
  one item and one link, which opens to the folder. Hidden files and empty
  folders are included; symlinks, sockets, FIFOs and devices are skipped and
  never followed; names are UTF-8. Each entry is stored or compressed
  depending on whether its first MiB compresses, and every source file is
  read once. Archives always use ZIP64, so folders over 4 GiB or with more
  than 65,535 entries work, and memory stays at a few MiB whatever the size.
  Before anything is written, the size limits check the most the folder's
  archive could take (its files' total plus zip overhead), so a folder whose
  zip would fit can still be refused. A folder or file whose name contains a
  backslash or isn't valid UTF-8 is refused by name: a zip can't carry it
  portably. Free disk must hold the archive
  plus a temporary copy of its central directory. If a file grows while it is
  zipped past the size it was checked against, the upload fails, names the
  file and asks you to try again.
- **Upload each file** (`files`): every regular file under the folder uploads
  as its own item, named by its file name (same names get the usual collision
  handling); symlinks are skipped. The file limit (`limits.maxFiles`, 100 by
  default, up to 10,000 in Settings → Advanced) counts the expanded files, and
  a folder over it is refused by name.

Mixed selections of files and folders follow these rules per item, in one
upload.

## Notifications without a tray

Notifications go to `org.freedesktop.Notifications` whether or not a
StatusNotifier tray exists, so tray-less GNOME shows the finished banner (with
its Open in browser button) and failures. The urgency is a D-Bus byte: 1 for
success, 2 for failure. Turn notifications off with **Show desktop
notifications**; the client's own refusal notices still appear, since nothing
else would report them.

## Agents and the CLI

`upload.files` `{paths, profileId?, installation?}` takes absolute paths. The
CLI form is `putgd-cli upload.files '{"paths":["/abs/a.png","/abs/folder"]}'`,
and the MCP tool is `upload_files(paths, profile_id?)`. Without `installation`
there is no identity check. Poll `operation.inspect` with the returned
`operationId`. Both settings take `settings.patch` and MCP
`patch_configuration` like any other.
