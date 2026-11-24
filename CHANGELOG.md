# Changelog

All notable changes to put.gd, newest first. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). put.gd uses calendar
versions `YY.MDD.N`: the UTC date of the released commit (month times 100 plus
day) and its commit count.

`tools/releases/promote.py --open` writes the `[Unreleased]` section from the
pull requests merged since the previous stable release, curated in its
`changelog: next stable release` pull request. Once the stable release is
published, `promote.py --finalize vYY.MDD.N` names the section after it; the
date is the day it was published.

## [Unreleased]

New places to upload, a check that catches passwords, keys and card numbers before a capture leaves your machine, styled screenshots, text reading in more languages, and many reliability fixes. The AppImage's bundled FFmpeg is updated, fixing the known security issues in the previous stable AppImage.

### Added

- **SFTP:** upload to your own server over SSH, signing in with a password or a key file.
- **Nextcloud and WebDAV:** upload to a Nextcloud server or any WebDAV share and copy a share link.
- **Cloudflare R2** is its own destination type, including tokens scoped to one bucket.
- **Custom HTTP uploader:** send captures to your own endpoint, or import a ShareX `.sxcu` file.
- **Expiring links:** S3 links can expire, and S3 and B2 links can be renewed from History.
- **Sensitive text check** (off by default): reads each capture before upload and looks for card numbers, keys and tokens, email addresses, phone numbers, IP addresses, your username and name, and your own words or patterns. Each kind is Off, Warn or Redact automatically. A warning offers **Redact and upload**, **Open in editor**, **Upload anyway** and **Cancel**.
- **Quick redact** covers connection-string passwords, secret-named values, passwords in flags, labels and diffs, webhooks, two-factor secrets, QR codes that hold a secret, many more providers' keys, and your own list.
- **Capture style** (off by default): a backdrop, padding, rounded corners and a shadow, with Graphite, Amber, Paper, Clear and Custom looks and a Style tool in the editor.
- **Text in other scripts:** Copy text and the sensitive text check read Chinese, Russian and other non-Latin pages in their own language when no language is set.
- **Unhinged mode** (off by default): text recognition, recording and History thumbnails may use every processor core.
- **Scrolling capture:** capture a page taller than the screen, scrolled by you or by put.gd's auto-scroll.
- **Capture:** one monitor, a delay, and Repeat last region. Copy text from any region and read QR codes.
- **Editor:** step numbers, pen, ellipse, line, pixelate and Copy image.
- **Recording:** microphone audio, click highlights, cutting out the middle of a recording, a filmstrip in review, and single-window recording on KDE and GNOME Wayland without the portal chooser.
- **Desktops and packages:** Budgie on Wayland, Hyprland (tested on Omarchy), a Fedora 45 RPM and Arch Linux packages from a signed pacman repository.
- **Updates:** the AppImage updates itself with put.gd's own zsync client, downloading from pkg.put.gd first.

### Changed

- The tray menu and action bar have a new layout, and the tray shows capture, processing and upload stages.
- History is refined, and an upload whose share link didn't finish can be completed in one click.
- Settings keeps errors in its own error log and saves one request at a time.
- Proton Drive uploads with a share link in under two seconds, and large B2 files send up to four parts at once.
- Text recognition uses up to eight cores and reads faint lines a second time.
- GNOME 50 setup finishes with one logout.

### Fixed

- A recording whose PipeWire screen share goes away stops with a clear error instead of leaving the recorder stuck.
- An accepted region is kept when the capture helper is slow to exit, on X11 and Wayland.
- Scrolling capture no longer draws stray lines or white patches, follows playing videos, and handles sticky headers and pinned side columns.
- No upload is sent twice. Slow S3 and B2 uploads finish instead of timing out, and an unclear multipart completion is checked instead of resent.
- Uploads with an unknown outcome are listed under Failed uploads and stay unknown when a resend fails.
- S3 deletion works on Garage and on R2 with a bucket-scoped token. S3 settings refuse a Region the SDK can't use and an Endpoint with a path.
- Proton recovers when KDE's keyring fails a new helper's first read.
- Quitting stops waiting deletions instead of waiting for the server.
- Long tooltips and file names stay readable, and failure notices no longer come back from notification servers that keep them.

### Security

