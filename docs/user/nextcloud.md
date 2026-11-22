# Nextcloud and WebDAV destinations

put.gd uploads to Nextcloud and to plain WebDAV servers as two storage services, `nextcloud`
and `webdav` (#1872). Both run every request in one helper, `putgd-webdav-worker`, so the
resident itself opens no network connections for them.

- **Nextcloud** signs in through your browser, puts each upload in a folder of your files and
  makes a public share link for it, which put.gd copies.
- **WebDAV** uploads to a folder on any WebDAV server with a user name and password. There is no
  share step: the link is the file's address, or the address of a public location that serves
  the same folder.

## Nextcloud

### Setting it up

1. **Destinations → Add destination → Nextcloud**.
2. Enter the **Server**: the address you open Nextcloud at, such as
   `https://cloud.example.com`. A pasted Files link or share link works too; it becomes the
   install's base address, keeping a subfolder install path such as
   `https://example.com/nextcloud`.
3. Press **Sign in…**. Your browser opens Nextcloud's **Connect to your account** page. Log in
   there and choose **Grant access**. Settings then shows **Signed in as** your name. Until you
   finish, Settings says it is waiting; **Cancel sign-in** stops waiting and **Open browser
   again** opens the page again (**Try opening browser again** when the browser didn't open).
   Sign-in stops by itself after 20 minutes.
4. **Folder** is `put.gd` by default: a folder in your own files, such as `put.gd` or
   `Screenshots/put.gd`. Test or the first upload creates it, and any folder above it.
5. Press **Test upload and link**. It uploads a small test file, makes its share link, opens the
   link without signing in and compares the file, then deletes it. When put.gd can't confirm the
   file is still its test file, it leaves the file there and says so.

Signing in gives put.gd an **app password** of its own; your Nextcloud password never reaches
put.gd. It is kept in the system keychain; to keep it only until put.gd quits, choose **This
session only** under **Credential storage** before you sign in. Nextcloud lists it under
**Personal settings → Security → Devices & sessions** as `put.gd (computer name)`.

**Sign out** tries to revoke that app password on the server, then forgets it here. Removing a
Nextcloud destination does the same first. When the server can't be reached put.gd says so, but
when the server refuses (for example, the password was already revoked) Settings shows only
**Signed out**. Remove any put.gd entry left under **Devices & sessions**.

The app password works only for the server address (scheme, host and port) and account it was
given for; a new install path on the same address keeps it. **Sign out before you change the
address**: after the change put.gd asks you to sign in again, sends nothing to the new address
until you do, and can't use the old app password to revoke it.

### Use an app password instead

A Nextcloud-compatible server without browser sign-in (Login Flow v2) takes an app password
made by hand. In Nextcloud, **Personal settings → Security → Devices & sessions →
Create new app password**. In put.gd, press **Use an app password instead** and enter the
**Login name** and **App password** under Credentials, then **Save to keychain** or **Use for
this session**. put.gd then asks the server whose account it is and shows **Signed in as**; until
it has, the app password is used for nothing else. **Test upload and link** asks first too.

### Links

Every upload gets a public, view-only share link to Nextcloud's share page,
`https://cloud.example.com/s/TOKEN`. Anyone with the link can view and download the file.
There is no choice of link type, expiry or password in put.gd:

- When the administrator **enforces an expiry date** on public links, put.gd asks for the
  latest date allowed and History shows when the link expires.
- When the administrator **enforces a password** on public links, put.gd can't make a link:
  Sign in and Test say so (`nextcloud_share_password_required`), and an upload stays in your
  files without a link.
- When **public links are turned off**, an upload is refused before anything is sent
  (`nextcloud_sharing_disabled`). When the share step is the first to find them off, the file
  stays in your files without a link.

If the upload works but the link can't be made (the server was unreachable, or it enforces a
link password, for example), the file stays in Nextcloud and History offers **Finish share
link**. It retries the link step without uploading again and succeeds once the cause is fixed.
A failed link never deletes the file. **Inspect link** checks the link, and **Revoke link…**
removes it; the file stays in Nextcloud.

put.gd's link is labelled `put.gd` and the upload's ID in Nextcloud's sharing panel. put.gd only ever
uses, finishes or revokes its own link: a link you made by hand for the same file is left alone.

### Large files

Files above 50 MiB go up in chunks of 16 MiB, larger when needed to stay within 10,000 chunks
(Nextcloud's chunked upload v2), so a server's upload
size limit applies to each chunk rather than the whole file. Nextcloud then assembles the file,
which can take up to 30 minutes. The file appears at its name only once it is complete, and
never replaces a file that appeared there meanwhile. A cancel, a lost reply or a server error
during assembly can leave the complete file there with the outcome unknown; History checks it before anything
is sent again. Otherwise a cancelled or failed upload leaves nothing at the name. Its chunks are
removed, or left for Nextcloud's own cleanup when that fails (`webdav_cleanup_failed`).

### Deleting from History

**Delete** removes the file only while it is still the one uploaded: put.gd sends the file's
recorded ETag, and a file another app changed since is refused with **Changed** and left alone.
Its share link stops working. Nextcloud keeps deleted files in its trash bin until the trash is
emptied or expires.

## WebDAV

### Setting it up

1. **Destinations → Add destination → WebDAV**.
2. Enter the **Folder address**: the WebDAV address of the folder uploads go to, such as
   `https://dav.example.com/uploads/`.
3. Optional: **Link address**, where the server serves the same folder publicly, such as
   `https://files.example.com/uploads/`. Links are this address and the file name. Without it,
   the link is the WebDAV address of the file, which opens only where the server lets visitors
   read without signing in. Test opens the link without signing in and fails with
   `public_url_unreachable` when it needs a login.
4. Enter the **User** and **Password** under Credentials, then **Save to keychain** or **Use for
   this session**.
5. Press **Test upload and link**.

The folder must exist. When it doesn't, Test says so and offers **Create folder**, which makes
that one folder: the folder above it must already exist.

A Nextcloud server also speaks WebDAV. When Test finds one, it suggests the Nextcloud service,
which signs in through the browser and makes share links. put.gd never changes the service by
itself.

### What Test checks

Besides the upload, the link and the cleanup, Test checks two things the server should do, when
the server gives uploads a usable ETag (without one it skips both and warns):

- refuse a second upload to the same name (`If-None-Match: *`). put.gd checks names before each
  upload, but a server that ignores this could let two uploads of the same name at the same
  moment replace each other.
- refuse a deletion whose ETag doesn't match (`If-Match`). A server that ignores this could
  delete a file that was replaced on the server since its upload.

A server that ignores either gets a warning under the form; uploads still work.

### Deleting from History

**Delete** sends the file's recorded ETag (`If-Match`), so a file replaced on the server is
refused with **Changed**. Servers such as Apache's mod_dav don't return an ETag when a file is
written; put.gd then reads the file back once and compares it before it records the ETag. When
it can't confirm the file that way (files over 256 MiB, or a server that gives no usable ETag),
Delete is unavailable for that upload, and Test leaves its test file on the server and says so.

## Both services

- **Certificates**: put.gd trusts what the system trusts and nothing else. A server whose
  certificate isn't trusted is refused with a message that says why (an unknown issuer, an
  expired certificate or another host name). When a certificate was received, the message gives
  its SHA-256 fingerprint, and for an unknown issuer the command that adds your own certificate
  authority to this distribution's trust store, such as `sudo cp server-ca.crt /usr/local/share/ca-certificates/ && sudo update-ca-certificates`
  on Debian, Ubuntu and Mint or `sudo cp server-ca.crt /etc/pki/ca-trust/source/anchors/ && sudo update-ca-trust`
  on Fedora.
- **Plain HTTP**: an `http://` address on another computer is refused unless **Allow plain HTTP
  for this destination** is on. That switch shows for an `http://` Server, Folder address or
  Link address on another computer, and while it is on. Plain HTTP sends files and the password
  unencrypted.
- **Slow connections**: requests that carry file data have no fixed time limit; they stop only
  when no data has moved for a minute.
- **Names**: an upload never replaces a file. For a name that is taken, choose **Add a unique
  suffix** or **Skip duplicate names** under Settings → Capture → File names → **When a name
  exists**; **Replace the checked object**
  fails with `unsupported_capability`. On a WebDAV server that ignores `If-None-Match` (Test
  warns), two uploads of one name at the same moment could still replace each other.

## Limits

- **A file replaced at the moment its link is made.** Nextcloud's share API picks the file by
  its path only. put.gd checks that the file is still the one it uploaded just before it asks for
  the link, and checks the link it gets back right after. If another app replaced the file in
  between, put.gd removes that link at once and reports the file as changed; until the removal is
  confirmed the outcome stays unknown. For that moment the link existed.
- **Encoded slashes.** The Server field doesn't keep an encoded slash (`%2F`) in an install path.
- **Credentials belong to an address.** An app password or WebDAV password is used only for the
  server address (scheme, host and port) it was given for, as browsers scope Basic
  authentication. Another folder on the same server keeps it.

## Agents

`ui.davDestination` (`putgd-cli`) and `drive_dav_destination` (MCP) edit the form, press Sign in,
Cancel sign-in, Open browser again, Sign out, Create folder and Test as a user does, and read the
sign-in page's address (`signInUrl`) while sign-in waits, so an agent can complete it in a
browser. Without the Settings window, `setup.connectAccount` (`sign_in_destination`) starts
sign-in; its job's `event.url` is the page, and `setup.cancel` (`cancel_setup_job`) stops it.
`setup.signOut` (`sign_out_destination`) tries to revoke the app password and forgets it, and
`setup.createFolder` (`create_destination_folder`) makes a missing folder. `putgd-cli` and the
MCP helper never carry a password: agents sign in through Login Flow v2, and an app password made
by hand is typed into Settings. See [cli.md](../automation/cli.md), [mcp.md](../automation/mcp.md) and the settings in
[configuration.md](../design/configuration.md).
