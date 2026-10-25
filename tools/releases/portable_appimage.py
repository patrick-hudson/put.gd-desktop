#!/usr/bin/env python3
"""Assemble an already installed release with pinned, official AppImage tooling."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import urllib.request

from audit_native import sha256 as digest
from audit_proton import audit_proton
import appimage_notices
import appimage_update
from release_identity import parse_tag

ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / "packaging/appimage/tools.lock.json"
MEDIA = ROOT / "packaging/appimage/media.json"
# AppRun names this icon in the host entries it writes for the windows' app IDs.
WINDOW_ICON = "usr/share/icons/hicolor/scalable/apps/putgd.svg"
# The pinned FFmpeg build (media.json) installs these; their notices come from its source.
FFMPEG_LIBRARIES = ("libavcodec", "libavformat", "libavfilter", "libavutil", "libswscale", "libswresample")


def tools(directory: Path) -> tuple[dict, dict[str, Path]]:
    lock = json.loads(LOCK.read_text())
    directory.mkdir(parents=True, exist_ok=True)
    result = {}
    for item in lock["tools"]:
        path = directory / item["name"]
        if path.exists():
            if digest(path) != item["sha256"]:
                raise ValueError("Deployment tool digest mismatch: " + item["name"])
        else:
            temporary = path.with_suffix(path.suffix + ".download")
            try:
                # A socket timeout, so a stalled download fails instead of hanging the build (#467).
                with urllib.request.urlopen(item["url"], timeout=120) as response, temporary.open("wb") as target:
                    shutil.copyfileobj(response, target)
                if digest(temporary) != item["sha256"]:
                    raise ValueError("Official deployment tool digest mismatch: " + item["name"])
                temporary.replace(path)
            finally:
                temporary.unlink(missing_ok=True)
        path.chmod(0o755 if item.get("executable", True) else 0o644)
        result[item["name"]] = path.resolve()
    # zsyncmake is the static zsync 0.6.2 inside the pinned appimagetool, checked
    # against its own digest. (zsync2's zsyncmake2 drops a trailing partial block.)
    bundled = lock["zsyncmake"]
    path = directory / "zsyncmake"
    if not path.exists():
        with tempfile.TemporaryDirectory(dir=directory) as temporary:
            subprocess.run([str(result[bundled["bundledIn"]]), "--appimage-extract", bundled["path"]],
                           check=True, cwd=temporary, stdout=subprocess.DEVNULL)
            shutil.copyfile(Path(temporary) / "squashfs-root" / bundled["path"], path)
    if digest(path) != bundled["sha256"]:
        raise ValueError("Bundled zsyncmake digest mismatch")
    path.chmod(0o755)
    result["zsyncmake"] = path.resolve()
    return lock, result


def system_library_licenses(appdir: Path, claimed: set[str],
                            extra_origins: dict[str, Path] | None = None) -> dict[str, list[str]]:
    """Retain the distro copyright/license notice for each deployed system ELF.

    Files in `claimed` belong to put.gd, Qt or another pinned upstream
    component (appimage_notices.py). `extra_origins` names files that are not
    in the linker cache, such as the GStreamer plugins and scanner, by their
    original distro path. Returns each binary package's AppDir files."""
    cache = subprocess.run(["/sbin/ldconfig", "-p"], check=True, text=True,
                           capture_output=True).stdout
    origins = {}
    for line in cache.splitlines():
        if " => " in line:
            name, source = line.split(" => ", 1)
            original = Path(source.strip())
            origins.setdefault(name.split()[0], original)
            origins.setdefault(original.resolve().name, original)
    origins.update(extra_origins or {})
    packages: dict[str, list[str]] = {}
    for path in sorted(appdir.rglob("*")):
        if path.is_symlink() or not path.is_file() or str(path.relative_to(appdir)) in claimed:
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        name = path.name
        source = origins.get(name)
        if source is None:
            # Private distro libraries outside the linker cache, such as
            # libpulsecommon under pulseaudio/, are found by their owning package.
            owners = subprocess.run(["dpkg-query", "-S", "*/" + name], text=True, capture_output=True).stdout
            paths = {line.split(": ", 1)[1].strip() for line in owners.splitlines()
                     if ": " in line and line.split(": ", 1)[1].strip().endswith("/" + name)}
            if len(paths) != 1:
                raise ValueError("Cannot identify deployed system library: " + name)
            source = Path(paths.pop())
        owner = subprocess.run(["dpkg-query", "-S", str(source)], text=True, capture_output=True)
        if owner.returncode:
            owner = subprocess.run(["dpkg-query", "-S", str(source.resolve())], text=True, capture_output=True)
        if owner.returncode:
            raise ValueError("Cannot identify distro license for: " + str(source))
        package = owner.stdout.split(": ", 1)[0].split(":", 1)[0]
        packages.setdefault(package, []).append(str(path.relative_to(appdir)))
        if len(packages[package]) > 1:
            continue
        notice = Path("/usr/share/doc") / package / "copyright"
        if not notice.is_file():
            raise ValueError("Distro copyright notice missing: " + package)
        destination = appdir / "usr/share/doc/putgd/bundled-licenses/distro" / package / "copyright"
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(notice, destination)
    common = Path("/usr/share/common-licenses")
    if common.is_dir():
        shutil.copytree(common, appdir / "usr/share/doc/putgd/bundled-licenses/distro-common",
                        dirs_exist_ok=True)
    return packages


