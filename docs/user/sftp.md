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
  ssh finds on its own, a key file with its passphrase, or a password. A server that asks for
  a one-time code (OTP, verification code) can't be used; sign in with a key instead.

## Setting it up

1. **Destinations → Add destination → SFTP**.
2. Enter the **Server**: `host`, `host:port`, `[2001:db8::7]:2200`, or a Host alias from
   `~/.ssh/config`. You can also paste a whole address: `sftp://alice@files.example.com:2222/srv/www/shots`
   or `alice@files.example.com:/srv/www/shots` fills **Server**, **User** and **Folder on server**
   at once. A password in the address is refused; enter it in **Password** instead.
3. **User** is optional. Left empty, put.gd uses the user ssh would: the one `~/.ssh/config`
   names for this server, else your own.
4. **Folder on server** is where uploads go. A path that doesn't start with `/` starts in the
   folder you log in to: `shots` and `~/shots` are the same folder; empty is the login folder
   itself. Another user's home is written as its full path (`/home/alice/shots`), not `~alice`.
5. **Link address** is optional: the public HTTPS address the folder is served at, such as
   `https://files.example.com/shots/`. See [Without a Link address](#without-a-link-address).
6. **Password** and **Key passphrase** are both optional. Leave them empty to sign in with your
   SSH agent or key files. **Save to keychain** keeps what you typed in the system keychain;
   **Use for this session** keeps it until put.gd quits. A new SFTP destination starts with
   **Use for this session**, so put.gd never asks the keychain about a destination that has
   nothing stored. There are two fields so a key passphrase never answers a password prompt,
   and a password never answers a key's passphrase prompt.
7. Press **Test upload** (or **Test upload and link** with a Link address).

Under **Folder on server**, one line says what ssh makes of the address, read from `ssh -G`:
for example "Connects to files.example.com:2222 as alice with key id_ed25519 (agent)". "(agent)"
means an SSH agent is there too. The line follows your `~/.ssh/config`, so an alias shows its
real host, port and user, and a ProxyJump shows "through …". It updates once the Server or User
you typed is saved.

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
| cleanup | The test file was checked and deleted again. |

A test that stops at a step names it, and the message says what to change.

In the destination list an SFTP destination shows **Needs a server** until Server is filled in,
then **Ready to test**, and **Connected** once a test passes, like the other destinations.

## Host keys

put.gd checks the server's host key against your own `known_hosts` files (as `ssh -G` names
them) first, and against put.gd's own `known_hosts` file, in put.gd's configuration folder,
second. It never writes your `~/.ssh` files.

- **Unknown key.** Uploads refuse with `ssh_host_unknown_key`. **Test** fetches the key over the
  same route the upload takes, without sending any credential, and shows it, such as
  `ED25519 SHA256:lKD1cde7…`. Compare it with the key your server's admin gave you
  (`ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` on the server), then press **Trust key**.
  put.gd fetches the key again and keeps it only if it is still exactly the one you were
  shown; the destination records its fingerprint. Then test again.
- **Changed key you trusted in put.gd** (`ssh_trusted_key_changed`): Settings shows the old and
  new fingerprints with a warning. Trust the new key only if your server's admin confirms it
  was replaced; **Trust new key** replaces the old one in put.gd's file.
- **Changed key in your own known_hosts** (`ssh_host_key_changed`): a hard error naming the file
  and line. It can mean someone is intercepting the connection. If the key was replaced on
  purpose, remove the old one with `ssh-keygen -R host` (`ssh-keygen -R "[host]:2222"` for
  another port) and test again.

Changing the Server clears the recorded fingerprint, because another server has another key.

## Folders

A missing folder is never created during an upload: the upload fails with
`sftp_folder_missing` and the full path it looked for. **Test** offers **Create folder**, which
creates that folder and any missing folders above it, with the server's default permissions.

## Uploads

Each file is created under its final name only if nothing on the server has that name yet,
and with no permissions of put.gd's own, so the server's umask decides them. put.gd never
writes a temporary name and renames it, because a rename replaces an existing file on several
servers. A name that is taken is handled by your naming rules (rename or skip); SFTP uploads
never replace a file. When the server offers it, put.gd asks it to flush the file to disk
before closing it.

Cancelling an upload removes the part already on the server. A cancel that arrives after the
last byte was sent is too late, and the upload finishes. If the connection drops in the
middle, put.gd can't remove the part; the upload is shown as unknown, and the partial file
may be left on the server.

