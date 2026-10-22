# DEB/RPM channel moves from the command line: lab acceptance, 2026-10-22 (#386)

This is the lab acceptance for [#428](https://github.com/patrick-hudson/put.gd-desktop/pull/428), the DEB/RPM part of [#386](https://github.com/patrick-hudson/put.gd-desktop/issues/386). It follows Patrick's decision ([revised design](https://github.com/patrick-hudson/put.gd-desktop/issues/386#issuecomment-6349320117)):

- put.gd shows the channel the system's repository follows, without a choice;
- it lists the commands that move to the other channel;
- it never runs them or changes the repository file.

**What had to be shown, on m74 (apt) and f44 (dnf):**
- Settings shows the channel read-only, with the instructions.
- Following them by hand moves nightly → stable, the older case, and the system updater then takes stable releases.
- A later package upgrade keeps the chosen channel.
- Moving back to nightly works the same way.

Everything ran on the builders. Lab objects went to `pkg.put.gd/lab/`, and the stable side was signed with a lab-only key. Afterwards every lab object was deleted and the lab keys were shredded. The live feeds, the live suites and the real stable key were not touched.

## Builds under test

The PR head is `434f17a6`. One local lab-key commit `63bfed0d` replaces `packaging/linux/repo/keys/putgd-stable.{asc,gpg}` with the lab stable key's public halves (GPG `E6CD0DE47D189AFC84F96CD0E87D9607134941AB`). So every package's stable repository file trusts the lab key, never the real one.

Four empty commits follow, with fixed identities and dates, so m74 and f44 made the same commits. Their tags are local and were never pushed. [lab386-build.sh](lab/lab386-build.sh) built them on the target hosts as release builds:

| Build | Commit | Role | ubuntu24.04 DEB (m74) | fedora44 RPM (f44) |
| --- | --- | --- | --- | --- |
| `v26.1019.988-nightly` | `6eace23e` | nightly N | `4c8d8541…` | `10daf7b0…` |
| `v26.1018.989` | `734c9c7b` | stable S1, older than N | `2943a3d2…` | `4b11f03a…` |
| `v26.1020.990` | `b944c49d` | stable S2 | `f2dbc2fb…` | `ba3996c1…` |
| `v26.1021.991-nightly` | `b8ce1446` | nightly N1 | `de0e34c9…` | `5d1d8e0d…` |

The full hashes are in [results/m74/built.txt](results/m74/built.txt) and [results/f44/built.txt](results/f44/built.txt).

**Two runs overlapped on f44.** A second build run started in the same tree while the first was still building. Its packages were discarded, and f44 was rebuilt from an empty build directory before anything was published. The hashes above are from that clean build.

**Package contents checked before publishing:**
- **DEB:** each has `/usr/share/putgd/repo/putgd-{nightly,stable}.sources`, identical across the four packages. Each conffile equals its own channel's copy.
- **RPM:** each has `putgd-{nightly,stable}.repo` and `RPM-GPG-KEY-putgd-{nightly,stable}`. The stable key equals the lab key.

## Lab publication

[lab386.py](lab/lab386.py) ran on d69 from #428's `pkgrepo.py`. It used its own state directory and read the live signer's configuration by path, as in #373. Only `ubuntu24.04` and `fedora44` were published.

| Publication | Path | Signed |
| --- | --- | --- |
| `v26.1019.988-nightly`, then `v26.1021.991-nightly` | `publish_nightly` (lab scope) | nightly key, as in #293, #343 and #373 |
| `v26.1018.989`, then `v26.1020.990` | `stage_stable` and `publish_stable`, the real stable path. The workstation signing step was done on d69 with the lab key. | lab stable GPG and minisign keys |

- **Dry runs first:** 16 nightly and 12 stable keys, all under `lab/`.
- **Receipts:** [results/d69/receipts](results/d69/receipts).
- **The lab before:** #314 had just used the lab and cleaned it up. `lab/update/v1/nightly.json` and `lab/apt/…/dists/nightly/InRelease` returned 404 before the first publication.

## m74: Linux Mint 22.3 Cinnamon X11, apt and Update Manager

The installed `/usr/bin/putgd` ran with a private profile, reading the lab feeds through `PUTGD_UPDATE_FEED_URL`, and was driven by [acc386.py](lab/acc386.py). Each command was copied with Settings' **Copy** button and read back from the clipboard ([to stable](results/m74/copied-to-stable.txt), [to nightly](results/m74/copied-to-nightly.txt)). Over SSH they ran with `sudo -n`, and `apt install` became `apt-get install -y`: the same resolver, without the prompt.

| Step | Result |
| --- | --- |
| `apt-get install ./putgd-26.1019.988-nightly-…deb` | 1 package. `putgd.sources` equals `putgd-nightly.sources`; apt reads the lab nightly suite. |
| Settings | Channel *Nightly*, read-only, *"Set by your system's put.gd repository, /etc/apt/sources.list.d/putgd.sources."* The status is *Up to date*, and **Other channel** shows the three apt commands. `channelSource: "repository"`. |
| The first two copied commands only (the wait path) | apt verified the lab stable `InRelease` with the key inside the copied file. The candidate stayed `26.1019.988~nightly`, because stable 26.1018.989 is older. Settings: *"Your system now updates put.gd from the stable repository. Stable 26.1018.989 is older than this nightly build…"*, with **Or in a terminal** `sudo apt install --allow-downgrades putgd/stable`. |
| The third command | `DOWNGRADED: putgd` to 26.1018.989. There was no conffile prompt, and dpkg now records the stable copy (`3a869116…`) as the shipped conffile. Settings: Stable, up to date. |
| S2 published, then **Check now** | *"put.gd 26.1020.990 is available from put.gd's stable repository."* **Open software updates** opened Update Manager, which listed `putgd 26.1020.990`. |
| `apt-get install --only-upgrade putgd` | 26.1018.989 → 26.1020.990 from the stable suite. No prompt, and the file is still the stable copy. Update Manager's own **Install Updates** wasn't pressed, because it would have installed all 11 updates. |
| N1 published, then the copied "to nightly" commands | 26.1020.990 → 26.1021.991~nightly, an ordinary upgrade. No prompt, and the conffile's md5 is back to the nightly copy's. |
| `Enabled: no` added by hand | `channelSource: "setting"` and the `download` action. The Channel choice is back, and the instructions are gone. |
| `apt-get purge putgd` | The package and `putgd.sources` are gone, and `apt update` no longer reads pkg.put.gd. |

## f44: Fedora 44 GNOME Wayland, dnf and GNOME Software

The setup was the same as on m74. Clicks and keys went through `tools/lab/host_input.py`, a transient uinput agent, and [inp.py](lab/inp.py). The clipboard was read with `wl-paste` ([to stable](results/f44/copied-to-stable.txt), [to nightly](results/f44/copied-to-nightly.txt)). `dnf` ran with `-y`.

| Step | Result |
| --- | --- |
| `dnf install ./putgd-26.1021.991-nightly-…rpm` | putgd plus its 3 declared dependencies. `putgd.repo` equals `putgd-nightly.repo` (`gpgkey=…RPM-GPG-KEY-putgd-nightly`, SELinux `system_conf_t`). |
| Settings | Channel *Nightly*, read-only, naming `/etc/yum.repos.d/putgd.repo`. **Other channel** shows the two dnf commands. |
| The first copied command only | `putgd.repo` equals `putgd-stable.repo`, still `system_conf_t`. Settings: stable 26.1020.990 is older; the command is `sudo dnf distro-sync --refresh putgd`. |
| `dnf distro-sync --refresh putgd` | Downgraded 26.1021.991 → 26.1020.990 from `putgd-stable`. dnf imported the lab stable key from `RPM-GPG-KEY-putgd-stable`, the per-channel key file #428 adds. |
| `dnf downgrade putgd-26.1018.989`, then **Open software updates** | GNOME Software, under **System Updates**: `putgd 26.1018.989-1.fc44 → 26.1020.990-1.fc44`. GNOME Software doesn't list put.gd as an app; its AppStream catalog is #314. |
| `dnf upgrade putgd` | 26.1018.989 → 26.1020.990 from `putgd-stable`. The `.repo` file is still the stable copy. |
| The copied "to nightly" commands | 26.1020.990 → 26.1021.991 from `putgd-nightly`, an upgrade. |
| `dnf remove putgd` | putgd and its 3 dependencies are removed, and so is `putgd.repo`. The lab key was removed from rpmdb. |

## Pictures

All are full desktops from the hosts' wall VNC servers. Each footer names the build, and [lab/annotate386.py](lab/annotate386.py) drew the annotations.

**m74:**

![m74: nightly DEB, channel follows putgd.sources](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/m74-1-nightly-repository.png)
![m74: moved to stable by hand, older](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/m74-2-stable-repository-older.png)
![m74: stable installed](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/m74-3-stable-installed.png)
![m74: Update Manager offers the next stable](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/m74-4-update-manager.png)
![m74: back on nightly](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/m74-5-back-on-nightly.png)
![m74: repository disabled, the choice is back](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/m74-6-repository-disabled.png)

**f44:**

![f44: nightly RPM, channel follows putgd.repo](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/f44-1-nightly-repository.png)
![f44: moved to stable by hand, older](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/f44-2-stable-repository-older.png)
![f44: a stable update from the stable repository](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/f44-3-stable-update-available.png)
![f44: GNOME Software lists putgd](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/f44-4-gnome-software.png)
![f44: back on nightly](https://raw.githubusercontent.com/patrick-hudson/put.gd-desktop/2b0367ae90ef24511aff54e6fa43a14660eb444e/docs/evidence/2026-10-22-deb-rpm-channel-386/pictures/f44-5-back-on-nightly.png)

## dpkg conffile behaviour

[i386-conffile-probe.sh](lab/i386-conffile-probe.sh) ran on d69 with dpkg 1.22.22, in a throwaway `--root`:

| Case | Result |
| --- | --- |
| A same-channel upgrade, with the copied file in place | The copied file is kept, with no prompt. |
| A cross-channel upgrade that ships exactly the copied bytes | Taken silently, and recorded as the shipped copy. |
| A file that differs from both copies | The conffile prompt appears. |

So the templates are frozen: `tests/releases/test_repository_files.py` pins their SHA-256 values. The nightly ones equal the conffiles in the published v26.1022.968-nightly DEBs, checked on m74.

## Cleanup

| Where | What was removed or restored | Record |
| --- | --- | --- |
| d69: the lab | All 50 objects of this run, plus the receipts. The bucket's `lab/` prefix then listed 0 objects, and every key returned 404 over `pkg.put.gd` after invalidation `I5K3HJH6GSFH21G4FLGU8FH4GL`. Live `update/v1/{nightly,stable}.json` returned 200. | [clean.log](results/d69/clean.log), [404-check.txt](results/d69/404-check.txt) |
| d69: lab keys | Shredded, after `gpgconf --kill all` for their GNUPG home. The lab tree is removed. | |
| m74 and f44 | putgd is purged or erased with its repository file. The lab stable key is removed from f44's rpmdb, the input agent is stopped and its socket removed, and both desktop locks were released with `--verified-clean`. | |
| m74 | A stray `~/.local/share/putgd` from the first launch, before the driver had a private `XDG_DATA_HOME`, was removed before the release. | |
| m74 | Update Manager's first-run welcome screen was answered with OK. | |
