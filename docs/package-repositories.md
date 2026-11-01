# Package repositories (pkg.put.gd)

put.gd's DEB and RPM update through apt and dnf like any other package. Each tagged
package adds put.gd's signed repository for its channel, so `apt upgrade`, `dnf upgrade`,
Update Manager, Discover and GNOME Software install new versions. This is phase 2 of
[#293](https://github.com/patrick-hudson/put.gd-desktop/issues/293). The update check in
[updates.md](updates.md) then points to the system's updater instead of a download.

## What the packages add

Only tagged builds (nightly and stable) add a repository, and each trusts only
its own channel's key. Development and source builds add
nothing.

| Package | File | Notes |
| --- | --- | --- |
| DEB (`ubuntu24.04`, `ubuntu26.04`, `debian13`) | `/etc/apt/sources.list.d/putgd.sources` | A dpkg conffile. `Signed-By` holds the channel's public key inline, so a removed but not purged package still leaves a source apt can verify. The key is also at `/usr/share/keyrings/putgd-archive-keyring.gpg`. Both channels' files are in `/usr/share/putgd/repo/putgd-{nightly,stable}.sources` ([switching](#switching-channels)). |
| RPM (`fedora44`) | `/etc/yum.repos.d/putgd.repo` | Copied in on the first install only, from `/usr/share/putgd/repo/putgd.repo`, and declared `%ghost %config(noreplace)`. Both channels' files are in `/usr/share/putgd/repo/putgd-{nightly,stable}.repo`, and each names its own key, `/etc/pki/rpm-gpg/RPM-GPG-KEY-putgd-{nightly,stable}`. `RPM-GPG-KEY-putgd` holds the package's own channel key, for files written before [#386](https://github.com/patrick-hudson/put.gd-desktop/issues/386). |

```ini
# /etc/apt/sources.list.d/putgd.sources (nightly, Ubuntu 24.04 and Mint 22)
Types: deb
URIs: https://pkg.put.gd/apt/ubuntu24.04
Suites: nightly
Components: main
Architectures: amd64
Signed-By:
 -----BEGIN PGP PUBLIC KEY BLOCK-----
 …
```

```ini
# /etc/yum.repos.d/putgd.repo (nightly)
[putgd-nightly]
name=put.gd (nightly)
baseurl=https://pkg.put.gd/rpm/fedora/44/x86_64/nightly
enabled=1
gpgcheck=1
repo_gpgcheck=0
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-putgd-nightly
metadata_expire=6h
```

**Turning it off:**
- **apt:** delete the file, or add `Enabled: no`. Upgrades leave either change alone, and `apt purge putgd` removes the file.
- **dnf:** delete the file, or set `enabled=0`. Upgrades leave either change alone, and `dnf remove putgd` removes the file.

The packages also install AppStream metadata, `/usr/share/metainfo/gd.put.Putgd.metainfo.xml`.

## Switching channels

put.gd doesn't change your repository file. A DEB or RPM follows the channel its repository file
names, and you move it to the other channel yourself, in a terminal
([#386](https://github.com/patrick-hudson/put.gd-desktop/issues/386); Patrick decided against
a password prompt inside put.gd).

**In Settings → Updates**, while the repository file is present and enabled:
- **Channel** shows that file's channel. You can't change it there, and the line under it says
  *"Set by your system's put.gd repository, /etc/apt/sources.list.d/putgd.sources."*
- **Update checks** read that channel's feed, whatever the stored choice says. The stored choice
  isn't rewritten, so an AppImage that shares the configuration keeps its own.
- **Other channel** lists the commands below, with **Copy** and a link here.

**Nightly to stable:**

```bash
# apt (Mint, Ubuntu, Debian)
sudo install -m 644 /usr/share/putgd/repo/putgd-stable.sources /etc/apt/sources.list.d/putgd.sources &&
sudo apt update &&
sudo apt install --allow-downgrades putgd/stable
```

```bash
# dnf (Fedora)
sudo install -m 644 /usr/share/putgd/repo/putgd-stable.repo /etc/yum.repos.d/putgd.repo &&
sudo dnf distro-sync --refresh putgd
```

**Stable to nightly:** the same commands with `nightly` in place of `stable`.

**What the commands do:**
- **The first one** puts the package's copy of the other channel's repository file in place. It's
  byte for byte the file that channel's package installs, with that channel's key.
- **The last one** installs that channel's package even when it's older. apt needs
  `--allow-downgrades` for that, and `dnf distro-sync` downgrades on its own. dnf asks once to
  import the other channel's key from `/etc/pki/rpm-gpg/RPM-GPG-KEY-putgd-CHANNEL`.

**An older target.** Versions follow dates ([versioning.md](versioning.md)), so stable is usually
older than a nightly. You have two choices:
- **Install it now:** run all the commands.
- **Wait:** leave out the last command. Your system updater then keeps the nightly until a stable
  release newer than it is out, and installs that one as a normal update. Meanwhile Settings
  says *"Your system now updates put.gd from the stable repository. Stable X is older than this
  nightly build (Y), so the system updater keeps this version until a newer stable release is
  out."* It shows the one command that installs it now.

**No stable release yet:** the stable repositories exist but are empty. The last command reports
that there's nothing to install, and the nightly stays until the first stable release.

**Package upgrades keep the channel you chose:**
- **apt:** dpkg keeps the copied file through upgrades of the same channel. When the other
  channel's package arrives, it ships exactly that file, so dpkg takes it without a conffile
  prompt. Checked with dpkg 1.22.22 on d69 ([probe](evidence/2026-10-22-deb-rpm-channel-386/lab/i386-conffile-probe.sh))
  and on Mint 22.3.
- **dnf:** RPM never rewrites a `%ghost` file.
- **The conffile's bytes are pinned:** `tests/releases/test_repository_files.py` holds the
  SHA-256 values. Changing the template needs a migration in the package, or dpkg would prompt
  anyone who switched.

**From an agent:** `update.status` reports `channelSource: "repository"`. Its `action.otherChannel`
holds `{channel, commands, docs}`, and its `action.older` holds `{version, command}`. They're
instructions only: there's no method that switches a repository install. If you remove or disable
the repository file, the Channel choice comes back and put.gd offers **Download** again.

![Settings on a nightly DEB: the channel follows putgd.sources, and the commands that move to stable](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/m74-1-nightly-repository.png)

![After moving the repository to stable by hand: stable 26.1018.989 is older, so apt keeps the nightly until the last command](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/m74-2-stable-repository-older.png)

The pictures come from lab builds of [#428](https://github.com/patrick-hudson/put.gd-desktop/pull/428), on Linux Mint 22.3 and Fedora 44. The lab channels were under `pkg.put.gd/lab`, and the stable side was signed with a lab-only key ([evidence](evidence/2026-10-22-deb-rpm-channel-386/README.md)).

## AppStream catalog (#314)

The installed metainfo only lets a software centre show put.gd once it's installed. To let people find put.gd by searching and install it from there, each repository also carries an AppStream catalog. The signer builds it from the exact package that suite or repository publishes.

| Repository | Catalog | Read by |
| --- | --- | --- |
| apt (each label and channel) | DEP-11 in `dists/<channel>/main/dep11/`: `Components-amd64.yml` and `icons-{48x48,64x64,64x64@2,128x128,128x128@2}.tar`, each also as `.gz`. All are listed in `Release`, so `InRelease` covers them. Origin `putgd-<label>-<channel>`. | apt fetches them through the targets appstream's `/etc/apt/apt.conf.d/50appstream` declares, and `appstreamcli refresh` installs them in `/var/lib/swcatalog`. Discover and GNOME Software read them through AppStream. |
| dnf | `appstream` and `appstream-icons` (`<size>/gd.put.Putgd.png`) entries in `repomd.xml`, in the copr layout. | When GNOME Software refreshes, dnf5 installs them as `/var/cache/swcatalog/xml/<repo id>.xml` and `icons/<repo id>/`. |

- **Built from the published package.** `tools/releases/appstream_catalog.py` extracts only the package's metainfo, desktop files and icons. `appstreamcli compose` renders the catalog and the cached icons from the scalable SVG. The signer then sets the origin and `<pkgname>putgd</pkgname>` and refuses anything but exactly `gd.put.Putgd`. The apt catalog comes from the DEB that `Packages` names; the dnf catalog comes from the newest RPM and is rebuilt after every `createrepo_c` run.
- **Validated before use.** `appstreamcli validate` checks the catalog XML, and also the DEP-11 YAML after converting it back to XML.
- **Recorded.** Archives are deterministic, so an unchanged catalog isn't uploaded again. The receipt's `catalog` map records each origin, the package and its SHA-256, and the files written.
- **Both apt forms.** apt skips an optional index target unless `Release` lists its *uncompressed* name. So the signer lists and publishes both forms, as Ubuntu's archive does, and apt then fetches the `.gz`.
- **Signing.** reprepro can't list files it didn't write, so it no longer signs. The signer adds the catalog to `Release` and then signs the finished file: nightly with `gpg --clearsign` and `--detach-sign` (SHA512); stable on the workstation, unchanged.
- **Mint's Software Manager** doesn't read AppStream for apt packages: `mintcommon/installer/pkgInfo.py` `AptPkgInfo` reads apt and the icon theme only. It finds put.gd as "Putgd", shows the control file's summary, and shows a generic icon until the package is installed. [#430](https://github.com/patrick-hudson/put.gd-desktop/issues/430) adds an extended package description so it can show more text.
- **No screenshots yet.** Adding them to the metainfo is [#431](https://github.com/patrick-hudson/put.gd-desktop/issues/431).

## Layout

| Path | Contents | Signed by |
| --- | --- | --- |
| `apt/<label>/dists/<channel>/` | `InRelease`, `Release`, `Release.gpg`, `main/binary-amd64/Packages*`, `main/dep11/*` (the AppStream catalog) | the channel key (`InRelease`, `Release.gpg`) |
| `apt/<label>/pool/main/p/putgd/` | the DEBs, byte-identical to the GitHub assets | through `InRelease` |
| `rpm/fedora/44/x86_64/<channel>/` | `Packages/*.rpm` and `repodata/`, including `appstream` and `appstream-icons` | each RPM, with `rpmsign` and the channel key |
| `releases/<tag>/` | `SHA256SUMS` and `SHA256SUMS.minisig`; for the newest two releases per channel also `putgd-VERSION-x86_64.AppImage` and its `.zsync` (#293 phase 3) | the channel's minisign key (`SHA256SUMS`, which lists the image and `.zsync`) |
| `dl/<channel>/appimage.zsync` | the newest mirrored `.zsync`, served at `https://app.put.gd/dl/<channel>/appimage.zsync` | through `SHA256SUMS` |
| `update/v1/<channel>.json` | the update feed and its `.minisig` | the channel's minisign key |
| `receipts/<tag>.json` | what each publication verified, signed and uploaded | — |

- **Labels and channels:** `<label>` is `ubuntu24.04`, `ubuntu26.04` or `debian13`, and `<channel>` is `nightly` or `stable`.
- **Separate archives per label:** the three DEBs share a name and version but have different bytes, and reprepro keeps one pool per archive.
- **One suite per channel:** an `InRelease` covers its whole suite, so each channel gets its own and is signed by its own key.
- **Retention:** apt keeps the newest version, and dnf keeps the newest three. The newest two releases per channel keep their AppImage (`appimage_keep`); older images and `.zsync` files go after the usual delay, and `SHA256SUMS` stays.
- **AppImage mirror:** the image and `.zsync` upload with the packages, before any metadata, with immutable caching; the `.zsync` is read back through the CDN; `dl/<channel>/appimage.zsync` moves with the feed. A `.zsync` must describe exactly the image and name either the GitHub asset (releases before #293 phase 3) or its mirror copy. `publish-local` takes an AppImage-only lab directory for the lab.

**Why the dnf metadata isn't signed.** On Fedora 44, GNOME Software reads packages through dnf5daemon. dnf5daemon 5.4 can't load any repository with `repo_gpgcheck=1`; OpenAI's ChatGPT repository fails the same way. So the dnf repository follows Fedora's own model: signed packages (`gpgcheck=1`) and unsigned repodata. A forged repository can't install a package that isn't signed by the channel key. The RPM in the repository is the GitHub asset plus a signature; its header and payload digests are unchanged, and each receipt records both SHA-256 values. (Decided 2026-10-21 in #293.)

## Keys

| Key | Where it lives | Signs |
| --- | --- | --- |
| Nightly GPG `4032E28D893BAF66F4AC23D9A11D38D55536B371` | d69, `~/.local/share/putgd-signer/gnupg` (0700, no passphrase) | `InRelease`, `Release.gpg`, RPMs |
| Nightly minisign `71440FA5E439AEBB` | d69, `~/.local/share/putgd-signer/minisign/nightly.key` | `SHA256SUMS`, feed |
| Stable GPG `C0962EFA929F5A5477A4509C0CC9E5A0F0B6FC1C` | Patrick's workstation keyring (passphrase) | as nightly, at stable publication |
| Stable minisign `A4388FB41F96A6D7` | workstation, `~/.minisign/putgd-stable.key` (passphrase) | as nightly |

- **Public halves:** they're in `packaging/linux/repo/keys/`, as armored and binary GPG keys and minisign public keys.
- **Expiry:** the GPG keys expire on 2029-10-20. Extend them before then, and ship the new public key in a package update.
- **Secrets stay off GitHub:** no private key or credential is ever in GitHub, an argument, a log or a receipt.

## The signer (d69)

`tools/releases/pkgrepo.py` runs from a systemd user timer every ten minutes
(`packaging/signer/putgd-signer.{service,timer}`). Each run does the following:

1. **Find new releases.** It lists published releases through the anonymous GitHub API and takes the newest nightly and stable releases the repositories don't carry yet.
2. **Verify the release.** It checks:
   - that the tag resolves to the commit `release-builds.json` names;
   - that `SHA256SUMS` lists exactly the assets;
   - each DEB and RPM against `SHA256SUMS`, its `release-builds.json` receipt and GitHub's `digest`;
   - each package's own name, version and architecture.
3. **Build the next state** in a copy of the workspace: reprepro for apt, and `createrepo_c` for dnf, with the RPM signed by `rpmsign`. The signed RPM must verify against the channel key only, and its digests must match the GitHub build. Each suite and repository then gets its [AppStream catalog](#appstream-catalog-314), and the signer signs the finished `Release` (`InRelease`, `Release.gpg`).
4. **Upload in order:** packages, then indexes, then `InRelease` and `repomd.xml`.
5. **Invalidate and read back.** It invalidates what changed, then reads `InRelease`, `Packages` and `repomd.xml` back through `https://pkg.put.gd`.
6. **Write the feed.** Only after that readback does it write and invalidate `update/v1/<channel>.json`, on pkg.put.gd and app.put.gd. The feed never names a version apt or dnf can't fetch yet.
7. **Leave a receipt,** in `~/.local/state/putgd-signer/receipts/` and at `receipts/<tag>.json`.

Objects that leave the repository are deleted an hour later, after every cached `InRelease` or `repomd.xml` that names them has expired. The bucket is versioned, so an older object version can be restored for 14 days.

### Setup on the builder

```sh
sudo apt-get install reprepro createrepo-c rpm minisign python3-boto3 appstream appstream-compose
sudo loginctl enable-linger "$USER"
git clone https://github.com/patrick-hudson/put.gd-desktop.git ~/putgd-signer/src   # then: git -C ~/putgd-signer/src checkout nightly
install -d -m 700 ~/.config/putgd-signer
cp ~/putgd-signer/src/packaging/signer/putgd-signer.{service,timer} ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now putgd-signer.timer
```

The configuration lives in `~/.config/putgd-signer/` (0700):
- **`signer.json`:** the state, `gnupghome`, `minisign_key` and `keys` paths, both fingerprints, and `feed_distributions` (`["EP85P0MVCKASG"]` once app.put.gd serves the feed from the pkg bucket).
- **`aws-config`:** the profile `publish` assumes `putgd-pkg-publish` from the `signer` profile.
- **`aws-credentials`:** the access key of IAM user `putgd-pkg-signer`, whose only permission is to assume that role. Patrick writes this file himself.

The bucket, CloudFront distribution, certificate and roles are Terraform in
patrick-hudson/put.gd-site `infra/pkg.tf`.

### Operating it

```sh
systemctl --user list-timers putgd-signer.timer
journalctl --user -u putgd-signer.service -n 50
python3 ~/putgd-signer/src/tools/releases/pkgrepo.py status
```

- **Lab scope:** `pkgrepo.py publish-local --tag vYY.MDD.N-nightly --assets DIR` publishes locally built, tagged packages under `pkg.put.gd/lab/`. They're signed by the same nightly key, and nothing goes into the live paths. Lab packages are built with `-DPUTGD_PACKAGE_REPOSITORY_URL=https://pkg.put.gd/lab`.
- **Failures:** a verification failure writes `receipts/<scope>/<tag>.failed.json` and changes nothing. The next run retries, because a signer outage only delays repository updates.

## Stable publication

The stable key never leaves the workstation, so stable releases are signed in two steps.

**First, the signer stages the release.** When it sees a published stable release, it verifies it and builds the stable suites and repodata in `~/.local/state/putgd-signer/stage/live/stable/<tag>/`. It publishes nothing yet.

**Then, on the workstation, after `publish.py --publish`:**

```sh
python3 tools/releases/sign_stable.py --tag vYY.MDD.N
```

The script:
- fetches the staged `Release` files, `Packages`, `SHA256SUMS` and the feed (a few KiB) over the pinned SSH connection to d69;
- checks every package hash in them against the tag's `SHA256SUMS` from GitHub;
- downloads the fedora44 RPM from GitHub, checks it, and signs it with `rpmsign`;
- signs `InRelease`, `Release.gpg` and the minisign signatures. gpg-agent and minisign ask for their passphrases.
- copies only the signatures and the signed RPM back.

The signer's next run checks them against the stable public keys, regenerates the repodata (and its catalog, from the stable-signed RPM) and publishes. The workstation needs `gpg`, `minisign`, `rpmsign` and `gh`.

The staged `Release` already lists the DEP-11 catalog, so the stable signature covers it. `sign_stable.py` checks every package hash; the catalog is metadata the signer builds from those packages, and it can't install anything.

**Once, before the first stable package exists,** create empty stable repositories so a stable package never points at a missing suite:

```sh
python3 ~/putgd-signer/src/tools/releases/pkgrepo.py init-stable   # on d69
python3 tools/releases/sign_stable.py --empty                      # on the workstation
```

## Acceptance (2026-10-21)

Lab-scope and live evidence (real nightlies 816 → 828 on all four desktops, 2026-10-21) is in [evidence/2026-10-21-pkg-repos-293](evidence/2026-10-21-pkg-repos-293/README.md).

The AppStream catalog (#314) was tested in the lab on 2026-10-22: search, install and remove from Discover (k72), GNOME Software (f44) and Mint's Software Manager (m74). The evidence is in [evidence/2026-10-22-appstream-catalog-314](evidence/2026-10-22-appstream-catalog-314/README.md).
