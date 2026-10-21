# Updates

How put.gd finds out about a newer version. This page covers phases 0 and 1 of the
design in [issue #293](https://github.com/patrick-hudson/put.gd-desktop/issues/293). The
later phases are listed at the end: the signed apt and dnf repositories, the AppImage's
own **Install update**, and Flatpak ([#83](https://github.com/patrick-hudson/put.gd-desktop/issues/83)).

Each install format updates the way that format normally does. put.gd's part is a small,
private check that tells you, and nothing is installed without you.

## The daily check

Once a day the resident asks app.put.gd for the newest version on its channel:

- **When it runs:** the first check is 5 to 15 minutes after put.gd starts. Later
  checks are 24 hours after the last attempt, give or take 2 hours. The last attempt is
  kept in `update-check.json` in the state folder (`$XDG_STATE_HOME/putgd`, normally
  `~/.local/state/putgd`), so a restart doesn't check again early. A failed check
  is logged and tried again the next day.
- **Where it runs:** the resident runs `putgd-cli --check-update CHANNEL` as a
  short-lived child. The resident itself never loads Qt Network or TLS, so its idle
  memory doesn't change.
- **What it sends:** one `GET https://app.put.gd/update/v1/<channel>.json`. There is no
  query string, cookie, version, locale or installation ID. The `User-Agent` is
  `put.gd-update-check/1`, and the request is the same from every computer.
  app.put.gd keeps no access logs.
- **Where the comparison happens:** on this computer. The links the result offers are
  built from the release tag (the GitHub release page and that package's download), so
  the feed can only choose which of the project's own release files to offer.

## Who is told

| Build | Channel it follows | Notification | Tray menu | Settings and main window |
| --- | --- | --- | --- | --- |
| Stable release | stable | once per new version | **Update available: X.Y.Z** | yes |
| Release candidate | stable | once per new version | yes | yes |
| Nightly | nightly | never | no | yes |
| Development or source build | none | no check | no | says why |
| Flatpak | none: Flatpak updates itself | no check | no | says why |

A version you already saw after **Check now** isn't announced again. The notification
also follows **Show a notification when a link is ready**.

![Settings → General → Updates and the main window rail on a nightly build, with a newer nightly available](evidence/2026-10-21-updates-293/settings-update-available-annotated.png)

![The Updates section when up to date, after a failed check (next check a day later) and on a development build, which never checks](evidence/2026-10-21-updates-293/update-states-annotated.png)

These pictures come from a tagged `v0.1.1-nightly.783` and `.784` build
(`PUTGD_PACKAGE_TARGET=fedora44`) on f44, and a development build on d69. Both ran in a
private Xvfb and profile against a local feed server set with `PUTGD_UPDATE_FEED_URL`.
The resident's TLS and Qt Network library mappings were the same before and after
**Check now**.

## Settings

**Settings → General → Updates**:

- **Check for updates once a day:** on by default.
- **Channel:** *This build's channel* (the default), *Stable* or *Nightly*.
- **Status:** the last result, when it was checked and when the next check runs, with
  **Check now**.
- **Download** and **Release notes:** shown when there is a newer version.
  **Download** opens this installation's package (the same DEB, RPM or AppImage you
  installed) in your browser.

When a newer version exists, the main window's rail shows **Update available: X**
under the version, which opens this section.

In the configuration file this is `preferences.updates`
([configuration](configuration.md)):

```json
"preferences": {"updates": {"check": true, "channel": "build"}}
```

Both keys are optional. Without the object, put.gd checks daily on this build's channel.

## CLI and MCP

| Interface | What it does |
| --- | --- |
| `putgd-cli update.status` | Returns `build` (version, kind, channel, packageTarget, supported, reason), `check`, `channelSetting`, `channel`, `enabled`, `notifies`, `checking`, `attemptedAt`, `checkedAt`, `nextCheckAt`, `result` and `error` |
| `putgd-cli update.check` | Checks now, even with daily checks off, and returns the same object. A failed check returns its error: `update_feed_unavailable`, `update_feed_invalid`, `update_check_failed` or `update_check_unavailable` |
| `putgd-cli --update-info` | This build's channel and package target. No resident and no network |
| `putgd-cli --check-update nightly` | The resident's child mode: one feed request and one JSON line. No resident needed |
| MCP `get_update_status`, `check_for_updates` | `update.status` and `update.check` |
| `settings.patch` / MCP `patch_configuration` | `{"preferences": {"updates": {"check": false}}}` and so on |

`PUTGD_UPDATE_FEED_URL` replaces `https://app.put.gd/update/v1/` for tests and the lab.
It must be `https`, or `http` on a loopback address.

## Feed contract v1

`https://app.put.gd/update/v1/stable.json` and `nightly.json`, published by the
put.gd-site deploy. The deploy runs within about a minute of each release.

```json
{
  "schema": 1,
  "channel": "nightly",
  "generatedAt": "2026-10-20T18:32:24Z",
  "release": {
    "tag": "v0.1.1-nightly.588",
    "version": "0.1.1-nightly.588",
    "publishedAt": "2026-10-20T05:55:50Z",
    "notesUrl": "https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v0.1.1-nightly.588",
    "assets": {
      "debian13": {"name": "putgd-0.1.1-nightly.588-debian13-x86_64.deb", "url": "https://github.com/…", "size": 52732030, "sha256": "…"}
    }
  }
}
```

- **Assets:** keyed `ubuntu24.04`, `ubuntu26.04`, `debian13`, `fedora44` and `appimage`.
- **No release:** `release` is `null` while a channel has none. Today that's stable.
- **Version order:** within one base version, `X.Y.Z-nightly.N` < `X.Y.Z-rc.N` < `X.Y.Z`.
- **Checks the client makes:** a stable feed may name only stable versions and a
  nightly feed only nightlies. The tag must be `v` + version. Any other `schema` is
  rejected, and so is a feed over 256 KiB.
- **Download link:** an asset whose `url` isn't the GitHub release download for that
  tag and name is replaced by `https://app.put.gd/dl/<channel>/<target>`.

## Builds and packages

Two CMake cache variables decide whether a build checks:

- **`PUTGD_PACKAGE_TARGET`:** which published package this build becomes:
  - `ubuntu24.04`, `ubuntu26.04`, `debian13` or `fedora44`, set by `package.yml` from the
    matrix name;
  - `appimage`, set by `tools/releases/build-appimage.sh`;
  - `flatpak`, set in `packaging/flatpak/gd.put.Putgd.yml`;
  - empty for development and source builds, which never check.

  `native.py --package-target` makes the native audit compare it with
  `putgd-cli --update-info`, and the AppImage build checks it too.
- **`PUTGD_UPDATE_CHECK`:** default `ON`. A distribution that packages put.gd itself sets
  it `OFF`; Settings then says checks were turned off for this build.

Only clean tagged builds (stable, candidate or nightly) have a channel. Every other
build reports `development_build` or `no_package` and never checks.

## Later phases (#293)

- **Phase 2:** signed apt and dnf repositories at `pkg.put.gd`, which the DEB and RPM
  add when installed. Settings will then point to your software updater instead of a
  download.
- **Phase 3:** the AppImage's own **Install update**. It replaces the file in place after
  checking a minisign signature.
- **Phase 4:** Flatpak, from its own signed repository, in [#83](https://github.com/patrick-hudson/put.gd-desktop/issues/83).
- **Windows:** waits for a published Windows build.
