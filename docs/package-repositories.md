# Package repositories (pkg.put.gd)

put.gd's DEB and RPM update through apt and dnf like any other package. Each tagged
package adds put.gd's signed repository for its channel, so `apt upgrade`, `dnf upgrade`,
Update Manager, Discover and GNOME Software install new versions. This is phase 2 of
[#293](https://github.com/patrick-hudson/put.gd-desktop/issues/293). The update check in
[updates.md](updates.md) then points to the system's updater instead of a download.

## What the packages add

Only tagged builds (nightly and stable, and the pre-#373 candidate) add a repository, and each trusts only
its own channel's key. Candidates follow stable. Development and source builds add
nothing.

| Package | File | Notes |
| --- | --- | --- |
| DEB (`ubuntu24.04`, `ubuntu26.04`, `debian13`) | `/etc/apt/sources.list.d/putgd.sources` | A dpkg conffile. `Signed-By` holds the channel's public key inline, so a removed but not purged package still leaves a source apt can verify. The key is also at `/usr/share/keyrings/putgd-archive-keyring.gpg`. |
| RPM (`fedora44`) | `/etc/yum.repos.d/putgd.repo` | Copied in on the first install only, from `/usr/share/putgd/repo/putgd.repo`, and declared `%ghost %config(noreplace)`. The key is `/etc/pki/rpm-gpg/RPM-GPG-KEY-putgd`. |

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
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-putgd
metadata_expire=6h
```

**Turning it off:**
- **apt:** delete the file, or add `Enabled: no`. Upgrades leave either change alone, and `apt purge putgd` removes the file.
- **dnf:** delete the file, or set `enabled=0`. Upgrades leave either change alone, and `dnf remove putgd` removes the file.

The packages also install AppStream metadata, `/usr/share/metainfo/gd.put.Putgd.metainfo.xml`.

## Layout

| Path | Contents | Signed by |
| --- | --- | --- |
| `apt/<label>/dists/<channel>/` | `InRelease`, `Release`, `Release.gpg`, `main/binary-amd64/Packages*` | the channel key (`InRelease`, `Release.gpg`) |
| `apt/<label>/pool/main/p/putgd/` | the DEBs, byte-identical to the GitHub assets | through `InRelease` |
| `rpm/fedora/44/x86_64/<channel>/` | `Packages/*.rpm` and `repodata/` | each RPM, with `rpmsign` and the channel key |
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
3. **Build the next state** in a copy of the workspace: reprepro for apt, and `createrepo_c` for dnf, with the RPM signed by `rpmsign`. The signed RPM must verify against the channel key only, and its digests must match the GitHub build.
4. **Upload in order:** packages, then indexes, then `InRelease` and `repomd.xml`.
5. **Invalidate and read back.** It invalidates what changed, then reads `InRelease`, `Packages` and `repomd.xml` back through `https://pkg.put.gd`.
6. **Write the feed.** Only after that readback does it write and invalidate `update/v1/<channel>.json`, on pkg.put.gd and app.put.gd. The feed never names a version apt or dnf can't fetch yet.
7. **Leave a receipt,** in `~/.local/state/putgd-signer/receipts/` and at `receipts/<tag>.json`.

Objects that leave the repository are deleted an hour later, after every cached `InRelease` or `repomd.xml` that names them has expired. The bucket is versioned, so an older object version can be restored for 14 days.

### Setup on the builder

```sh
sudo apt-get install reprepro createrepo-c rpm minisign python3-boto3
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

The signer's next run checks them against the stable public keys, regenerates the repodata and publishes. The workstation needs `gpg`, `minisign`, `rpmsign` and `gh`.

**Once, before the first stable package exists,** create empty stable repositories so a stable package never points at a missing suite:

```sh
python3 ~/putgd-signer/src/tools/releases/pkgrepo.py init-stable   # on d69
python3 tools/releases/sign_stable.py --empty                      # on the workstation
```

## Acceptance (2026-10-21)

Lab-scope and live evidence (real nightlies 816 → 828 on all four desktops, 2026-10-21) is in [evidence/2026-10-21-pkg-repos-293](evidence/2026-10-21-pkg-repos-293/README.md).