def deployment_qmake(qt: Path, output: Path) -> Path:
    """Use Qt's supported qt.conf paths to deploy SQLite without unused drivers."""
    view = output / "qt-deployment"
    plugins = view / "plugins"
    (plugins / "sqldrivers").mkdir(parents=True)
    for directory in (qt / "plugins").iterdir():
        if directory.name != "sqldrivers":
            (plugins / directory.name).symlink_to(directory, target_is_directory=directory.is_dir())
    sqlite = qt / "plugins/sqldrivers/libqsqlite.so"
    if not sqlite.is_file():
        raise ValueError("Official Qt SQLite driver is required")
    (plugins / "sqldrivers/libqsqlite.so").symlink_to(sqlite)
    binary = view / "bin/qmake"
    binary.parent.mkdir()
    shutil.copy2(qt / "bin/qmake", binary)
    (binary.parent / "qt.conf").write_text("[Paths]\nPrefix=" + str(qt) + "\nPlugins=" + str(plugins) + "\n")
    return binary


# linuxdeploy's Qt plugin deploys only xcb unless told otherwise. The resident
# starts its image worker (`putgd-ui --image-job`, src/core/ImageJob.cpp) on
# offscreen, as do putgd-record and the editor job without a display; Qt aborts
# a process whose requested platform plugin is absent. Since Qt 6.10 one
# libqwayland.so serves the wayland and wayland-egl platform names.
EXTRA_PLATFORM_PLUGINS = ("libqwayland.so", "libqoffscreen.so")


# The image worker decodes History's GIF and WebP previews (#189) with these;
# the aqt Qt's qtimageformats module carries WebP with its own libwebp.
IMAGE_FORMAT_PLUGINS = ("libqgif.so", "libqjpeg.so", "libqwebp.so")


def audit_platforms(appdir: Path, inventory: dict) -> dict:
    """Start the image worker the way the resident does, from the image alone."""
    platforms = appdir / "usr/plugins/platforms"
    missing = [name for name in ("libqxcb.so", *EXTRA_PLATFORM_PLUGINS) if not (platforms / name).is_file()]
    if missing:
        raise ValueError("Qt platform plugins missing from the image: " + ", ".join(missing))
    formats = appdir / "usr/plugins/imageformats"
    missing = [name for name in IMAGE_FORMAT_PLUGINS if not (formats / name).is_file()]
    if missing:
        raise ValueError("Qt image format plugins missing from the image: " + ", ".join(missing))
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("DISPLAY", "WAYLAND_DISPLAY") and not key.startswith("QT_")}
    environment.update(LD_LIBRARY_PATH=str(appdir / "usr/lib") + ":" + str(appdir / inventory["privateLibraryDirectory"]),
                       QT_QPA_PLATFORM="offscreen")
    # Its control channel is a pipe pair. A worker whose platform initialised
    # sees the input close before any request and exits with status 3.
    worker = subprocess.run([str(appdir / "usr/bin/putgd-ui"), "--image-job"], input="", env=environment,
                            capture_output=True, text=True, timeout=30)
    if worker.returncode != 3:
        lines = worker.stderr.strip().splitlines()
        raise ValueError("Image worker did not start on the offscreen platform: " +
                         (lines[0] if lines else "exit status " + str(worker.returncode)))
    return {"platformPlugins": sorted(path.name for path in platforms.glob("*.so")),
            "imageFormatPlugins": sorted(path.name for path in formats.glob("*.so")),
            "imageWorkerStartedOffscreen": True}


# Qt's Wayland platform does not start without a shell integration: Qt then
# falls back to XWayland, or aborts where QT_QPA_PLATFORM=wayland is set.
# linuxdeploy's Qt plugin deploys these client plugins only for Qt Wayland
# Compositor apps, so deploy what distro qt6-wayland packages ship: xdg-shell
# for ordinary windows (the KDE region overlay sets LayerShellQt per window),
# client-side decorations where the compositor draws none (Qt picks adwaita on
# GNOME, bradient elsewhere) and the EGL buffer integration that the generic
# platform loads for OpenGL windows.
WAYLAND_CLIENT_PLUGINS = ("wayland-shell-integration/libxdg-shell.so",
                          "wayland-decoration-client/libbradient.so",
                          "wayland-decoration-client/libadwaita.so",
                          "wayland-graphics-integration-client/libqt-plugin-wayland-egl.so")
# A headless compositor offering xdg-shell and, like GNOME, no server-side decorations.
HEADLESS_COMPOSITOR = ("weston", "--backend=headless-backend.so", "--shell=kiosk-shell.so",
                       "--no-config", "--socket={socket}")


