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
- **GNOME 50:** needs the put.gd Shell extension. After installing, log out and back in, then enable it.
- **Fedora recording:** needs extra encoder packages. See the recording guide.

[Unreleased]: https://github.com/patrick-hudson/put.gd-desktop/commits/nightly