A full disk is reported as `sftp_disk_full` with the free space, but only when the server's
own free-space figure confirms it: SFTP version 3 reports a full disk as a plain failure.

## Without a Link address

The upload still works. Nothing is copied, so the clipboard stays as it was, and the notice says
"Saved to files.example.com:/srv/www/shots/shot.png". In History, the file's menu has
**Copy path**, which copies that same `host:/path`. The upload summary never says "Shared link".

## Deleting from History

Before deleting, put.gd checks that the file on the server is still the one it uploaded: a
regular file with the same size, modification time and owner as recorded, and, for files up to
32 MB, the same bytes (it reads the file back and compares its SHA-256). A file that changed is
left alone and the deletion answers `changed`. SFTP has no conditional delete, so a file
replaced in the moment between that check and the removal is removed too, as R2's ignored
`If-Match` on DELETE is ([r2.md](r2.md#how-r2-differs-from-s3)). Web caches in front of Link
address may keep serving a deleted file for a while.

## Signing in

- **SSH agent.** put.gd uses `SSH_AUTH_SOCK` when your session sets it, else the agent sockets
  desktops provide under `$XDG_RUNTIME_DIR`: `gcr/ssh`, `keyring/ssh`, `ssh-agent.socket`,
  `openssh_agent` and `gnupg/S.gpg-agent.ssh`. An `IdentityAgent` in `~/.ssh/config` wins.
- **Key files** are the ones ssh tries on its own or that `IdentityFile` names. An encrypted key
  needs its passphrase in **Key passphrase**, unless it is already in your agent.
- **Password.** put.gd answers ssh's password prompt once, through a private socket only that
  connection can use; a wrong password is not retried.
- **Security keys** (FIDO, `-sk` keys) work as they do in a terminal: touch the key when it
  blinks. During **Test**, Settings' status line says "Touch your security key" while ssh waits
  for the touch. During an upload put.gd shows no such notice; a failed sign-in that waited for
  a touch says so in its error details (`touched`).

One connection serves a whole upload (its name check, upload and link) and is kept for about a
minute for the next one, so a burst of captures signs in once.

## Troubleshooting

| Code | What to do |
| --- | --- |
| `ssh_not_installed` | Install the OpenSSH client (`openssh-client`, `openssh-clients` or `openssh`) |
| `ssh_config_invalid` | `ssh -G` couldn't read your SSH settings for this server; the message has ssh's own words. Fix `~/.ssh/config` |
| `ssh_host_unknown_key`, `ssh_trusted_key_changed`, `ssh_host_key_changed`, `ssh_host_key_mismatch` | See [Host keys](#host-keys) |
| `ssh_auth_failed` | The message says which: a key that needs its passphrase, a server that asks for a password, no agent and no key or password at all, or credentials it refused. Check User too |
| `ssh_needs_one_time_code` | Sign in with a key or your agent |
| `ssh_host_not_found`, `ssh_connection_refused`, `ssh_connection_timeout` | Check the server name, port and network |
| `ssh_no_sftp` | Ask the admin to enable the `sftp` subsystem. Text your shell prints when it starts (from `.bashrc` or similar) also breaks SFTP: print it only in interactive shells |
| `sftp_folder_missing`, `sftp_not_folder` | Fix Folder on server, or use **Create folder** |
| `sftp_permission_denied` | The user can't write there; fix the folder's permissions or choose another |
| `sftp_disk_full` | Free space on the server |

Every code is listed in [errors.md](errors.md#sftp).

## Agents

`ui.sftpDestination` (`putgd-cli`) and `drive_sftp_destination` (MCP) read and edit the form,
paste an address into Server, press Test upload, **Trust key** (only with the exact fingerprint
the form offers) and **Create folder**, and add an SFTP destination, as a person does. They carry
no password or passphrase. `setup.inspectAccount`, `setup.trustHostKey` and `setup.createFolder`
(the MCP tool `sftp_destination_setup`) resolve the address, trust a key by its exact
fingerprint and create the saved folder for a saved destination. After a trust, record the
fingerprint with `settings.patch` at `/profiles/<id>/settings/hostKeyFingerprint`. A password or
key passphrase is stored with `setup.storeCredentials` over the session socket, as
`{"values":{"password":"…","passphrase":""}}`; `putgd-cli` refuses to store credentials, as it
does for every destination. See [cli.md](../automation/cli.md) and
[mcp.md](../automation/mcp.md); the settings are described in
[configuration.md](../design/configuration.md).
