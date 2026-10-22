# put.gd for Windows

The Windows build under `src/windows/` is a native Win32 implementation of the same
product: no Qt, no bundled runtime, no third-party libraries. It speaks the same
schema-1 JSON session protocol as the Linux resident, so `putgd-cli` and the MCP
helper work unchanged, and it keeps the same process boundaries so the resident
stays small ([memory strategy](memory-strategy.md)). It landed as draft pull request
[#405](https://github.com/patrick-hudson/put.gd-desktop/pull/405); the gaps it
declares are issues #400 to #404 and #406.

## Process model

| Process | Role | Built-in Windows APIs |
| --- | --- | --- |
| `putgd.exe` | Resident: tray icon and menu, global shortcuts, clipboard owner, journal, session pipe, helper supervision | Shell_NotifyIcon (toasts through `NIF_INFO`), RegisterHotKey, clipboard format listener, `System32\winsqlite3.dll`, named pipes, Credential Manager, the Run key |
| `putgd.exe --capture MODE` | Live region/window/desktop selector and pixel capture, PNG encode | layered windows, GDI `BitBlt` / `PrintWindow`, DWM, WIC |
| `putgd.exe --image-job` | Decode and normalize clipboard or file images, previews, DIB export | WIC |
| `putgd.exe --s3-worker` | One S3 request: SigV4 signing over WinHTTP, then exit | WinHTTP, BCrypt |
| `putgd.exe --ui VIEW` | History/Activity, Destinations, Settings and About window | common controls v6, custom drawing, `IFileOpenDialog`, uxtheme, DWM |
| `putgd.exe --edit-image` | Capture region & edit canvas | GDI+, WIC |
| `putgd-cli.exe` | JSON session client over the pipe | named pipes |
| `proton\putgd-proton-runtime.exe --stdio` | One Proton Drive request through the official Drive SDK (the Linux helper, cross-built for Windows) | Credential Manager through Bun.secrets, WinHTTP-free: Bun's own TLS stack |

The helper control protocol (bootstrap, ready, activate, heartbeat, checkpoints) is
specified in `src/windows/common/Protocol.h`. Helpers never create files: the
resident stages owner-only inputs and outputs and verifies byte counts and SHA-256
digests on both sides of a handoff; the editor's PNG must pass the same chunk
inspector as on Linux ([image editor](image-editor.md)).

The session endpoint is `\\.\pipe\putgd-<hash of the user SID>`, served with an
owner-only DACL. A second resident refuses to start, and `putgd.exe` launched again
forwards a `ui.show` request. Secrets live in Credential Manager as generic
credentials named `putgd/<reference>` (DPAPI under the user's logon, no unlock
prompt); session mode keeps them in resident memory only. Configuration is
`%LOCALAPPDATA%\put.gd\config.json`: the schema-1 document, content-hash revisions,
a cooperative `config.json.lock`, atomic replacement and a directory watcher with
the 75 ms debounce ([configuration](configuration.md)). The journal is
`history.sqlite` through the SQLite that ships in Windows; artifacts, previews and
capture/editor staging sit beside it as owner-only directories.

## Proton Drive

The resident registers the `proton-drive` provider ([Proton Drive](proton-drive.md)) when
`proton\putgd-proton-runtime.exe` next to `putgd.exe` matches the digest compiled in from
its provisioning manifest (`-DPUTGD_PROTON_MANIFEST=…/manifest.json`); otherwise
`capabilities` reports the provider unavailable with `proton_runtime_unavailable` and the
Destinations editor keeps its Add menu entry disabled. The helper is the same TypeScript
source as on Linux, cross-compiled on a Linux host with
`python3 tools/proton/provision_runtime.py --target windows-x64` (the locked
`@oven/bun-windows-x64` runtime, checks recorded in the manifest for running on Windows).
Each request is one child with a newline-JSON request on stdin, a private
owner-only data directory under `%LOCALAPPDATA%\put.gd\proton-data`, and a per-account
lock file under `proton-accounts` because the service rotates the refresh token; the
resident holds no Proton secret. The helper stores the session and link passwords in
Credential Manager through Bun.secrets, which refuses writes from a non-interactive
logon (an SSH key session, a service): the resident must run in the user's desktop
session. Sharing, link expiry, revocation, reconciliation and the retry deadline
adoption follow the Linux rules; the journal's `sharing_operations` table and the
`history.share`/`history.revokeLink`/`history.inspectLink`/`share.inspect` methods
are served on Windows. Shipping the runtime with a Windows build (CI provisioning,
Bun attribution, catalog, installer placement) is #408; Backblaze B2 on Windows is #409.

![The Proton destination editor](evidence/2026-10-22-windows-native/08-proton-editor-annotated.png)

![Connect account: the sign-in URL from the setup job opened in the default browser](evidence/2026-10-22-windows-native/09-proton-connect-annotated.png)

![Add destination offers Proton Drive when the helper is verified](evidence/2026-10-22-windows-native/10-proton-add-menu-annotated.png)

Verified on 2026-10-22 in the guest (`work/verification/2026-10-22-windows-native/proton/`),
first without an account: the helper's version, probe and stdio cases, a Bun.secrets round
trip through Credential Manager, the connect auth event within a second, a Proton profile
validated and saved, Test connection answering `proton_setup_required`, concurrent setup on
one account answering `proton_account_busy`, `history.share` on an S3 record answering
`sharing_unsupported`, and the S3 path unchanged. Then with Patrick signed in on the guest
display through the window's Connect account… (`live-results.md` there): account
inspection with the e-mail and folders, a folder created by the app, Test connection
(`account-folder`), a capture uploaded and shared with the link copied, inspect, revoke
(link gone, `linkAvailable` false), a new link created, a one-day expiry recorded as
`shareExpiresAt`, a stored link password used for a new link, a private upload with no
link and nothing copied followed by an explicit link, a 4-second deadline ending as
`proton_request_timeout` with the upload unknown and Retry adopting the longer current
deadline (the link step keeps the receipt's accepted deadline, as on Linux), a repeated
name renamed on collision, and a 120 MiB upload cancelled mid-transfer
(`proton_result_unknown`, reason `cancelled`) that Retry reconciled and uploaded. The
same session found and fixed three window repaint faults (a scrolled pane showing another
label's text, Settings combos still calling Proton unavailable, History rows blanked by a
one-row repaint).

![History with the Proton uploads and their link actions](evidence/2026-10-22-windows-native/11-history-proton-annotated.png)

![The connected destination, scrolled](evidence/2026-10-22-windows-native/12-proton-editor-connected-scrolled-annotated.png)

![The completion toast after a Proton upload](evidence/2026-10-22-windows-native/13-proton-toast-annotated.png)

Not verified: Proton's plan-size (200003) and quota refusals, a service-side revision change
racing a link mutation, and DPI above 100 %.

## Building

Cross-compile from Linux (what CI's `windows` job does, [ci.md](ci.md)):

```sh
sudo apt-get install -y g++-mingw-w64-x86-64-posix mingw-w64-tools ninja-build
cmake -S src/windows -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/x86_64-w64-mingw32.cmake"
cmake --build build/windows
```

The executables are static (libstdc++, libgcc and winpthread linked in) and import
only system DLLs. The build identity comes from the repository's calendar-version
generator ([versioning](versioning.md)), so `putgd.exe --version-json` reports the
same `YY.MDD.N` version as the Linux build of that commit. The portable core (JSON,
settings, naming, operation records, S3 rules, PNG inspector, SigV4, the editor
model) builds and tests on Linux:

```sh
cmake -S src/windows -B build/windows-host -DPUTGD_WINDOWS_PORTABLE_TESTS=ON
cmake --build build/windows-host && ctest --test-dir build/windows-host
```

The sources are MSVC-clean C++20 at `/W4` with the mingw build's warning set
(no unused-parameter or shadowing warnings). CI's `windows-msvc` job builds both
executables with Visual Studio 2022 and `/WX` ([ci.md](ci.md)); locally:

```sh
cmake -S src/windows -B build/msvc -G "Visual Studio 17 2022" -A x64
cmake --build build/msvc --config Release
```

## Packages and versions

`src/windows/packaging/package.py` turns a cross build into the two packages CI keeps
as the `windows-packages` artifact (wixl comes from Ubuntu's `wixl` package):

```sh
sudo apt-get install -y wixl msitools
python3 src/windows/packaging/package.py --build build/windows --output build/windows-packages
```

| File | Contents |
| --- | --- |
| `putgd-VERSION-windows-x86_64.zip` | `put.gd\` with `putgd.exe`, `putgd-cli.exe`, `build-info.json` and `LICENSE.txt`: the portable build, run from any folder you can write to |
| `putgd-VERSION-windows-x86_64.msi` | the per-user installer (`src/windows/packaging/putgd.wxs`) |
| `windows-packages.json` | the build identity, the version mappings and each file's size and SHA-256 |

The packager refuses executables that don't embed the manifest's version and commit,
so a stale executable can't be packaged under a new label.

**Versions (#396).** Both executables carry a version resource generated from the
build manifest at configure time (`src/windows/cmake/WindowsVersion.cmake`):

| Field | Mapping | `26.1022.942-nightly` |
| --- | --- | --- |
| `FILEVERSION`, `PRODUCTVERSION` | `YY,MDD,N/10000,N%10000` | `26,1022,0,942` |
| `ProductVersion` string (Explorer's Details) | the full version | `26.1022.942-nightly` |
| MSI `ProductVersion` (upgrade order only) | `(YY−26)×12+month−1` . `day×8+N/65536` . `N%65536` | `9.176.942` |

Each mapping orders versions as [versioning.md](versioning.md) does: date, then count.
Windows Installer compares only three fields of at most 255.255.65535, so the MSI
mapping keeps 19 bits of the count. Configuration stops past N = 524,287 or April 2047
rather than produce an MSI that sorts below its predecessor. A nightly, a development
build and the stable release of one `YY.MDD.N` map to the same MSI version, and either
replaces the other. The CTest `windows_version_mapping` checks the order across day,
month and year boundaries and N passing 9,999 and 65,535 on one day.

## Installing and uninstalling

The MSI installs for the current user only, with no administrator prompt. It is not
signed yet, so SmartScreen asks once ([#447](https://github.com/patrick-hudson/put.gd-desktop/issues/447)).

- **Where:** `%LOCALAPPDATA%\Programs\put.gd`, a **put.gd** entry in the Start menu, and
  an entry in Settings → Apps → Installed apps that shows the calendar version.
  `HKCU\Software\put.gd\Installer` records the folder, so put.gd knows it was installed
  by the MSI.
- **Upgrades:** opening a newer MSI replaces the installed version: the old one is removed
  completely, then the new one is installed. An older MSI is refused with "A newer put.gd
  is already installed. Uninstall it first to install this version."
- **Start at login** stays put.gd's own setting, as in the portable build: put.gd writes
  and removes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\put.gd` from
  **Launch at login**, pointing at the `putgd.exe` that runs. The installer never writes it.
- **Uninstalling** (Settings → Apps, or `msiexec /x` with the product code) removes what
  the MSI installed: the program folder, the Start menu entry, the Apps entry and the
  `Installer` key. It keeps everything put.gd made: `%LOCALAPPDATA%\put.gd`
  (configuration, history, previews, logs), local copies under `Documents\put.gd`, and
  the Credential Manager entries. That matches the Linux packages ([#382](https://github.com/patrick-hudson/put.gd-desktop/issues/382)).
- **Start at login after uninstalling:** if **Launch at login** was on, its Run value stays
  and points at the removed `putgd.exe`. Windows skips a missing program at login, and
  reinstalling brings it back. To remove it, turn **Launch at login** off before
  uninstalling, or delete the `put.gd` value under that Run key.

**Why an MSI and not MSIX:** a packaged (MSIX) app's AppData writes go to a private
store that uninstalling deletes, its start at login is the `windows.startupTask`
extension rather than the app's own Run value, it can't be built on Linux, and it won't
install unsigned outside developer mode.

## Updating

A tagged build (`YY.MDD.N` or `YY.MDD.N-nightly`) updates itself from Settings →
Updates → **Download and install**, or `update.install` with `confirm: true`, and then
**Restart put.gd** (`update.restart`). This works the way the AppImage's install does
([updates.md](updates.md#updating-on-windows)): the release's `SHA256SUMS` must carry
the channel's minisign signature, and the package must match it.

- **MSI installs:** the verified MSI waits in `%LOCALAPPDATA%\put.gd\updates`. On restart,
  a copy of `putgd.exe` there waits for put.gd to quit and runs `msiexec /passive`. Then it
  starts the new version. If the installer fails, Windows Installer rolls it back, and put.gd
  starts again on the old version and reports the exit code.
- **Portable folders:** the verified zip is unpacked beside `putgd.exe`. The running files
  move to `.putgd-update-backup-VERSION`, and the new ones take their names. The restart
  starts the new `putgd.exe`. The backup is removed once the new version runs.

Development builds, and a channel other than the build's own, don't install
(`update_install_unavailable`, `development_build` or `switch_unsupported`).

Verified on 2026-10-22 in the w11 guest with lab-signed nightlies 26.1022.1000 to 1004.
That covered install, Start menu and Apps, MSI upgrade, same-version replacement, the
refused downgrade, in-app updates of the MSI and the portable folder from Settings and the
CLI, a tampered `SHA256SUMS` refused, and an uninstall that kept the user data
([evidence](evidence/2026-10-22-windows-packaging-404/README.md)).

![The Start menu entry of the per-user MSI](evidence/2026-10-22-windows-packaging-404/01-start-menu-search-annotated.png)

![Apps shows the calendar version](evidence/2026-10-22-windows-packaging-404/03-apps-putgd-A-annotated.png)

![Settings → Updates: Download and install](evidence/2026-10-22-windows-packaging-404/20-settings-download-and-install-annotated.png)

![The update is ready; Restart put.gd runs the installer](evidence/2026-10-22-windows-packaging-404/21-settings-after-click-annotated.png)

![After the restart: updated from the previous version](evidence/2026-10-22-windows-packaging-404/23-settings-updated-from-annotated.png)

## Lab

Acceptance runs in a disposable Windows 11 guest cloned from the sealed base on a
builder ([Windows VM lab](windows-vm-lab.md)), reached over the new
`--ssh-forward` port. The resident is started on the interactive desktop through an
interactive scheduled task, after which `putgd-cli` works from any session because
the pipe namespace is global. Global shortcuts, the tray and the clipboard belong to
the desktop session; the test scripts set and read the clipboard from there.

## Verified on 2026-10-22

Guest `w11-e3ceec338284` on d69 (Windows 11 Enterprise evaluation 26H2, 3 vCPUs,
8 GiB), build `26.1022.985-dev+g0b8dfe0e7278`, mingw-w64 GCC 14. Raw screenshots,
the guest notes and the test scripts are in the private
`work/verification/2026-10-22-windows-native/` folder; the pictures below are the
curated subset.

- Resident start, pipe, `capabilities`, `diagnostics`, `settings.get/patch/reload/validate/reorderProfiles/developmentTemplate`, revision conflicts, `ui.quit`.
- Desktop, window (click to pick) and region capture (live overlay with crosshair, drag; Escape cancels with `capture_cancelled`), the Ctrl+Shift+3 global shortcut, tray menu and brand icon, completion toast.
- Clipboard import of text, files and images; local copies under `Documents\put.gd`; history records and previews; `history.copyLink`, `history.copyImage`, `history.preview`, `history.detail`, `operation.copy/retry/retryLocalSave`.
- Capture region & edit driven through `editor.open/inspect/apply/preview/submit` (redaction, arrow, text, crop) with the sanitized PNG handoff.
- Live S3 with the acceptance key set under `put.gd/acceptance/windows-lab/`: credentials stored in Credential Manager, `setup.testDestination` (synthetic upload, public GET, guarded cleanup), a real capture uploaded and fetched publicly with matching bytes, link copied, `delete.prepare/confirm` removed the object; a wrong key set stayed `failed` with `s3_access_denied` and retried truthfully.
- Launch at login through the Run key, the update feed check against app.put.gd, the diagnostics package, `cache.clear`, `logs.clear`.
- Memory, resident alone with no window open, five minutes of one-second samples: 2.6 MiB working set on average (4.4 peak), 2.8 MiB private, 0 CPU seconds. After captures and uploads the resident settled between 4 and 8 MiB; the window helper used about 25 MiB while open and exits with the window.

![History in the light scheme: thumbnails, relative times, shortcut hints, retained preview](evidence/2026-10-22-windows-native/01-history-light-annotated.png)

![History in the dark scheme](evidence/2026-10-22-windows-native/02-history-dark-annotated.png)

![Settings: pivot navigation and flat sections](evidence/2026-10-22-windows-native/03-settings-light-annotated.png)

![Destinations in the dark scheme](evidence/2026-10-22-windows-native/04-destinations-dark-annotated.png)

![The completion toast](evidence/2026-10-22-windows-native/05-toast-annotated.png)

![The live region overlay over the running desktop](evidence/2026-10-22-windows-native/06-region-overlay-annotated.png)

![The tray icon in the notification area flyout](evidence/2026-10-22-windows-native/07-tray-flyout.png)

Not verified here: DPI scaling above 100 %, multiple monitors, keyboard-only
operation of the window, Windows 10, and a signed install.

## Not available in this build

| Gap | Issue |
| --- | --- |
| Screen recording (`record` action, `recording.*`) | #400 |
| Shipping the Proton runtime with a Windows build | #408 |
| Backblaze B2 destination | #409 |
| Multipart uploads and files over the 5 GiB single-PUT limit | #402 |
| Per-item cancellation, the `ui.*` window-driving methods, diagnostics upload, history gallery/date/MIME filters | #403 |
| Publishing the Windows packages with nightly and stable releases | [#446](https://github.com/patrick-hudson/put.gd-desktop/issues/446) |
| Signed executables and installer | [#447](https://github.com/patrick-hudson/put.gd-desktop/issues/447) |
| Switching update channels in-app | [#448](https://github.com/patrick-hudson/put.gd-desktop/issues/448) |
| Following a system theme change while the window is open | #406 |
