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
   again** opens the page again. Sign-in stops by itself after 20 minutes.
4. **Folder** is `put.gd` by default: a folder in your own files, such as `put.gd` or
   `Screenshots/put.gd`. put.gd creates it on the first upload.
5. Press **Test upload and link**. It uploads a small test file, makes its share link, opens the
   link without signing in and compares the file, then deletes it.

Signing in gives put.gd an **app password** of its own; your Nextcloud password never reaches
put.gd. The app password is kept in the system keychain, or until put.gd quits when Credential
storage is **For this session**. Nextcloud lists it under **Personal settings → Security →
Devices & sessions** as `put.gd (computer name)`.

**Sign out** revokes that app password on the server and forgets it here. Removing a Nextcloud
destination in Settings does the same before it removes the destination. If the server can't be
reached or doesn't confirm, put.gd still forgets the password and says so; revoke it in Nextcloud
under **Devices & sessions**.

The app password works only for the server and account it was given for. After you change the
Server to another address, put.gd asks you to sign in again and sends nothing to the new address
until you do.

### Use an app password instead

Servers without browser sign-in (Nextcloud's Login Flow v2), such as ownCloud 10, take an app
password made by hand. In Nextcloud, **Personal settings → Security → Devices & sessions →
Create new app password**. In put.gd, press **Use an app password instead** and enter the
**Login name** and **App password** under Credentials, then **Save to keychain** or **Use for
this session**. put.gd then asks the server whose account it is and shows **Signed in as**.

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
  (`nextcloud_sharing_disabled`).

If the upload works but the link can't be made (the server was unreachable, or it enforces a
link password, for example), the file stays in Nextcloud and History offers **Finish share
link**, which makes the link without uploading again. A failed link never deletes the file.

put.gd's link is labelled `put.gd` and a short code in Nextcloud's sharing panel. put.gd only ever
uses, finishes or revokes its own link: a link you made by hand for the same file is left alone.

### Large files

Files above 50 MB go up in 16 MB chunks (Nextcloud's chunked upload v2) and are assembled at the
end, so a server's upload size limit applies to each chunk rather than the whole file. The file
appears at its name only once it is complete, and never replaces a file that appeared there
meanwhile. A cancelled or failed upload leaves nothing at the name; its chunks are removed.

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
   read without signing in.
4. Enter the **User** and **Password** under Credentials, then **Save to keychain** or **Use for
   this session**.
5. Press **Test upload and link**.

The folder must exist. When it doesn't, Test says so and offers **Create folder**.

A Nextcloud server also speaks WebDAV. When Test finds one, it suggests the Nextcloud service,
which signs in through the browser and makes share links. put.gd never changes the service by
itself.

### What Test checks

Besides the upload, the link and the cleanup, Test checks two things the server should do:

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
it can't confirm the file that way (files over 256 MB, or a server that gives no usable ETag),
Delete is unavailable for that upload, and Test leaves its test file on the server and says so.

## Both services

- **Certificates**: put.gd trusts what the system trusts and nothing else. A server whose
  certificate isn't trusted is refused with a message that says why (an unknown issuer, an
  expired certificate or another host name) and gives the certificate's SHA-256 fingerprint
  and the command that adds your own certificate authority to this distribution's trust store,
  such as `sudo cp server-ca.crt /usr/local/share/ca-certificates/ && sudo update-ca-certificates`
  on Debian, Ubuntu and Mint or `sudo cp server-ca.crt /etc/pki/ca-trust/source/anchors/ && sudo update-ca-trust`
  on Fedora.
- **Plain HTTP**: an `http://` address on another computer is refused unless **Allow plain HTTP
  for this destination** is on. That switch shows only for such an address. Plain HTTP sends
  files and the password unencrypted.
- **Slow connections**: requests that carry file data have no fixed time limit; they stop only
  when no data has moved for a minute.
- **Names**: an upload never replaces a file. A name that is taken follows the collision rule in
  Settings → Capture (rename or skip).

## Agents

`ui.davDestination` (`putgd-cli`) and `drive_dav_destination` (MCP) edit the form, press Sign in,
Cancel sign-in, Open browser again, Sign out, Create folder and Test as a user does, and read the
sign-in page's address (`signInUrl`) while sign-in waits, so an agent can complete it in a
browser. Without the Settings window, `setup.connectAccount` (`sign_in_destination`) starts
sign-in; its job's `event.url` is the page, and `setup.cancel` (`cancel_setup_job`) stops it.
`setup.signOut` (`sign_out_destination`) revokes and forgets the app password, and
`setup.createFolder` (`create_destination_folder`) makes a missing folder. `putgd-cli` and the
MCP helper never carry a password: agents sign in through Login Flow v2, and an app password made
by hand is typed into Settings. See [cli.md](../automation/cli.md), [mcp.md](../automation/mcp.md) and the settings in
[configuration.md](../design/configuration.md).
