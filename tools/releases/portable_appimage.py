#!/usr/bin/env python3
"""Assemble an already installed release with pinned, official AppImage tooling."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request

from audit_native import sha256 as digest

ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / "packaging/appimage/tools.lock.json"


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
                urllib.request.urlretrieve(item["url"], temporary)
                if digest(temporary) != item["sha256"]:
                    raise ValueError("Official deployment tool digest mismatch: " + item["name"])
                temporary.replace(path)
            finally:
                temporary.unlink(missing_ok=True)
        path.chmod(0o755 if item.get("executable", True) else 0o644)
        result[item["name"]] = path.resolve()
    return lock, result


def copy_licenses(sources: list[Path], destination: Path) -> list[str]:
    copied = []
    for source in sources:
        source = source.resolve()
        matches = [path for path in source.rglob("*") if path.is_file() and
                   (path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "COPYRIGHT")) or
                    path.name == "qt_attribution.json")]
        if not matches:
            raise ValueError("No licenses found in source: " + str(source))
        for path in matches:
            target = destination / source.name / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            copied.append(str(target.relative_to(destination)))
    return sorted(copied)


def system_library_licenses(appdir: Path, qt: Path) -> list[str]:
    """Retain the distro copyright/license notice for each deployed system ELF."""
    qt_names = {path.name for path in qt.rglob("*") if path.is_file()}
    cache = subprocess.run(["/sbin/ldconfig", "-p"], check=True, text=True,
                           capture_output=True).stdout
    origins = {}
    for line in cache.splitlines():
        if " => " in line:
            name, source = line.split(" => ", 1)
            original = Path(source.strip())
            origins.setdefault(name.split()[0], original)
            origins.setdefault(original.resolve().name, original)
    copied = set()
    for path in appdir.rglob("*"):
        if not path.is_file():
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        name = path.name
        if name in qt_names or name.startswith(("putgd", "libputgd", "libaws", "libs2n",
                                                  "libqt6keychain", "libLayerShellQt", "liblayer-shell")):
            continue  # Exact pinned upstream notices are already installed.
        source = origins.get(name)
        if source is None:
            raise ValueError("Cannot identify deployed system library: " + name)
        owner = subprocess.run(["dpkg-query", "-S", str(source)], text=True, capture_output=True)
        if owner.returncode:
            owner = subprocess.run(["dpkg-query", "-S", str(source.resolve())], text=True, capture_output=True)
        if owner.returncode:
            raise ValueError("Cannot identify distro license for: " + str(source))
        package = owner.stdout.split(": ", 1)[0].split(":", 1)[0]
        if package in copied:
            continue
        notice = Path("/usr/share/doc") / package / "copyright"
        if not notice.is_file():
            raise ValueError("Distro copyright notice missing: " + package)
        destination = appdir / "usr/share/doc/putgd/bundled-licenses/distro" / package / "copyright"
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(notice, destination)
        copied.add(package)
    common = Path("/usr/share/common-licenses")
    if common.is_dir():
        shutil.copytree(common, appdir / "usr/share/doc/putgd/bundled-licenses/distro-common",
                        dirs_exist_ok=True)
    return sorted(copied)



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
            if library.strip().startswith(("libQt6", "libaws", "libs2n", "libqt6keychain", "libLayerShellQt")):
                resolved = Path(target.strip().split()[0]).resolve()
                if not resolved.is_relative_to(appdir):
                    raise ValueError("Bundled dependency resolves outside AppDir: " + library.strip())
        checked += 1
    return {"identityMatchedPrograms": identities, "resolvedElfFiles": checked,
            "bundledDependenciesResolvedInsideImage": True,
            "checkedDependencyPrefixes": ["libQt6", "libaws", "libs2n", "libqt6keychain", "libLayerShellQt"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True, help="CMake DESTDIR root containing usr/")
    parser.add_argument("--output-dir", type=Path, required=True, help="New output directory")
    parser.add_argument("--tools-dir", type=Path, required=True)
    parser.add_argument("--qt-root", type=Path, required=True, help="Official Qt 6.8.3 gcc_64 install")
    parser.add_argument("--dependency-prefix", type=Path, required=True, help="QtKeychain/LayerShellQt install prefix")
    parser.add_argument("--license-source", type=Path, action="append", required=True,
                        help="Pinned Qt/module, QtKeychain, LayerShellQt source; repeat per source")
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    # Check the native install's identity, modes, runtime receipts and absence of
    # test/private files before deployment changes ELF RPATHs and bundles Qt.
    from audit_native import validate_inventory
    inventory = validate_inventory(args.stage, require_kde=True)
    if inventory["manifest"]["version"] != args.version:
        raise ValueError("Requested AppImage version differs from installed build")
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
    qt_version = subprocess.run([str(qmake), "-query", "QT_VERSION"], check=True,
                                text=True, capture_output=True).stdout.strip()
    if qt_version != lock["buildInputs"]["qt"]:
        raise ValueError("AppImage Qt version differs from the locked build baseline")
    dependency_prefix = args.dependency_prefix.resolve()
    layer_plugins = list(dependency_prefix.rglob("liblayer-shell.so"))
    if len(layer_plugins) != 1:
        raise ValueError("Expected one LayerShellQt native Wayland shell plugin")
    licenses = copy_licenses(args.license_source, appdir / "usr/share/doc/putgd/bundled-licenses")
    desktop = ROOT / "packaging/appimage/putgd.desktop"
    shutil.copy2(desktop, appdir / "usr/share/applications/putgd.desktop")
    deployment_tool = deployment_qmake(qt, output)
    environment = dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1", QMAKE=str(deployment_tool),
                       ARCH="x86_64", EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so",
                       LD_LIBRARY_PATH=":".join((str(qt / "lib"), str(dependency_prefix / "lib"),
                                                  str(appdir / inventory["privateLibraryDirectory"]),
                                                  os.environ.get("LD_LIBRARY_PATH", ""))))
    selected_plugins = subprocess.run([str(deployment_tool), "-query", "QT_INSTALL_PLUGINS"],
                                      check=True, env=environment, text=True, capture_output=True).stdout.strip()
    if selected_plugins != str(output / "qt-deployment/plugins"):
        raise ValueError("Qt deployment plugin-path override was not applied")
    argv = [str(executables["linuxdeploy-x86_64.AppImage"]), "--appdir", str(appdir),
            "--desktop-file", str(desktop), "--icon-file", str(ROOT / "packaging/linux/putgd.svg"),
            "--plugin", "qt", "--custom-apprun", str(ROOT / "packaging/appimage/AppRun")]
    for name in inventory["programs"]:
        if name != "putgd-launcher":
            argv += ["--executable", str(appdir / "usr/bin" / name)]
    module = next(appdir.glob("usr/lib*/**/putgd/" + inventory["runtimeModule"]["file"]))
    argv += ["--library", str(module)]
    subprocess.run(argv, check=True, env=environment)
    # Deploy the native theme explicitly, including its ordinary shared-library
    # dependencies; do not replace GTK dialogs with Qt-only fallback dialogs.
    theme = appdir / "usr/plugins/platformthemes/libqgtk3.so"
    theme.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(gtk, theme)
    subprocess.run([str(executables["linuxdeploy-x86_64.AppImage"]), "--appdir", str(appdir),
                    "--library", str(theme)], check=True, env=environment)
    layer = appdir / "usr/plugins/wayland-shell-integration/liblayer-shell.so"
    layer.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(layer_plugins[0], layer)
    subprocess.run([str(executables["linuxdeploy-x86_64.AppImage"]), "--appdir", str(appdir),
                    "--library", str(layer)], check=True, env=environment)
    # linuxdeploy may wrap custom launchers after each pass. Install ours last:
    # its environment and KDE supervision are the application runtime contract.
    shutil.copy2(ROOT / "packaging/appimage/AppRun", appdir / "AppRun")
    (appdir / "AppRun").chmod(0o755)
    distro_licenses = system_library_licenses(appdir, qt)
    refresh_runtime_manifest(appdir)
    deployed = audit_deployed(appdir, inventory)
    target = output / ("putgd-" + args.version + "-x86_64.AppImage")
    subprocess.run([str(executables["appimagetool-x86_64.AppImage"]), "--runtime-file",
                    str(executables["runtime-x86_64"]), "--mksquashfs-opt=-b",
                    "--mksquashfs-opt=16384", str(appdir), str(target)],
                   check=True, env=environment)
    target.chmod(0o755)
    files = [path for path in appdir.rglob("*") if path.is_file()]
    receipt = {"schemaVersion": 1, "format": "AppImage", "architecture": "x86_64",
               "build": inventory["manifest"], "toolLock": lock,
               "compression": {"codec": "zstd", "squashfsBlockBytes": 16384},
               "artifact": {"file": target.name, "sha256": digest(target),
                            "bytes": target.stat().st_size, "mode": "0755"},
               "postDeploymentAudit": deployed,
               "bundledSourceLicenses": licenses, "distroLicensePackages": distro_licenses,
               "inventory": [{"path": str(path.relative_to(appdir)), "sha256": digest(path),
                              "bytes": path.stat().st_size} for path in sorted(files)],
               "desktopAcceptance": "pending"}
    (output / "appimage-build.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
