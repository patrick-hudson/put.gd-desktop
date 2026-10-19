# Plasma Wayland adapter

Target baseline: Kubuntu 26.04 LTS, Plasma 6.6, native Qt Wayland.
The adapter uses KWin ScreenShot2, KGlobalAccel v2, and the official
`ext-data-control-v1` protocol. Compositors without ext, such as KWin 6.3 on
Debian 13, still offer its wire-identical predecessor
`wlr-data-control-unstable-v1` (version 2). The adapter binds that only when ext
is absent; `capabilities` reports the protocol in use. Native desktop acceptance
remains required.

## Normal distro dependencies and integration

Build dependencies are `libwayland-dev`, `wayland-protocols` and Qt DBus. Both
data-control protocol files are vendored unmodified. CMake runs the distro `wayland-scanner`
`client-header` and `private-code` modes against the unmodified protocol XML
in `src/platform/kde/protocols/`. Keep generated files in the build tree.
Region selection also needs the current distro LayerShellQt interface development
package with Qt 6.5 or later for per-window shell integration: ScreenShot2's interactive method selects a **window or whole screen**,
not a rectangular region. The adapter uses compositor-positioned overlay layer
surfaces to show the required crosshair and rectangle over the live desktop.
After selection, it hides those surfaces and awaits a Wayland display sync before
requesting current screen pixels. The selected layout stays fixed throughout;
layout changes cancel acquisition. Native compositor evidence must verify that
neither the current nor a previous selection outline appears in the saved image.
It never substitutes a screen picker for region selection.
The Qt 6.4 compatibility build reports region selection unavailable when the
per-window LayerShellQt API cannot be used; the Plasma 6.6 native profile needs
the current distro library. An app-global shell-plugin override is not used.

Install an application desktop entry whose `Exec` identifies the actual
resident binary and includes:

```
X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2;
```

The screen-recording helper has its own entry, `putgd-record.desktop`, whose
`Exec` names the helper binary and whose `X-KDE-Wayland-Interfaces` lists
`zkde_screencast_unstable_v1`; KWin advertises its screencast protocol only to
executables matched that way (see `docs/recording.md`, "Wayland acquisition").
The helper also links the layer-shell region selector: a recording of a
region without a rectangle shows the same `KdeRegionOverlay` as a region
screenshot before any stream exists, and the ready ring, dimming and control
bar are overlay layer surfaces placed outside the recorded rectangle. KWin 6.6
and later leave every window of the client that requested a screencast stream
out of that stream, which also keeps the bar out of monitor recordings; KWin
6.3 (Debian 13) does not.
A desktop entry naming a different build executable does not grant permission.
Restart the resident application after installing/updating its entry. Do not
set `KWIN_SCREENSHOT_NO_PERMISSION_CHECKS` or other permission bypasses.
Permission rejection is reported as `capture_permission`; no portal picker or
XWayland fallback is introduced.

## Capture semantics

ScreenShot2 version 4 or later is needed for explicit returned screen identity,
window identity, and scale. Full desktop and region use native-resolution
per-output captures with the cursor excluded; frame composition uses shared
KTD8 geometry, highest participating density, and transparent layout gaps.
Selection is invalidated when output identity, position, dimensions, scale or
rotation changes. Window capture includes decorations, excludes shadows and
cursor, and preserves the compositor's native occlusion semantics.
An active-window request starts asynchronously at invocation so later app focus
cannot retarget it. The accepted operation still validates its configured
resource limits before the acquired image enters the shared pipeline.

Image-transfer FDs are nonblocking, bounded, timeout-controlled, and closed on
cancellation. Metadata validates image type, dimensions, stride, format, scale,
and exact transfer size. No source pixels enter diagnostics.

## Clipboard and shortcuts

