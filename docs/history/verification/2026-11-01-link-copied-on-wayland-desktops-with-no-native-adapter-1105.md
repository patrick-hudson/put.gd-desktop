# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Link copied on Wayland desktops with no native adapter (#1105) — 2026-11-01

PR #1134's CI packages (`26.1101.3720-dev+ga01c147bef80`) ran on disposable
guests with a real right-click from a cold start, live S3 and the real browser.
Offscreen and fixture tests prove only the protocol.

| Desktop | Package | Compositor offers data-control | Result |
|---|---|---|---|
| COSMIC 1.11, Fedora 44 | RPM, AppImage | ext and wlr | "Upload complete", clipboard holds the link, object sha256 matches |
| Budgie 10.10, Ubuntu 26.04 | DEB, AppImage | ext and wlr | "Upload complete", clipboard holds the link, object sha256 matches |
| Pantheon Secure Session, elementary OS 8.1 | Ubuntu 24.04 DEB | neither | "Completed with a problem: Uploaded. Could not copy the link.", `source_unsupported` |

On COSMIC with the RPM, **Upload clipboard** also read text and a PNG (pixels
equal to the uploaded object), `history.copyImage` wrote an image, and a region
capture failed with `source_unsupported`. Budgie's clipboard read and image write
were not run on a real desktop. GNOME, KDE and X11 guests were not rerun for this
change; their routes are unchanged.
