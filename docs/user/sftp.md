# SFTP destination

put.gd uploads to your own server over SFTP as its own storage service, `sftp` (#1871). It
connects with your own OpenSSH client (`ssh -s sftp`, as sshfs does), so `~/.ssh/config`
works as it does in a terminal: Host aliases, ProxyJump, your SSH agent, security keys,
crypto policy and your `known_hosts`. put.gd speaks SFTP itself over that connection, so it
can create each file only when its name is free, check a file before it deletes it, show
progress and cancel cleanly. It links no SSH library.

## What it needs

- **The OpenSSH client.** The DEB, RPM and pacman packages depend on it (`openssh-client` on
  Debian, Ubuntu and Mint, `openssh-clients` on Fedora, `openssh` on Arch). With the AppImage,
  install it yourself if it is missing. Without `ssh`, Settings and every upload say
  `ssh_not_installed` and name the package.
- **A server with the SFTP subsystem.** Every OpenSSH server has it unless its admin turned it
  off. A server that offers only SCP or a shell isn't supported: SCP can't check whether a
  name is free, can't create a file only when it is, and can't check a file before deleting
  it, so put.gd would risk overwriting or deleting someone else's file.
- **A way to sign in** that works without typing into a terminal: your SSH agent, a key file
  ssh finds on its own, a key file with its passphrase, or a password the server accepts by
  password authentication. put.gd turns keyboard-interactive sign-in off, so a server that asks
  for a one-time code (OTP, verification code), or takes a password only through its own
  keyboard-interactive prompt, can't be used; sign in with a key instead.

## Setting it up

1. **Destinations → Add destination → SFTP**.
2. Enter the **Server**: `host`, `host:port`, `[2001:db8::7]:2200`, or a Host alias from
   `~/.ssh/config`. You can also paste a whole address: `sftp://alice@files.example.com:2222/srv/www/shots`
   or `alice@files.example.com:/srv/www/shots` fills **Server**, **User** and **Folder on server**
   at once, and `sftp://files.example.com/` sets Folder on server to `/`. Digits after a colon
   are always a port (1 to 65535), never a path. A pasted address must be valid as a whole: if
   any part isn't, no field changes, Server goes back to what it was, and the field says why.
   A password in the address is left out at once, wherever it was typed or pasted, and the field
   says so; enter it in **Password** instead.
3. **User** is optional. Left empty, put.gd uses the user ssh would: the one `~/.ssh/config`
   names for this server, else your own.
4. **Folder on server** is where uploads go. A path that doesn't start with `/` starts in the
   folder you log in to: `shots` and `~/shots` are the same folder; empty is the login folder
   itself. Another user's home is written as its full path (`/home/alice/shots`), not `~alice`.
5. **Link address** is optional: the public HTTPS address the folder is served at, such as
   `https://files.example.com/shots/`. A user name or password in it is left out as you type or
   paste it. See
   [Without a Link address](#without-a-link-address).

   A field that isn't valid isn't saved: its message stays beside it until you fix it.
6. **Password** and **Key passphrase** are both optional. Leave them empty to sign in with your
   SSH agent or key files. **Save to keychain** keeps what you typed in the system keychain;
   **Use for this session** keeps it until put.gd quits. A new SFTP destination starts with
   **Use for this session**, so put.gd never asks the keychain about a destination that has
   nothing stored. Storing saves only what you typed: a stored password stays when you store a
   passphrase, and the other way round. **Clear**, beside a stored one, forgets just that one and
   leaves whatever you typed into the other field.
   There are two fields so a key passphrase never answers a password prompt, and a password
   never answers a key's passphrase prompt.
7. Press **Test upload** (or **Test upload and link** with a Link address).

Under **Folder on server**, one line says what ssh makes of the address, read from `ssh -G`:
for example "Connects to files.example.com:2222 as alice with key id_ed25519 (agent)". "(agent)"
means an SSH agent is there too. The line follows your `~/.ssh/config`, so an alias shows its
real host, port and user, and a ProxyJump or ProxyCommand shows "through a jump host" or
"through a proxy command" (never the command itself). It updates once the Server or User you
typed is saved.

## What Test checks

The test reports each step and what it proved:

| Step | What passed means |
| --- | --- |
| connect | The server answered on its port (through any ProxyJump). |
| hostKey | Its host key matches one you trust (see [Host keys](#host-keys)). |
| signIn | The server accepted the user and key, agent or password. |
| folder | Folder on server exists and is a folder. |
| upload | A small test file was uploaded under a name nothing else uses. |
| link | The link, fetched anonymously as anyone given it would, returned the same bytes. Skipped without a Link address. |
| cleanup | The test file was checked and deleted again, and the server confirmed it. |

Every step's answer is checked: the flush to disk, the check after the file was closed and the
removal. A test that stops at a step names it, and the message says what to change. If the
server doesn't confirm the test file was removed, cleanup reads "unconfirmed" and the message
names the file to check. The test never removes a file that changed under the test file's name,
and a test file whose creation got no answer is reported the same way. The summary saying the
test passed appears only when every step passed.

In the destination list an SFTP destination shows **Needs a server** until Server is filled in,
then **Ready to test**, and **Connected** once a test passes, like the other destinations.

## Host keys

When your own `known_hosts` files (the `UserKnownHostsFile` and `GlobalKnownHostsFile` that
`ssh -G` names, looked up with `ssh-keygen -F`) have any key for the server, they decide alone,
as in a terminal. Only when they know nothing of it does put.gd add its own `known_hosts`, kept
at `ssh/known_hosts` in put.gd's configuration folder (a private folder, the file readable only
by you; put.gd won't hand it to ssh if it is anything else). It never writes your `~/.ssh`
files. If put.gd can't check your known_hosts files (ssh-keygen fails, or a file named with a
`%` token or `~user` it can't expand), nothing connects and you see
`ssh_known_hosts_unreadable`. If your SSH settings turn known_hosts files off
(`UserKnownHostsFile none`), put.gd uses none of its own either, and Trust key refuses with
`ssh_known_hosts_off`.

Host keys come only from those files: never from DNS, never skipped for localhost, with no
second check by IP address, and no keys learned from the server. A ProxyJump hop's own host key
is checked by your ssh settings, as in a terminal.

- **Unknown key.** Uploads refuse with `ssh_host_unknown_key`. **Test** fetches the key over the
  same route the upload takes, without sending any credential, and shows it, such as
  `ED25519 SHA256:lKD1cde7…`. Compare it with the key your server's admin gave you
  (`ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` on the server), then press **Trust key**.
  put.gd fetches the key again and keeps it only if it is still exactly the one you were
  shown, for that one exact host name. put.gd records its fingerprint in the destination
  itself, if Settings haven't changed meanwhile. Then test again. Trust refuses when your own
  `known_hosts` already has a key for the server (`ssh_host_key_known`: that one decides), and
  when its `HostKeyAlias` is a pattern or a list (`ssh_host_alias_pattern`).
- **Changed key you trusted in put.gd** (`ssh_trusted_key_changed`): Settings shows the old and
  new fingerprints with a warning. Trust the new key only if your server's admin confirms it
  was replaced; **Trust new key** replaces the old one in put.gd's file, and only while the file
  still holds the key this destination trusted.
- **Changed key in your own known_hosts** (`ssh_host_key_changed`): a hard error naming the
  file put.gd found the server's key in. It can mean someone is intercepting the connection. If the key was replaced on
  purpose, remove the old one with `ssh-keygen -R host` (`ssh-keygen -R "[host]:2222"` for
  another port) and test again.

Changing the Server clears the recorded fingerprint, because another server has another key.
A trusted key stays in put.gd's `known_hosts` after its last destination is removed; that
leftover does no harm.

## Folders

A missing folder is never created during an upload: the upload fails with
`sftp_folder_missing` and the full path it looked for. **Test** offers **Create folder**, which
creates that folder and any missing folders above it, with the server's default permissions.
The offer is only for the folder the test found missing: editing Folder on server or Server,
pasting included, hides it, and put.gd creates the folder only while it still resolves to the
path the test reported. An agent's edit hides it too. A folder whose path on the server isn't
valid UTF-8 is refused before anything is written (`sftp_path_unsupported`), and so is Create
folder for a relative Folder on server when the server doesn't name your login folder in UTF-8.

## Uploads

Each file is created under its final name only if nothing on the server has that name yet,
and with no permissions of put.gd's own, so the server's umask decides them. put.gd never
writes a temporary name and renames it, because a rename replaces an existing file on several
servers. A name that is taken is handled by your naming rules (rename or skip); SFTP uploads
never replace a file. When the server offers it, put.gd asks it to flush the file to disk
before closing it.

Cancelling an upload removes the part already on the server; the message says so only when the
server confirmed it, and otherwise names the file to check. A cancel counts from the moment
you press it, even before the upload started. Once put.gd has asked the server to create the
file, it waits up to 20 seconds for that answer before it stops, so it can remove the file. A cancel that arrives
after the last byte was sent is too late, and the upload finishes. If the connection drops in
the middle, put.gd can't remove the part; the upload is shown as unknown, and the partial file
may be left on the server.

After an unknown upload, put.gd claims a file at its name as this upload only with its own
evidence: before the file was closed, put.gd saved the size, time and owner the server reported
for its own handle, with the file's SHA-256. A file that matches (and, up to 256 MB, reads back
the same) is this upload. Otherwise the upload stays unknown with the path to check
(`sftp_reconcile_unknown`). A name that was taken when the file was created is never claimed.

Each upload's record names the server it went to: the host, port and user ssh resolved and the
`known_hosts` lines that hold that server's key (shown as its fingerprint), besides the file's
size, time, owner and SHA-256. An upload whose server put.gd can't find in those files isn't
sent.

A full disk is reported as `sftp_disk_full` with the free space, but only when the server's
own free-space figure confirms it: SFTP version 3 reports a full disk as a plain failure.

## Without a Link address

The upload still works. Nothing is copied, so the clipboard stays as it was, and the notice says
"Saved to files.example.com:/srv/www/shots/shot.png". In History, the file's menu has
**Copy path**, which copies that same `host:/path`. The upload summary never says "Shared link",
and the capture output in the status band and Settings says "Upload only".

## Deleting from History

Before deleting, put.gd connects with only the recorded `known_hosts` lines, so ssh itself
proves the server still holds the key it had at the upload; if it doesn't, or the destination
now reaches another host, port or user, put.gd refuses with `sftp_server_changed`. Then it checks that the file is still the one it uploaded: a regular
file with the same size, modification time and owner as recorded, and, for files up to 32 MB,
the same bytes (it reads the file back and compares its SHA-256). A file that changed is left
alone and the deletion answers `changed`. put.gd saves its intent to delete before these checks,
checks the file once more right before the removal, and keeps that intent on record when the
server's answer is lost. A record without these details can't be deleted
(`sftp_delete_unavailable`).

SFTP has no conditional delete, so a file replaced in the moment between that last check and
the removal is removed too, as R2's ignored `If-Match` on DELETE is
([r2.md](r2.md#how-r2-differs-from-s3)). Web caches in front of Link address may keep serving a
deleted file for a while.

## Signing in

- **SSH agent.** put.gd uses `SSH_AUTH_SOCK` when your session sets it, else the agent sockets
  desktops provide under `$XDG_RUNTIME_DIR`: `gcr/ssh`, `keyring/ssh`, `ssh-agent.socket`,
  `openssh_agent` and `gnupg/S.gpg-agent.ssh`. An `IdentityAgent` in `~/.ssh/config` wins.
- **Key files** are the ones ssh tries on its own or that `IdentityFile` names. An encrypted key
  needs its passphrase in **Key passphrase**, unless it is already in your agent.
- **Password.** put.gd answers ssh's own password prompt for this server
  (`alice@files.example.com's password:`) once, through a private socket only that connection
  can use; a wrong password is not retried. The passphrase answers only ssh's
  `Enter passphrase for key '…':` for a key file your settings name, compared as the whole
  prompt, and only when the ssh put.gd started asks. Any other prompt gets no answer: a jump
  host's password prompt, a server's own keyboard-interactive text, a PIN. Only
  requests that sign in get the password and passphrase; checking the address learns only
  whether a password is set, and put.gd's helper forgets both once it has signed in.
- **Security keys** (FIDO, `-sk` keys) work as they do in a terminal: touch the key when it
  blinks. During **Test**, Settings' status line says "Touch your security key" while ssh waits
  for the touch. During an upload put.gd shows no such notice; a failed sign-in that waited for
  a touch says so in its error details (`touched`).

One connection serves a whole upload (its name check, upload and link) and is kept for about a
minute for the next one, so a burst of captures signs in once. An idle one ends on time even if
nothing asks again.

## Troubleshooting

| Code | What to do |
| --- | --- |
| `ssh_not_installed` | Install the OpenSSH client (`openssh-client`, `openssh-clients` or `openssh`) |
| `ssh_config_invalid` | `ssh -G` couldn't read your SSH settings for this server. Run `ssh -G <server>` in a terminal and fix `~/.ssh/config` |
| `ssh_host_unknown_key`, `ssh_trusted_key_changed`, `ssh_host_key_changed`, `ssh_host_key_mismatch`, `ssh_host_key_known`, `ssh_known_hosts_off`, `ssh_host_alias_pattern` | See [Host keys](#host-keys) |
| `ssh_auth_failed` | The message says which: a key that needs its passphrase, a server that asks for a password, no agent and no key or password at all, or credentials it refused. Check User too |
| `ssh_needs_one_time_code` | The server wants keyboard-interactive sign-in. Sign in with a key or your agent, or ask the admin to allow password authentication |
| `ssh_failed` | ssh stopped for another reason; the message gives its exit status. Run `ssh -v <server>` in a terminal to see why |
| `ssh_host_not_found`, `ssh_connection_refused`, `ssh_connection_timeout` | Check the server name, port and network |
| `ssh_no_sftp` | Ask the admin to enable the `sftp` subsystem. Text your shell prints when it starts (from `.bashrc` or similar) also breaks SFTP: print it only in interactive shells |
| `sftp_folder_missing`, `sftp_not_folder` | Fix Folder on server, or use **Create folder** |
| `sftp_permission_denied` | The user can't write there; fix the folder's permissions or choose another |
| `sftp_disk_full` | Free space on the server |
| `sftp_server_changed` | The server no longer holds the key it had at the upload, or the destination now reaches another server; put.gd leaves that upload alone |
| `ssh_known_hosts_unreadable` | put.gd couldn't check your known_hosts files; fix the file or the `UserKnownHostsFile` setting the message names |
| `sftp_path_unsupported` | Choose a folder whose name on the server is plain UTF-8 |

Every code is listed in [errors.md](errors.md#sftp).

## Agents

`ui.sftpDestination` (`putgd-cli`) and `drive_sftp_destination` (MCP) read and edit the form,
paste an address into Server, press Test upload, **Trust key** (only with the exact fingerprint
the form offers), **Create folder**, Store (for what a person typed into Password or Key
passphrase) and **Clear**, and add an SFTP destination, as a person does. They carry no password
or passphrase. `setup.inspectAccount`, `setup.trustHostKey` and `setup.createFolder` (the MCP
tool `sftp_destination_setup`) resolve the address, trust a key by its exact fingerprint and
create the saved folder (up to 4096 bytes, with the path the test reported) for a saved
destination. A trust records the
fingerprint in the destination itself. A password or key passphrase is stored with
`setup.storeCredentials` over the session socket, one or both at a time, as
`{"values":{"password":"…"}}`; the other one stays as stored, and an empty value clears that
one. `putgd-cli` refuses to store credentials, as it does for every destination. A no-link
upload's place on the server (what Copy path copies) is its receipt's
`objectReference.location` in `history.detail` (MCP `get_history_entry`), beside the server
identity it was verified as. See [cli.md](../automation/cli.md) and
[mcp.md](../automation/mcp.md); the settings are described in
[configuration.md](../design/configuration.md).

## Limits

- A `UserKnownHostsFile` path with a space in it isn't supported: `ssh -G` prints the paths
  separated by spaces.
- A file someone else writes with exactly the bytes, size, time and owner of an upload, and a
  file swapped in at the path while put.gd's own upload still holds it open, aren't told apart
  from put.gd's own.
- Only files up to 32 MB are read back to check their bytes before a deletion; a larger one is
  checked by its size, time, owner and the server's recorded key.
- ssh shows only the first 100 bytes of a key's path in its passphrase prompt. When two of
  your key files share those 100 bytes, put.gd can't tell which one ssh asks about and
  answers neither; give one a shorter path or add it to your agent.
- With a port other than 22, an upload is refused if your known_hosts holds the server's key only
  under its bare name (no `[host]:port` entry): put.gd records the entry ssh checks and fails
  closed when it finds none.
- A pasted Server text that isn't an address put.gd can read is refused as a whole; put.gd
  doesn't guess at malformed plain host text.
- A folder that a symbolic link inside an existing parent points somewhere else between Test and
  Create folder isn't noticed.
- A symbolic link changed between the name check and the upload isn't noticed.
- put.gd doesn't bound SFTP replies other than file data below the protocol's own limits; the
  paths it sends are at most 4096 bytes.
- A port written with leading zeros (`000022`) is read as its number.
- A server key replaced in your known_hosts while a connection is signing in isn't noticed: the
  upload records the key the file holds after sign-in.