- The AppImage bundles FFmpeg 9.0.2. The 26.1105.3906 AppImage's FFmpeg 7.1.5 had known critical and high CVEs with fixes ([#1309](https://github.com/patrick-hudson/put.gd-desktop/issues/1309)).
- A token from a `.sxcu` address stays out of thumbnail URLs and error messages, and Test upload and link no longer keeps its signed link.
- The Proton Drive helper is still built with Bun 1.4.4. Bun's HTTP/3 advisory GHSA-p268-p2w7-qr8x is accepted for this release because the helper doesn't use HTTP/3; the next release moves to Bun 1.4.5 ([#2005](https://github.com/patrick-hudson/put.gd-desktop/issues/2005)).

### Known limitations

- The sensitive text check covers only text put.gd can read; very small, faint or low-contrast text can be missed.
- On a page in a script you haven't chosen, the check can mistake text for a key.
- About one time in a hundred, the image editor on Fedora GNOME closes by itself as it opens; open it again.
- GNOME 50 still needs the put.gd Shell extension, and Fedora recording still needs extra encoder packages.

## [26.1105.3906] - 2026-11-05

The first stable release of put.gd for Linux: screenshots and screen recordings that upload to your own storage and leave a shareable link on your clipboard.

### Added

- **Capture:** a region, the active window or the whole desktop, from the main window, the tray menu or a keyboard shortcut you can change.
- **Region selection:** the desktop stays live while you drag. A setting freezes the screen first if you prefer.
- **Image editor:** crop, add arrows and text, and redact private details. Nothing uploads until you choose Upload.
- **Screen recording:** record a region, with pause and resume, then trim it and add timed blur or solid covers. Export MP4 or WebM with system audio, or a silent GIF or animated WebP.
- **Storage:** Amazon S3, Cloudflare R2 and other S3-compatible services, plus Backblaze B2. Large files upload in parts and several uploads can run at once, with live progress.
- **Proton Drive (experimental):** save privately or share through Proton Drive. This is a third-party integration, and its links open a share page rather than the image itself.
- **File managers:** right-click files or folders and choose Upload with put.gd.
- **History:** search past captures in a list or gallery, copy a link again, or reopen a saved image to edit it.
- **Appearance:** System, Dark and Light themes.
- **Credentials:** saved in your system keychain. The configuration file only stores references to them.
- **Install:** a one-line installer that verifies its download, with DEB packages for Ubuntu 24.04, Linux Mint 22, Ubuntu and Kubuntu 26.04 and Debian 13, an RPM for Fedora 44, and an AppImage for Aurora and other distributions with glibc 2.39 or newer.
- **Updates:** a daily check. DEB and RPM installs update from the signed repository at pkg.put.gd, and AppImage updates are offered in the app.
- **Desktops:** Cinnamon on X11, GNOME 50 on Wayland, and KDE Plasma on Wayland or X11.
- **Scripting and agents:** `putgd-cli` controls the running app, and an optional MCP helper gives agents capture, editing, recording, uploads and History.
- **Troubleshooting:** persistent logs, crash records, and a diagnostics package you can save locally or upload to one of your destinations.

### Changed

For anyone moving from the nightly builds:

- Versions are now calendar based: `YY.MDD.N`.
- put.gd now uses less memory while idle, and its region picker opens faster on Cinnamon, GNOME and KDE.
- The AppImage now bundles Qt 6.12.

### Known limitations

- **Monitors:** capture across monitors with different scaling, rotated monitors and unplugging a monitor mid-capture haven't been tested on real hardware.
- **KDE Plasma on X11:** tested less than Plasma on Wayland.
- **Software stores:** installing through a graphical software store isn't tested. Use the installer, `apt` or `dnf`.
- **Right-click upload:** in Thunar it's under Send To. With the AppImage, Files and Caja list it under Scripts.
- **Cinnamon tray:** the tray icon can show a placeholder image for a few seconds after put.gd starts. It recovers by itself.
- **GNOME 50:** needs the put.gd Shell extension. After installing a DEB or RPM, log out and back in, then enable it. The AppImage needs the matching extension installed separately; see the [GNOME desktop notes](docs/desktops/gnome.md).
- **Fedora recording:** needs extra encoder packages. See the [recording guide](docs/design/recording.md).

[Unreleased]: https://github.com/patrick-hudson/put.gd-desktop/compare/v26.1105.3906...nightly
[26.1105.3906]: https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1105.3906