def audit_wayland(appdir: Path, inventory: dict, compositor: tuple[str, ...] = HEADLESS_COMPOSITOR) -> dict:
    """Start the image worker on Qt's Wayland platform under a private headless compositor."""
    plugins = appdir / "usr/plugins"
    missing = [name for name in ("wayland-shell-integration/liblayer-shell.so", *WAYLAND_CLIENT_PLUGINS)
               if not (plugins / name).is_file()]
    if missing:
        raise ValueError("Qt Wayland client plugins missing from the image: " + ", ".join(missing))
    if shutil.which(compositor[0]) is None:
        raise ValueError("The Wayland start check requires " + compositor[0])
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR") and not key.startswith("QT_")}
    with tempfile.TemporaryDirectory(prefix="pgwl-") as runtime:
        environment["XDG_RUNTIME_DIR"] = runtime
        socket = Path(runtime) / "putgd-audit"
        log = (Path(runtime) / "compositor.log").open("w")
        server = subprocess.Popen([part.format(socket=socket.name) for part in compositor], env=environment,
                                  stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 20
            while not socket.exists() and server.poll() is None and time.monotonic() < deadline:
                time.sleep(0.1)
            if not socket.exists():
                lines = (Path(runtime) / "compositor.log").read_text(errors="replace").strip().splitlines()
                raise ValueError("The headless Wayland compositor did not start" + (": " + lines[-1] if lines else ""))
            environment.update(LD_LIBRARY_PATH=str(appdir / "usr/lib") + ":" + str(appdir / inventory["privateLibraryDirectory"]),
                               WAYLAND_DISPLAY=socket.name, XDG_SESSION_TYPE="wayland", QT_QPA_PLATFORM="wayland",
                               QT_LOGGING_RULES="qt.qpa.wayland.debug=true")
            worker = subprocess.run([str(appdir / "usr/bin/putgd-ui"), "--image-job"], input="", env=environment,
                                    capture_output=True, text=True, timeout=30)
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
            log.close()
    lines = worker.stderr.strip().splitlines()
    shells = re.findall(r"Using the '([^']+)' shell integration", worker.stderr)
    if worker.returncode != 3 or shells != ["xdg-shell"]:
        failures = [line for line in lines if "failed" in line.lower() or "could not" in line.lower()]
        raise ValueError("Image worker did not start on the Wayland platform with xdg-shell: " +
                         (failures[0] if failures else lines[-1] if lines else "exit status " + str(worker.returncode)))
    return {"waylandClientPlugins": sorted(str(path.relative_to(plugins)) for path in plugins.glob("wayland-*/*.so")),
            "headlessCompositor": compositor[0], "imageWorkerStartedOnWayland": True,
            "waylandShellIntegration": shells[0]}


def refresh_runtime_manifest(appdir: Path) -> None:
    path = appdir / "usr/share/putgd/aws-runtime-manifest.json"
    manifest = json.loads(path.read_text())
    module = manifest["module"]
    candidates = list(appdir.glob("usr/lib*/**/putgd/" + module["file"]))
    if len(candidates) != 1:
        raise ValueError("Expected exactly one installed private S3 module")
    for record in [*manifest["libraries"], module]:
        record["sha256"] = digest(candidates[0].parent / record["file"])
    path.write_text(json.dumps(manifest, indent=2) + "\n")



def pkg_config_variable(module: str, variable: str) -> Path:
    value = subprocess.run(["pkg-config", "--variable=" + variable, module], check=True,
                           text=True, capture_output=True).stdout.strip()
    if not value:
        raise ValueError("pkg-config has no " + variable + " for " + module)
    return Path(value)


def media_layout(appdir: Path) -> dict[str, Path]:
    """Where putgd-record (BundledRuntime) and AppRun expect the recording runtime."""
    return {"plugins": appdir / "usr/lib/gstreamer-1.0",
            "scanner": appdir / "usr/libexec/gstreamer-1.0/gst-plugin-scanner",
            "fallback": appdir / "usr/lib/putgd-host-fallback"}


def media_sources(appdir: Path, media: dict) -> tuple[list[Path], Path]:
    """The builder's GStreamer plugins and scanner that putgd-record uses."""
    if not (appdir / "usr/bin/putgd-record").is_file():
        raise ValueError("The AppImage requires putgd-record; configure with GStreamer and the pinned FFmpeg")
    plugin_source = pkg_config_variable("gstreamer-1.0", "pluginsdir")
    plugins = [plugin_source / ("libgst" + name + ".so") for name in media["gstreamer"]["plugins"]]
    for path in plugins:
        if not path.is_file():
            raise ValueError("GStreamer plugin missing on the builder: " + path.name)
    scanner = pkg_config_variable("gstreamer-1.0", "pluginscannerdir") / "gst-plugin-scanner"
    if not scanner.is_file():
        raise ValueError("gst-plugin-scanner missing on the builder")
    return plugins, scanner


def relocate_media(appdir: Path, plugins: list[Path], scanner: Path) -> dict[str, Path]:
    """Move what linuxdeploy deployed into usr/lib and usr/bin to where AppRun
    points GStreamer, and keep their RPATH on the image's usr/lib.

    Returns each moved file's distro origin for the license notices."""
    layout = media_layout(appdir)
    layout["plugins"].mkdir(parents=True)
    layout["scanner"].parent.mkdir(parents=True)
    origins = {}
    for source in plugins:
        deployed = appdir / "usr/lib" / source.name
        if not deployed.is_file():
            raise ValueError("linuxdeploy did not deploy GStreamer plugin " + source.name)
        deployed.replace(layout["plugins"] / source.name)
        subprocess.run(["patchelf", "--set-rpath", "$ORIGIN/..", str(layout["plugins"] / source.name)], check=True)
        origins[source.name] = source
    deployed = appdir / "usr/bin" / scanner.name
    if not deployed.is_file():
        raise ValueError("linuxdeploy did not deploy gst-plugin-scanner")
    deployed.replace(layout["scanner"])
    subprocess.run(["patchelf", "--set-rpath", "$ORIGIN/../../lib", str(layout["scanner"])], check=True)
    origins[scanner.name] = scanner
    return origins


def stage_host_fallbacks(appdir: Path, media: dict) -> list[str]:
    """Bundle the host-preferred libraries AppRun adds only when the host lacks them.

    libva must match the host's VA drivers, so the image normally uses the
    host's copy; the fallback only lets putgd-record load on a host without it."""
    cache = subprocess.run(["/sbin/ldconfig", "-p"], check=True, text=True, capture_output=True).stdout
    fallback = media_layout(appdir)["fallback"]
    fallback.mkdir(parents=True)
    copied = []
    for soname in media["hostFallbackLibraries"]:
        paths = [line.split(" => ", 1)[1].strip() for line in cache.splitlines()
                 if " => " in line and line.split()[0] == soname and "x86-64" in line]
        if not paths:
            raise ValueError("Host fallback library missing on the builder: " + soname)
        shutil.copy2(Path(paths[0]).resolve(), fallback / soname)
        copied.append(soname)
    return copied


def gpl_components(appdir: Path, media: dict) -> list[dict]:
    """The bundled GPL libraries and the plugin linking them, with their distro packages.

    The release publishes these source packages (appimage_media_sources.py)."""
    cache = subprocess.run(["/sbin/ldconfig", "-p"], check=True, text=True, capture_output=True).stdout
    origins = {}
    for line in cache.splitlines():
        if " => " in line and "x86-64" in line:
            origins.setdefault(line.split()[0], Path(line.split(" => ", 1)[1].strip()))
    plugin_dir = pkg_config_variable("gstreamer-1.0", "pluginsdir")
    files = [(path, origins.get(path.name), False) for pattern in media["gplLibraries"]
             for path in sorted((appdir / "usr/lib").glob(pattern))]
    # The one bundled plugin that links a GPL library (x264enc).
    files.append((media_layout(appdir)["plugins"] / "libgstx264.so", plugin_dir / "libgstx264.so", True))
    components, seen = [], set()
    for path, origin, plugin in files:
        if origin is None or not path.is_file():
            raise ValueError("Cannot identify the distro origin of " + path.name)
        owner = subprocess.run(["dpkg-query", "-S", str(origin.resolve())], text=True, capture_output=True)
        if owner.returncode:
            owner = subprocess.run(["dpkg-query", "-S", str(origin)], check=True, text=True, capture_output=True)
        package = owner.stdout.split(": ", 1)[0].split(":", 1)[0]
        fields = subprocess.run(["dpkg-query", "-W", "-f", "${Version}\t${source:Package}\t${source:Version}", package],
                                check=True, text=True, capture_output=True).stdout.split("\t")
        record = {"file": path.name, "binaryPackage": package, "binaryVersion": fields[0],
                  "sourcePackage": fields[1], "sourceVersion": fields[2]}
        if plugin and record["sourcePackage"] not in media["gplSourcePackages"]:
            raise ValueError("Unexpected source package for " + path.name + ": " + record["sourcePackage"])
        key = (record["sourcePackage"], record["sourceVersion"])
        record["publishSource"] = key not in seen
        seen.add(key)
        components.append(record)
    if not any(item["file"].startswith("libx264.so") for item in components) or not any(item["file"].startswith("libx265.so") for item in components):
        raise ValueError("The image must bundle libx264 and libx265")
    return components


def write_gpl_notice(appdir: Path, components: list[dict], ffmpeg: dict, version: str) -> Path:
    """State the image's GPL terms and where its corresponding source is."""
    lines = ["put.gd AppImage " + version, "",
             "This image bundles GPL-licensed codec libraries for screen recording, so the image as a",
             "whole is distributed under the GNU General Public License, version 2 or later",
             "(usr/share/doc/putgd/bundled-licenses/ffmpeg/COPYING.GPLv2). put.gd's own code stays",
             "MIT licensed; every bundled component keeps its own license, listed under",
             "usr/share/doc/putgd/bundled-licenses.", "",
             "Corresponding source: the release asset putgd-" + version + "-appimage-complete-source.tar.gz",
             "(all bundled components, including Qt, LGPL libraries and the static runtime) and putgd-" + version + "-source.tar.gz",
             "(put.gd), published with this image on the project's GitHub release. The source location",
             "of every other bundled component, Qt and the LGPL libraries among them, is listed in",
             "runtime-notices.txt and bundled-components.json.", "",
             "FFmpeg " + ffmpeg["release"] + " (" + ffmpeg["repository"] + " commit " + ffmpeg["commit"] + "),",
             "configured as recorded in packaging/appimage/media.json.", ""]
    lines += ["%s %s (Ubuntu source package %s %s)" % (item["binaryPackage"], item["binaryVersion"],
              item["sourcePackage"], item["sourceVersion"]) for item in components]
    notice = appdir / "usr/share/doc/putgd/APPIMAGE-GPL-NOTICE.txt"
    notice.write_text("\n".join(lines) + "\n")
    return notice


def audit_media(appdir: Path, media: dict, probe: str) -> dict:
    """Require the relocated helper to see only image plugins and the pinned FFmpeg."""
    capabilities = json.loads(probe)
    gst = capabilities.get("gstreamer", {})
    if not gst.get("available"):
        raise ValueError("Bundled GStreamer did not initialise: " + str(gst.get("error")))
    missing = [name for name in media["gstreamer"]["requiredElements"] if not gst.get("elements", {}).get(name)]
    if missing:
        raise ValueError("Bundled GStreamer elements missing: " + ", ".join(missing))
    plugins = media_layout(appdir)["plugins"].resolve()
    outside = [directory for directory in gst.get("pluginDirectories", []) if Path(directory).resolve() != plugins]
    if outside:
        raise ValueError("GStreamer loaded plugins from outside the image: " + ", ".join(outside))
    ffmpeg = capabilities.get("ffmpeg", {})
    if not ffmpeg.get("h264Decoder", {}).get("usable"):
        raise ValueError("Bundled FFmpeg cannot open its H.264 decoder")
    for name in ("mp4", "webm", "gif", "webp"):
        if name not in capabilities.get("formats", []):
            raise ValueError("Bundled FFmpeg cannot export " + name)
    # Without a GPU (the builder), the bundled fallbacks must be what works:
    # x264 for live capture, libx264 and libx265 for MP4 export.
    exportable = capabilities.get("exportEncoders", {})
    if "libx264" not in exportable.get("h264", []) or "libx265" not in exportable.get("h265", []):
        raise ValueError("Bundled libx264/libx265 export encoders do not open")
    if "x264enc" not in gst.get("usableH264Encoders", []):
        raise ValueError("Bundled x264enc does not encode")
    library = appdir / "usr/lib"
    bundled_host = sorted(path.name for pattern in media["hostLibraries"] for path in library.glob(pattern))
    if bundled_host:
        raise ValueError("Host-provided libraries were bundled: " + ", ".join(bundled_host))
    return {"gstreamer": gst.get("version"), "pluginDirectories": gst.get("pluginDirectories"),
            "elements": {name: gst["elements"][name] for name in media["gstreamer"]["requiredElements"]},
            "ffmpeg": {key: ffmpeg.get(key) for key in ("avcodec", "avformat", "avfilter", "avutil")},
            "h264Decoder": ffmpeg["h264Decoder"]["name"], "formats": capabilities.get("formats"),
            "codecs": capabilities.get("codecs"), "exportEncoders": exportable,
            "usableH264Encoders": gst.get("usableH264Encoders"),
            "hostLibraries": media["hostLibraries"], "hostLibrariesBundled": False}


def audit_window_icon(appdir: Path) -> str:
    """Reject an image without the icon AppRun's Wayland window entries name."""
    icon = appdir / WINDOW_ICON
    if not icon.is_file():
        raise ValueError("AppRun's window icon is missing from the image: " + WINDOW_ICON)
    if WINDOW_ICON not in (ROOT / "packaging/appimage/AppRun").read_text():
        raise ValueError("AppRun does not name the audited window icon")
    return WINDOW_ICON


ROOT_DESKTOP_ENTRY = "putgd.desktop"


def keep_one_root_desktop_entry(appdir: Path) -> list[str]:
    """Remove root links to the image's other desktop entries; return their names.

    The AppImage spec allows one desktop entry at the AppDir root, and integrators
    such as AppImageLauncher (libappimage) read whichever they find. A linuxdeploy
    pass without --desktop-file links the first entry it finds in
    usr/share/applications, so the NoDisplay putgd-ui.desktop could join
    putgd.desktop there and stop integration (#306). Only such links are removed:
    usr/share/applications keeps every entry AppRun and the helpers need.
    """
    applications = (appdir / "usr/share/applications").resolve()
    removed = []
    for path in sorted(appdir.glob("*.desktop")):
        if path.name == ROOT_DESKTOP_ENTRY:
            continue
        if not path.is_symlink() or path.resolve().parent != applications:
            raise ValueError("Unexpected desktop entry at the AppDir root: " + path.name)
        path.unlink()
        removed.append(path.name)
    return removed


def audit_root_desktop_entry(appdir: Path) -> str:
    """Reject an AppDir whose root is not exactly one integrable putgd.desktop."""
    entries = sorted(path.name for path in appdir.glob("*.desktop"))
    if entries != [ROOT_DESKTOP_ENTRY]:
        raise ValueError("The AppDir root must hold exactly one desktop entry, " + ROOT_DESKTOP_ENTRY +
                         "; found: " + (", ".join(entries) or "none"))
    entry = appdir / ROOT_DESKTOP_ENTRY
    if not entry.resolve().is_relative_to(appdir.resolve()) or not entry.is_file():
        raise ValueError("The root desktop entry must be a file inside the AppDir")
    group = None
    keys = {}
    for line in entry.read_text().splitlines():
        line = line.strip()
        if line.startswith("["):
            group = line
        elif group == "[Desktop Entry]" and "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            keys[key.strip()] = value.strip()
    if keys.get("Type") != "Application" or not keys.get("Exec"):
        raise ValueError("The root desktop entry must be a launchable application")
    for key in ("NoDisplay", "Hidden"):
        if keys.get(key, "false").lower() != "false":
            raise ValueError("The root desktop entry must not set " + key + ": integrators skip it")
    return ROOT_DESKTOP_ENTRY


def audit_deployed(appdir: Path, inventory: dict) -> dict:
    """Check actual relocated helpers and dynamic dependencies without SDK paths."""
    environment = dict(os.environ, LD_LIBRARY_PATH=str(appdir / "usr/lib") + ":" +
                       str(appdir / inventory["privateLibraryDirectory"]))
    identities = []
    for name in inventory["programs"]:
        if name == "putgd-launcher":
            continue
        executable = appdir / "usr/bin" / name
        result = subprocess.run([str(executable), "--version-json"], check=True,
                                env=environment, capture_output=True, text=True, timeout=30)
        if json.loads(result.stdout) != inventory["manifest"]:
            raise ValueError("Post-deployment executable identity mismatch: " + name)
        identities.append(name)
    media_audit = None
    if "putgd-record" in inventory["programs"]:
        # As mounted: APPDIR makes the helper confine GStreamer to the image
        # (src/record/BundledRuntime.cpp); no display, so nothing is captured.
        media = json.loads(MEDIA.read_text())
        with tempfile.TemporaryDirectory(prefix="pgmedia-") as cache:
            probe_environment = {key: value for key, value in environment.items()
                                 if key not in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE") and not key.startswith("GST_")}
            probe_environment.update(APPDIR=str(appdir), XDG_CACHE_HOME=cache)
            probe = subprocess.run([str(appdir / "usr/bin/putgd-record"), "--capabilities"], check=True,
                                   env=probe_environment, capture_output=True, text=True, timeout=60).stdout
        media_audit = audit_media(appdir, media, probe)
    checked = 0
    for path in appdir.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        dependencies = subprocess.run(["ldd", str(path)], check=True, env=environment,
                                      capture_output=True, text=True).stdout
        if "not found" in dependencies:
            raise ValueError("Unresolved post-deployment dependency: " + str(path.relative_to(appdir)))
        for line in dependencies.splitlines():
            if "=>" not in line:
                continue
            library, target = line.split("=>", 1)
            if library.strip().startswith(("libQt6", "libaws", "libs2n", "libqt6keychain", "libLayerShellQt", "libgst", *FFMPEG_LIBRARIES)):
                resolved = Path(target.strip().split()[0]).resolve()
                if not resolved.is_relative_to(appdir):
                    raise ValueError("Bundled dependency resolves outside AppDir: " + library.strip())
        checked += 1
    result = {"identityMatchedPrograms": identities, "resolvedElfFiles": checked,
              "bundledDependenciesResolvedInsideImage": True,
              "checkedDependencyPrefixes": ["libQt6", "libaws", "libs2n", "libqt6keychain", "libLayerShellQt", "libgst", *FFMPEG_LIBRARIES]}
    if media_audit:
        result["recording"] = media_audit
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True, help="CMake DESTDIR root containing usr/")
    parser.add_argument("--output-dir", type=Path, required=True, help="New output directory")
    parser.add_argument("--tools-dir", type=Path, required=True)
    parser.add_argument("--qt-root", type=Path, required=True, help="Official Qt gcc_64 install of the locked version (tools.lock.json)")
    parser.add_argument("--dependency-prefix", type=Path, required=True, help="QtKeychain/LayerShellQt install prefix")
    parser.add_argument("--license-source", type=Path, action="append", required=True,
                        help="Pinned licence source of a bundled component, named by its id "
                             "(packaging/appimage/components.json or the Qt repository); repeat per source")
    parser.add_argument("--version", required=True)
    parser.add_argument("--require-proton", action="store_true")
    parser.add_argument("--proton-manifest", type=Path, help="Expected provisioned Proton runtime manifest")
    parser.add_argument("--release-tag", help="Embed this release's update information and write its .zsync")
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY") or appimage_update.REPOSITORY,
                        help="GitHub OWNER/NAME whose release serves the image")
    parser.add_argument("--update-origin", help="Lab only: point the update information and .zsync at this server")
    parser.add_argument("--update-base", help="Lab only: with --release-tag, the signer's lab prefix (https://pkg.put.gd/lab/)")
    args = parser.parse_args()
    # Check the native install's identity, modes, runtime receipts and absence of
    # test/private files before deployment changes ELF RPATHs and bundles Qt.
    from audit_native import validate_inventory
    proton_expected = json.loads(args.proton_manifest.read_text()) if args.proton_manifest else None
    inventory = validate_inventory(args.stage, require_kde=True, require_proton=args.require_proton,
                                   expected_proton_manifest=proton_expected)
    if inventory["manifest"]["version"] != args.version:
        raise ValueError("Requested AppImage version differs from installed build")
    if args.release_tag and parse_tag(args.release_tag).version != args.version:
        raise ValueError("Release tag differs from the AppImage version")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    appdir = output / "AppDir"
    shutil.copytree(args.stage, appdir, symlinks=True)
    appdir.chmod(0o755)
    lock, executables = tools(args.tools_dir)
    shutil.copy2(executables["runtime-LICENSE"], appdir / "usr/share/doc/putgd/appimage-runtime-LICENSE")
    qt = args.qt_root.resolve()
    qmake = qt / "bin/qmake"
    gtk = qt / "plugins/platformthemes/libqgtk3.so"
    if not qmake.is_file() or not gtk.is_file():
        raise ValueError("Official Qt must include qmake and the native GTK3 platform theme plugin")
    missing_wayland = [name for name in WAYLAND_CLIENT_PLUGINS if not (qt / "plugins" / name).is_file()]
    if missing_wayland:
        raise ValueError("Official Qt must include the Qt Wayland client plugins: " + ", ".join(missing_wayland))
    qt_version = subprocess.run([str(qmake), "-query", "QT_VERSION"], check=True,
                                text=True, capture_output=True).stdout.strip()
    if qt_version != lock["buildInputs"]["qt"]:
        raise ValueError("AppImage Qt version differs from the locked build baseline")
    dependency_prefix = args.dependency_prefix.resolve()
    layer_plugins = list(dependency_prefix.rglob("liblayer-shell.so"))
    if len(layer_plugins) != 1:
        raise ValueError("Expected one LayerShellQt native Wayland shell plugin")
    desktop = ROOT / "packaging/appimage/putgd.desktop"
    shutil.copy2(desktop, appdir / "usr/share/applications/putgd.desktop")
    deployment_tool = deployment_qmake(qt, output)
    environment = dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1", QMAKE=str(deployment_tool),
                       ARCH="x86_64", EXTRA_PLATFORM_PLUGINS=";".join(EXTRA_PLATFORM_PLUGINS),
                       LD_LIBRARY_PATH=":".join((str(qt / "lib"), str(dependency_prefix / "lib"),
                                                  str(appdir / inventory["privateLibraryDirectory"]),
                                                  os.environ.get("LD_LIBRARY_PATH", ""))))
    selected_plugins = subprocess.run([str(deployment_tool), "-query", "QT_INSTALL_PLUGINS"],
                                      check=True, env=environment, text=True, capture_output=True).stdout.strip()
    if selected_plugins != str(output / "qt-deployment/plugins"):
        raise ValueError("Qt deployment plugin-path override was not applied")
    # Screen recording: putgd-record's plugins and scanner join the first pass so
    # their dependencies resolve with the executables'. Host-matched libraries
    # (VA-API, PipeWire, GPU encoders) are never bundled.
    media = json.loads(MEDIA.read_text())
    media_plugins, media_scanner = media_sources(appdir, media)
    excludes = [argument for pattern in media["hostLibraries"] for argument in ("--exclude-library", pattern)]
    argv = [str(executables["linuxdeploy-x86_64.AppImage"]), "--appdir", str(appdir),
            "--desktop-file", str(desktop), "--icon-file", str(ROOT / "packaging/linux/putgd.svg"),
            "--plugin", "qt", "--custom-apprun", str(ROOT / "packaging/appimage/AppRun"), *excludes]
    for name in inventory["programs"]:
        if name != "putgd-launcher":
            argv += ["--executable", str(appdir / "usr/bin" / name)]
    module = next(appdir.glob("usr/lib*/**/putgd/" + inventory["runtimeModule"]["file"]))
    argv += ["--library", str(module)]
    for path in media_plugins:
        argv += ["--library", str(path)]
    argv += ["--executable", str(media_scanner)]
    subprocess.run(argv, check=True, env=environment)
    media_origins = relocate_media(appdir, media_plugins, media_scanner)
    # Deploy the native theme explicitly, including its ordinary shared-library
    # dependencies; do not replace GTK dialogs with Qt-only fallback dialogs.
    theme = appdir / "usr/plugins/platformthemes/libqgtk3.so"
    theme.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(gtk, theme)
    subprocess.run([str(executables["linuxdeploy-x86_64.AppImage"]), "--appdir", str(appdir),
                    "--library", str(theme), *excludes], check=True, env=environment)
    layer = appdir / "usr/plugins/wayland-shell-integration/liblayer-shell.so"
    layer.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(layer_plugins[0], layer)
    subprocess.run([str(executables["linuxdeploy-x86_64.AppImage"]), "--appdir", str(appdir),
                    "--library", str(layer), *excludes], check=True, env=environment)
    # Its build RUNPATH names the CI's dependency prefix and Qt install (#421).
    appimage_notices.set_origin_runpath(appdir, layer)
    wayland = []
    for name in WAYLAND_CLIENT_PLUGINS:
        wayland += ["--library", str(appdir / "usr/plugins" / name)]
        (appdir / "usr/plugins" / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(qt / "plugins" / name, appdir / "usr/plugins" / name)
    subprocess.run([str(executables["linuxdeploy-x86_64.AppImage"]), "--appdir", str(appdir), *wayland, *excludes],
                   check=True, env=environment)
    fallbacks = stage_host_fallbacks(appdir, media)
    gpl = gpl_components(appdir, media)
    write_gpl_notice(appdir, gpl, lock["buildInputs"]["ffmpeg"], args.version)
    # linuxdeploy may wrap custom launchers after each pass. Install ours last:
    # its environment and KDE supervision are the application runtime contract.
    shutil.copy2(ROOT / "packaging/appimage/AppRun", appdir / "AppRun")
    (appdir / "AppRun").chmod(0o755)
    removed_root_entries = keep_one_root_desktop_entry(appdir)
    distro_licenses = {}

    def distro_packages(root: Path, claimed: set[str]) -> dict[str, list[str]]:
        distro_licenses.update(system_library_licenses(root, claimed, media_origins))
        return distro_licenses

    # Native DEP-5 metadata includes distro-owned runtime claims. The AppImage
    # generates its own complete notices and source inventory below.
    for native_doc in ("copyright", "changelog.gz"):
        (appdir / "usr/share/doc/putgd" / native_doc).unlink(missing_ok=True)
    notices = appimage_notices.finish(appdir, qt=qt, lock=lock, license_sources=args.license_source,
                                      distro_packages=distro_packages, version=args.version,
                                      build=inventory["manifest"], gpl=gpl)
    refresh_runtime_manifest(appdir)
    deployed = audit_deployed(appdir, inventory)
    deployed["qtPlatforms"] = audit_platforms(appdir, inventory)
    deployed["qtWayland"] = audit_wayland(appdir, inventory)
    deployed["windowIcon"] = audit_window_icon(appdir)
    deployed["rootDesktopEntry"] = {"file": audit_root_desktop_entry(appdir),
                                    "removedLinks": removed_root_entries}
    proton = audit_proton(appdir, required=args.require_proton,
                          expected_manifest=inventory.get("proton", {}).get("manifest"))
    if proton:
        deployed["proton"] = proton
    deployed["notices"] = appimage_notices.audit(appdir)
    target = output / ("putgd-" + args.version + "-x86_64.AppImage")
    update = appimage_update.targets(target.name, args.release_tag, args.repository, args.update_origin, args.update_base)
    subprocess.run([str(executables["appimagetool-x86_64.AppImage"]), "--runtime-file",
                    str(executables["runtime-x86_64"]), "--mksquashfs-opt=-b",
                    "--mksquashfs-opt=16384", *(("-u", update[0]) if update else ()), str(appdir), str(target)],
                   check=True, env=environment, cwd=output)
    target.chmod(0o755)
    if appimage_update.read_update_information(target) != (update[0] if update else ""):
        raise ValueError("AppImage update information differs from the requested value")
    zsync = output / appimage_update.zsync_name(target.name)
    # With -u, appimagetool runs its bundled zsyncmake in its working directory,
    # naming the image by a relative URL. Ours names the image's absolute URL on
    # the signer's mirror, so it works behind the site's redirect and needs no
    # redirect itself (#297).
    zsync.unlink(missing_ok=True)
    if update:
        subprocess.run([str(executables["zsyncmake"]), "-u", update[1], "-o", str(zsync), target.name],
                       check=True, cwd=output, stdout=subprocess.DEVNULL)
        appimage_update.verify_zsync(zsync, target, target.name, update[1])
    files = [path for path in appdir.rglob("*") if path.is_file()]
    receipt = {"schemaVersion": 1, "format": "AppImage", "architecture": "x86_64",
               "build": inventory["manifest"], "toolLock": lock,
               "compression": {"codec": "zstd", "squashfsBlockBytes": 16384},
               "artifact": {"file": target.name, "sha256": digest(target),
                            "bytes": target.stat().st_size, "mode": "0755"},
               "update": {"information": update[0],
                          "zsync": {"file": zsync.name, "url": update[1], "sha256": digest(zsync),
                                    "bytes": zsync.stat().st_size}} if update else None,
               "postDeploymentAudit": deployed,
               "recordingRuntime": {"media": media, "hostFallbackLibraries": fallbacks,
                                    "ffmpeg": lock["buildInputs"]["ffmpeg"], "gplComponents": gpl,
                                    "license": "GPL-2.0-or-later combined work"},
               "notices": notices,
               "bundledSourceLicenses": sorted(name for component in notices["inventory"]["components"]
                                               if not component["id"].startswith("ubuntu:")
                                               for name in component["licenseFiles"]),
               "distroLicensePackages": sorted(distro_licenses),
               "inventory": [{"path": str(path.relative_to(appdir)), "sha256": digest(path),
                              "bytes": path.stat().st_size} for path in sorted(files)],
               "desktopAcceptance": "pending"}
    (output / "appimage-build.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