Data control imports `text/uri-list`, supported image MIME types, and UTF-8 plain
text while the resident application is unfocused. Local files take precedence,
then image, then text. URLs in plain text remain text. Ownership changes advance
a revision without continuously reading payloads. Results advertise text,
PNG, or file URIs. An asynchronous compositor ownership fence checks for a newer
owner or invocation before committing the result. Superseded unsent copies return
`copy_suppressed`. After `set_selection` is sent, only ownership acknowledgement
remains; destroying the source would clear the clipboard rather than restore its
previous owner, so that committed request is not rolled back. Results set only CLIPBOARD; the adapter never sends
`set_primary_selection`. A separate application must witness PRIMARY preservation.

KGlobalAccel v2 uses `globalShortcutAvailable`, `setShortcutKeys`,
`yourShortcutsChanged`, and component `globalShortcutPressed`.
Returned bindings are authoritative and conflicts are exposed without stealing
shortcuts. Session service changes trigger capability updates and rebinding.

## Required native witness gate

Use the installed package under a real Plasma 6.6 Wayland session. Record distro,
KWin, Qt platform/version, ScreenShot2 version, data-control version, executable
identity/desktop entry, display layout, acceleration and package identity.
Test region crosshair/rectangle and Escape/right-click cancellation; external
window capture with focus changes; every enabled monitor and panels; permission
entry absence/mismatch; text/image/two-file imports while unfocused and independent
pastes of local-only and public-link results. Verify PRIMARY in another app before
and after every result category. Repeat with two fractional-scale/rotated outputs,
layout gaps, and output removal during selection. Verify shortcut conflict and
service/compositor loss expose errors and recover after session restart.

The bounded FD integration test is a synthetic pipe test. It cannot prove
permission, output placement, actual hotkeys, or clipboard interoperability.
Neither an offscreen test nor interface discovery establishes desktop support.

## Primary API evidence (read 2026-10-08)

- [Plasma 6.6 ScreenShot2 XML](https://raw.githubusercontent.com/KDE/kwin/Plasma/6.6/src/plugins/screenshot/org.kde.KWin.ScreenShot2.xml): raw-image FD, native-resolution options, scale/window/output metadata, interactive window/screen kinds only.
- [Plasma 6.6 ScreenShot2 implementation](https://raw.githubusercontent.com/KDE/kwin/Plasma/6.6/src/plugins/screenshot/screenshotdbusinterface2.cpp): interface version 5, restricted executable permission, delayed DBus reply and separate pipe writer.
- [Current KGlobalAccel interface](https://raw.githubusercontent.com/KDE/kglobalaccel/master/src/org.kde.KGlobalAccel.xml) and [client implementation](https://raw.githubusercontent.com/KDE/kglobalaccel/master/src/kglobalaccel.cpp): current sequence wire types and returned binding semantics.
- [Current KGlobalAccel sequence serialization](https://raw.githubusercontent.com/KDE/kglobalaccel/master/src/kglobalshortcutinfo_dbus.cpp): four combined Qt keys including trailing zeros inside the v2 sequence struct.
- [Current KGlobalAccel private flags](https://raw.githubusercontent.com/KDE/kglobalaccel/master/src/kglobalaccel_p.h): SetPresent=2, NoAutoloading=4.
- [Official ext-data-control protocol](https://raw.githubusercontent.com/wayland-mirror/wayland-protocols/main/staging/ext-data-control/ext-data-control-v1.xml) and [Plasma 6.6 Wayland server](https://raw.githubusercontent.com/KDE/kwin/Plasma/6.6/src/wayland_server.cpp): unfocused data control, CLIPBOARD/PRIMARY separation, KWin manager implementation.
- [LayerShellQt Window interface](https://raw.githubusercontent.com/KDE/layer-shell-qt/master/src/interfaces/window.h): output anchoring, overlay layer, keyboard interactivity. [Plasma 6.6 per-window implementation](https://raw.githubusercontent.com/KDE/layer-shell-qt/Plasma/6.6/src/interfaces/window.cpp) initializes individual layer window roles; older app-global shell setup is not used.
