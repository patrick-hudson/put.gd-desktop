#!/usr/bin/env python3
"""Audit a native DEB/RPM's extracted contents. This is not desktop acceptance."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess

try:
    from audit_proton import audit_proton
except ModuleNotFoundError:
    from tools.releases.audit_proton import audit_proton

RESIDENT_FORBIDDEN_NEEDED = re.compile(r"^lib(Qt6Network|ssl|crypto|gtk)")
# The interface helper reaches the resident over a native Unix socket (#1161).
UI_FORBIDDEN_NEEDED = re.compile(r"^libQt6Network")
PROGRAMS = ("putgd", "putgd-cli", "putgd-ui", "putgd-capture", "putgd-s3-worker", "putgd-b2-worker", "putgd-http-worker", "putgd-launcher", "putgd-ocr")
SDK_LICENSES = ("aws-sdk-LICENSE", "aws-crt-cpp-LICENSE", *(
    "aws-crt-cpp-crt-" + name + "-LICENSE" for name in (
        "aws-c-auth", "aws-c-cal", "aws-c-common", "aws-c-compression", "aws-c-event-stream",
        "aws-c-http", "aws-c-io", "aws-c-mqtt", "aws-c-s3", "aws-c-sdkutils", "aws-checksums", "s2n")),
    # Apache-2.0 NOTICE files and the notices of code compiled into the SDK and CRT (#383).
    "aws-sdk-NOTICE", "aws-crt-cpp-NOTICE", *(
    "aws-crt-cpp-crt-" + name + "-NOTICE" for name in (
        "aws-c-auth", "aws-c-cal", "aws-c-common", "aws-c-compression", "aws-c-event-stream",
        "aws-c-http", "aws-c-io", "aws-c-mqtt", "aws-c-s3", "aws-c-sdkutils", "s2n")),
    "aws-sdk-cJSON-LICENSE", "aws-sdk-TinyXML2-LICENSE", "aws-crt-cpp-crt-aws-c-common-THIRD-PARTY-LICENSES",
    "aws-crt-cpp-crt-aws-checksums-xxHash-LICENSE")
DESKTOP_ASSETS = ("share/applications/putgd.desktop", "share/applications/putgd-ui.desktop",
                  "share/icons/hicolor/scalable/apps/putgd.svg",
                  "share/gnome-shell/extensions/putgd@put.gd/extension.js",
                  "share/gnome-shell/extensions/putgd@put.gd/metadata.json",
                  "share/putgd/autostart/putgd.desktop",
                  # AppStream: software centres list put.gd and its updates as an application.
                  "share/metainfo/gd.put.Putgd.metainfo.xml")


OCR_LIBRARIES = ("libtesseract", "liblept", "libZXing")
OCR_FORBIDDEN_NEEDED = re.compile(r"^libQt6(Gui|Sql|Network|DBus)")


def audit_ocr_dependencies(name: str, needed: list[str]) -> None:
    if name == "putgd-ocr":
        forbidden = [lib for lib in needed if OCR_FORBIDDEN_NEEDED.match(lib)]
    else:
        forbidden = [lib for lib in needed if lib.startswith(OCR_LIBRARIES)]
    if forbidden:
        raise ValueError(f"OCR dependency isolation violated by {name}: {', '.join(forbidden)}")


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate_inventory(root: Path, expected_manifest: dict | None = None, *, require_kde=False,
                       require_proton=False, expected_proton_manifest: dict | None = None) -> dict:
    root = root.resolve()
    files = sorted(path for path in root.rglob("*") if not path.is_dir())
    for path in files:
        name = path.name.lower()
        relative = path.relative_to(root)
        if name == "credentials.txt" or "fixture" in name or "bench" in name or any(
                part in ("tests", "tools", ".context", "work", ".aws") for part in relative.parts):
            raise ValueError(f"Forbidden release file: {relative}")
        if name.startswith("libqt6"):
            raise ValueError(f"Bundled Qt in native package: {relative}")
        if not path.resolve().is_relative_to(root):
            raise ValueError(f"Package symlink escapes extracted tree: {relative}")
    proton = audit_proton(root, required=require_proton, expected_manifest=expected_proton_manifest)
    programs = list(PROGRAMS)
    if require_kde or (root / "usr/bin/putgd-region").exists():
        programs.append("putgd-region")
    keeper = ["usr/bin/putgd-clipboard-keeper"] if (root / "usr/bin/putgd-region").exists() else []  # Budgie and Hyprland quit path; not a Qt product
    if (root / "usr/bin/putgd-record").exists():
        programs.append("putgd-record")  # optional screen-recording helper (docs/design/recording.md)
    required = ["usr/bin/" + name for name in programs] + keeper
    required += ["usr/" + name for name in DESKTOP_ASSETS]
    required += ["usr/share/putgd/" + name for name in
                 ("build-info.json", "dependency-lock.json", "aws-runtime-manifest.json")]
    required += ["usr/share/doc/putgd/" + name for name in
                 ("LICENSE", "runtime-notices.txt", "copyright", "changelog.gz", "wayland-ext-data-control-v1.xml",
                  "wayland-wlr-data-control-unstable-v1.xml", "wayland-ext-image-copy-capture-v1.xml",
                  "wayland-ext-image-capture-source-v1.xml", "wayland-ext-foreign-toplevel-list-v1.xml",
                  "wayland-wlr-foreign-toplevel-management-unstable-v1.xml", "wayland-hyprland-global-shortcuts-v1.xml",
                  "wayland-hyprland-toplevel-mapping-v1.xml", "wayland-wlr-layer-shell-unstable-v1.xml",
                  "wayland-wlr-virtual-pointer-unstable-v1.xml", "wayland-fake-input.xml", *SDK_LICENSES)]
    for name in required:
        path = root / name
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Required release file missing or empty: {name}")
    copyright_text = (root / "usr/share/doc/putgd/copyright").read_text()
    if not copyright_text.startswith("Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"):
        raise ValueError("Native copyright is not DEP-5")
    changelog = gzip.decompress((root / "usr/share/doc/putgd/changelog.gz").read_bytes()).decode()
    if not changelog.startswith("putgd ("):
        raise ValueError("Native changelog is invalid")
    for name in [*programs, *(Path(path).name for path in keeper)]:
        mode = stat.S_IMODE((root / "usr/bin" / name).stat().st_mode)
        if mode != 0o755:
            raise ValueError(f"Executable mode must be 0755: {name} ({mode:04o})")
    manifest = json.loads((root / "usr/share/putgd/build-info.json").read_text())
    if expected_manifest is not None and manifest != expected_manifest:
        raise ValueError("Build manifest mismatch against the release build")
    runtime = json.loads((root / "usr/share/putgd/aws-runtime-manifest.json").read_text())
    module = runtime["module"]
    module_paths = list(root.glob("usr/lib*/**/putgd/" + module["file"]))
    if len(module_paths) != 1:
        raise ValueError("Expected one private S3 module")
    private = module_paths[0].parent
    records = [*runtime["libraries"], module]
    if not runtime["libraries"] or module["contractVersion"] != 2 or not re.fullmatch(r"[0-9a-f]{64}", module["buildIdentity"]):
        raise ValueError("Incomplete AWS runtime/module manifest")
    for record in records:
        if Path(record["file"]).name != record["file"]:
            raise ValueError("Runtime manifest file is not a basename")
        path = private / record["file"]
        if not path.is_file() or sha256(path) != record["sha256"]:
            raise ValueError(f"Runtime digest mismatch: {record['file']}")
    recorded = {record["file"] for record in records}
    if any(path.name not in recorded for path in private.iterdir() if path.is_file()):
        raise ValueError("Unrecorded private runtime library")
    return {**({"proton": proton} if proton else {}), "manifest": manifest, "programs": programs, "keeper": keeper,
            "privateLibraryDirectory": str(private.relative_to(root)),
            "runtimeModule": module,
            "inventory": [{"path": str(path.relative_to(root)), "size": path.stat().st_size,
                           "mode": f"{stat.S_IMODE(path.stat().st_mode):04o}", "sha256": sha256(path)}
                          for path in files]}


def files_in_tree(root: Path):
    for path in root.rglob("*"):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as stream:
                if stream.read(4) == b"\x7fELF":
                    yield path


def command(argv: list[str], *, env=None) -> str:
    result = subprocess.run(argv, check=True, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env, timeout=30)
    return result.stdout.strip()


def audit_tree(root: Path, expected_manifest=None, *, require_kde=False, require_proton=False,
               expected_proton_manifest: dict | None = None, package_target: str | None = None) -> dict:
    root = root.resolve()
    result = validate_inventory(root, expected_manifest, require_kde=require_kde,
                                require_proton=require_proton, expected_proton_manifest=expected_proton_manifest)
    private = root / result["privateLibraryDirectory"]
    env = {**os.environ, "LD_LIBRARY_PATH": str(private)}
    result["compiledIdentities"] = {}
    result["elfDependencies"] = {}
    for relative in result.get("keeper", []):
        # Not a Qt product: no crash handler or version to check, but hardened like the rest.
        keeper_path = root / relative
        if not re.search(r"Type:\s+DYN\b", command(["readelf", "-hW", str(keeper_path)])):
            raise ValueError(f"Native executable is not PIE: {relative}")
        if "GNU_RELRO" not in command(["readelf", "-lW", str(keeper_path)]) or not re.search(r"BIND_NOW|FLAGS.*\bNOW\b", command(["readelf", "-dW", str(keeper_path)])):
            raise ValueError(f"Native executable lacks full RELRO: {relative}")
        resolved = command(["ldd", str(keeper_path)], env=env)
        if "not found" in resolved:
            raise ValueError(f"Unresolved native dependency: {keeper_path.name}\n{resolved}")
        result["elfDependencies"][relative] = {
            "needed": re.findall(r"\(NEEDED\).*?\[(.*?)\]", command(["readelf", "-d", str(keeper_path)])),
            "resolved": resolved.splitlines()}
    for name in result["programs"]:
        if name == "putgd-launcher":
            continue
        path = root / "usr/bin" / name
        header = command(["readelf", "-hW", str(path)])
        segments = command(["readelf", "-lW", str(path)])
        dynamic = command(["readelf", "-dW", str(path)])
        if not re.search(r"Type:\s+DYN\b", header):
            raise ValueError(f"Native executable is not PIE: {name}")
        if "GNU_RELRO" not in segments or not re.search(r"BIND_NOW|FLAGS.*\bNOW\b", dynamic):
            raise ValueError(f"Native executable lacks full RELRO: {name}")
        # The test-only crash trigger (#124) is compiled out of every release build.
        if b"PUTGD_TEST_CRASH" in path.read_bytes():
            raise ValueError(f"Release binary contains the test-only crash trigger: {name}")
        # The crash handler's exported pthread_create gives every thread an
        # alternate signal stack, so worker stack overflows are recorded (#156).
        if name != "putgd-cli" and not re.search(r"^\s*\d+:\s+\S+\s+\d+\s+FUNC\s+GLOBAL\s+\S+\s+\d+\s+pthread_create(@|\s|$)",
                                                  command(["readelf", "--dyn-syms", "-W", str(path)]), re.M):
            raise ValueError(f"Release binary does not export the crash handler's pthread_create: {name}")
        identity = json.loads(command([str(path), "--version-json"], env=env))
        if identity != result["manifest"]:
            raise ValueError(f"Compiled build manifest mismatch: {name}")
        if name not in ("putgd-s3-worker", "putgd-b2-worker", "putgd-http-worker"):
            version = command([str(path), "--version"], env=env)
            if version != name + " " + result["manifest"]["version"]:
                raise ValueError(f"Compiled version mismatch: {name}")
        result["compiledIdentities"][name] = identity
    # The update check offers this package's own download (#293); a build
    # without its target would never check.
    update = json.loads(command([str(root / "usr/bin/putgd-cli"), "--update-info"], env=env))
    if package_target is not None and update.get("packageTarget") != package_target:
        raise ValueError(f"Update check package target mismatch: {update.get('packageTarget')!r} is not {package_target!r}")
    result["updateCheck"] = update
    for path in [*(root / "usr/bin" / name for name in result["programs"] if name != "putgd-launcher"),
                 *(path for path in private.iterdir() if path.is_file()),
                 *([root / result["proton"]["helper"]] if "proton" in result else [])]:
        dynamic = command(["readelf", "-d", str(path)])
        needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
        # The resident stays small at idle (acceptance matrix gate C): no
        # QtNetwork, OpenSSL or GTK may be a direct dependency of usr/bin/putgd.
        # Networking lives in putgd-cli and the worker processes (#1149).
        if path == root / "usr/bin/putgd":
            if forbidden := [soname for soname in needed if RESIDENT_FORBIDDEN_NEEDED.match(soname)]:
                raise ValueError(f"Resident links a library gate C forbids: {', '.join(forbidden)}")
        if path == root / "usr/bin/putgd-ui":
            if forbidden := [soname for soname in needed if UI_FORBIDDEN_NEEDED.match(soname)]:
                raise ValueError(f"Interface helper links a networking library: {', '.join(forbidden)}")
        audit_ocr_dependencies(path.name, needed)
        resolved = command(["ldd", str(path)], env=env)
        if "not found" in resolved:
            raise ValueError(f"Unresolved native dependency: {path.name}\n{resolved}")
        for soname, location in re.findall(r"^\s*(\S+)\s+=>\s+(\S+)", resolved, re.MULTILINE):
            if soname.startswith(("libaws", "libs2n")) and not Path(location).resolve().is_relative_to(private.resolve()):
                raise ValueError(f"Private SDK dependency resolved outside package: {soname} -> {location}")
        if path.name == result["runtimeModule"]["file"] and not re.search(r"\(RPATH\).*?\[\$ORIGIN\]", dynamic):
            raise ValueError("Installed S3 module must retain transitive $ORIGIN RPATH")
        result["elfDependencies"][str(path.relative_to(root))] = {"needed": needed, "resolved": resolved.splitlines()}
    for path in files_in_tree(root):
        needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", command(["readelf", "-d", str(path)]))
        audit_ocr_dependencies(path.name, needed)
    result["schemaVersion"] = 1
    result["scope"] = "native extracted package identity/inventory/dependencies; not desktop acceptance"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--manifest", type=Path, help="Expected build-info.json from the release build")
    parser.add_argument("--require-kde", action="store_true")
    parser.add_argument("--require-proton", action="store_true")
    parser.add_argument("--proton-manifest", type=Path, help="Expected provisioned Proton runtime manifest")
    parser.add_argument("--package-target", help="Expected PUTGD_PACKAGE_TARGET, such as debian13")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    expected = json.loads(args.manifest.read_text()) if args.manifest else None
    proton_expected = json.loads(args.proton_manifest.read_text()) if args.proton_manifest else None
    result = audit_tree(args.root, expected, require_kde=args.require_kde,
                        require_proton=args.require_proton, expected_proton_manifest=proton_expected,
                        package_target=args.package_target)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
