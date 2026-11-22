# Storage servers for the SFTP and Nextcloud destinations

Real servers to test the SFTP destination (#1871) and the Nextcloud and WebDAV destination
(#1872) against, each in rootless Podman containers on one builder. The recipe is
`tools/lab/storage_servers/`.

| Server | Version | Port | What it is |
| --- | --- | --- | --- |
| SFTP | OpenSSH from Alpine 3.24 | 18371 | Users `putgd-key` (key only) and `putgd-pw` (password only). Folders `/upload` (writable, also served publicly), `/ro` (read-only) and `/full` (a 1 MB tmpfs, filled) |
| SFTP without SFTP | the same | 18372 | The same two users, shell and commands only: the SFTP subsystem is off |
| Nextcloud, untrusted | 35.0.3 | 18373 | Nextcloud behind a Caddy certificate whose CA is never exported |
| Nextcloud, trusted CA | 35.0.3 | 18374 | The same Nextcloud behind Caddy's internal CA, which `ca` exports |
| WebDAV | Apache 2.4.71 `mod_dav` | 18375 | Basic authentication, user `putgd-dav`, HTTPS from the same CA |
| Public files, HTTPS | Caddy 2 | 18376 | `/sftp/<file>` serves the SFTP upload folder and `/webdav/<file>` the WebDAV folder, no login |
| Public files, HTTP | Caddy 2 | 18377 | The same two locations over plain HTTP |

Ports 18378 and 18379 are free for the next server; 18361 to 18363 belong to the
[HTTP receivers](http-receivers.md). Images are pinned by digest in `servers.py`; the OpenSSH
image is built from a pinned Alpine base on the builder. The servers hold synthetic files only.

## Start, check, stop

Run on the builder that hosts the servers, from a checkout or a copy of the folder:

```sh
python3 tools/lab/storage_servers/servers.py up --bind 192.168.68.71
python3 tools/lab/storage_servers/servers.py status --bind 192.168.68.71
python3 tools/lab/storage_servers/servers.py env --bind 192.168.68.71     # the file the harness sources
python3 tools/lab/storage_servers/servers.py check --bind 192.168.68.71   # 61 checks, see below
python3 tools/lab/storage_servers/servers.py down
```

`down --purge` also removes the volumes, the built image, the keys and the passwords.

- `--bind` is the one address the servers listen on: the builder's lab-network address,
  or `127.0.0.1`. An unspecified or public address is refused. Servers that already run on
  another address are refused too: run `down` first.
- `up` creates two pods, `putgd-st-sftp` and `putgd-st-web`, waits until each service answers,
  finishes Nextcloud's first-run install and settings, makes the user `putgd`, exports the CA
  and writes `known_hosts` and the env file. A second `up` changes nothing that exists.
- State is in `~/putgd-lab/storage-servers` (mode 0700). `client/` holds what a test reads:
  the key pairs, the passphrase, every password and `known_hosts`, all mode 0600.
  `server/` holds what the containers read: host keys, configuration, the WebDAV password
  hash. `ca/putgd-lab-root.crt` is the exported root certificate (public).
- Secrets are in files, never on a command line. `status` prints which exist, masked.
- `env` writes `storage-servers.env` (mode 0600): addresses, ports, user names and the paths
  of the key, passphrase, password and CA files. It holds no secret value, so a test sources it
  and reads each file inside the process that uses it.

## What each subcommand changes

| Command | Effect |
| --- | --- |
| `rotate-hostkey` | New ed25519 and RSA host keys for both SSH servers, which restart. `client/known_hosts` keeps the old key on purpose, so the next connection sees "host key changed". `known-hosts --bind ADDRESS` pins the new one |
| `ca --bind ADDRESS` | Writes the trusted Caddy's root certificate to `ca/putgd-lab-root.crt` and prints its SHA-256 fingerprint |
| `quota 5MB` / `quota none` | The Nextcloud quota of user `putgd` (also `500KB`, `2GB`) |
| `sharing on` / `sharing off` | Nextcloud's `shareapi_enabled`; off answers the share API with 404 "Share API is disabled" |
| `share-password on` / `off` | Nextcloud's `shareapi_enforce_links_password`; on refuses a public link without a password (403) |

## What `check` proves

`check.py` (run through `servers.py check`) drives every server the way a desktop test would and
prints one line per check, never a secret. It exits 0 when all 61 pass; its screenshots go to
`~/putgd-lab/storage-servers/checks/`.

- SFTP: sign in with the plain key, with the passphrase key through an `ssh-agent`, and with the
  password; each user refuses the other's method; `/ro` write is `Permission denied`; `/full`
  write fails; the chroot shows only the three folders; port 18372 refuses the subsystem but
  still gives a shell; after `rotate-hostkey` the old pin is refused and the new one works.
- WebDAV: a PUT without login is 401; PUT 201, then 204 over it; `If-None-Match: *` is 201 on a
  new name and 412 on an existing one; MKCOL, PROPFIND, GET, DELETE.
- Nextcloud: the exported CA verifies the trusted listener and not the untrusted one; Login Flow
  v2 (`POST /index.php/login/v2`, sign in and grant in headless Chromium, poll) ends with an app
  password; a WebDAV PUT with it, `If-None-Match: *` 412, a public share link that loads in curl
  and Chromium, `share-password`, `sharing` and `quota` taking effect at once.
- Public files: `/sftp/<file>` and `/webdav/<file>` over HTTPS and HTTP load in curl and in
  headless Chromium; a missing or deleted file is 404.

Use the files from a test like this:

```sh
. ~/putgd-lab/storage-servers/storage-servers.env
# SFTP, plain key:   $PUTGD_LAB_SFTP_KEY_USER @ $PUTGD_LAB_STORAGE_HOST : $PUTGD_LAB_SFTP_PORT, key $PUTGD_LAB_SFTP_KEY_FILE
# Nextcloud:         $PUTGD_LAB_NEXTCLOUD_URL, user $PUTGD_LAB_NEXTCLOUD_USER, CA $PUTGD_LAB_CA_FILE
# WebDAV:            $PUTGD_LAB_WEBDAV_URL, public link base $PUTGD_LAB_WEBDAV_PUBLIC_URL
```

Copy `ca/putgd-lab-root.crt` to a desktop that should trust the lab CA. Copy the key and password
files only to the desktop under test, into a folder of mode 0700, and do not print them.

## Traps

- **OpenSSH answers a full disk with a generic `Failure`.** `sftp-server` has no "no space"
  status: a write to `/full` fails as `write remote ... Failure`. The client can tell a full
  disk apart only through the `statvfs@openssh.com` extension (`df /full` reports 0 available).
  The folder is filled at start so every write fails; recreating the container refills it.
- **Every SSH client reaches the pod from one address.** OpenSSH 9.8 penalises a source address
  that fails authentication, which would delay the next test by seconds, so the configuration
  sets `PerSourcePenalties no`.
- **An `ssh-agent` key needs its public file with `IdentitiesOnly=yes`.** `IdentitiesOnly` also
  limits the agent; `-i key.pub` names the agent key to offer.
- **The passphrase reaches `ssh-keygen` through `SSH_ASKPASS` with `SSH_ASKPASS_REQUIRE=force`,**
  from the passphrase file, never `-N`.
- **A Caddy that serves HTTPS also opens port 80 for its redirects,** which is Nextcloud's port in
  the same pod: both Caddyfiles say `auto_https disable_redirects`. Admin is off because two
  Caddy instances in one pod would both claim port 2019. `default_sni` names the bind address
  because an IP client sends no server name and the pod's own address is not on the certificate.
- **Nextcloud's APCu cache served old sharing settings** after an `occ` change, so `sharing off`
  did nothing for minutes. The recipe overwrites the image's `apcu.config.php` with an array
  cache, so every `occ` change applies on the next request.
- **Apache's Alpine image has no DBM driver,** so `mod_dav` answers 500 ("lock database could not
  be opened"). The Debian image has it. Apache runs as uid 1; the WebDAV volume is chowned to that
  user with `podman unshare` after the pods start, because Podman resets a new volume's owner when
  its first container starts.
- **SELinux labels follow the bind mount.** A `:Z` mount of the whole state folder (as the
  Playwright step of the receivers does) relabels the servers' files and every container loses
  them. The browser here mounts only `checks/` and one password file. Servers mount `server/`
  with `:z`, and an unchanged file is never rewritten, because a single-file mount follows the
  inode a rewrite replaces.
- **Login Flow v2 needs the browser to follow redirects** across a "Log in", a sign-in form and a
  "Grant access" page; `check.py` loops over whichever it sees. `trusted_domains` holds
  `ADDRESS:18373`, `ADDRESS:18374` and the bare address, and `overwriteprotocol` is `https`;
  without those the login URL names the wrong host or Nextcloud answers "untrusted domain".
- **Nextcloud's front page is slow without APCu** (several seconds); `status` probes `/status.php` instead.
- **Brute-force protection is off** so a test that fails a sign-in on purpose does not lock the
  lab's own address out.
