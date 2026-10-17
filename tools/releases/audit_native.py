#!/usr/bin/env python3
"""Audit a native DEB/RPM's extracted contents. This is not desktop acceptance."""
from __future__ import annotations

import argparse
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

PROGRAMS = ("putgd", "putgd-cli", "putgd-ui", "putgd-capture", "putgd-s3-worker", "putgd-launcher")
SDK_LICENSES = ("aws-sdk-LICENSE", "aws-crt-cpp-LICENSE", *(
    "aws-crt-cpp-crt-" + name + "-LICENSE" for name in (
        "aws-c-auth", "aws-c-cal", "aws-c-common", "aws-c-compression", "aws-c-event-stream",
        "aws-c-http", "aws-c-io", "aws-c-mqtt", "aws-c-s3", "aws-c-sdkutils", "aws-checksums", "s2n")))
DESKTOP_ASSETS = ("share/applications/putgd.desktop", "share/applications/putgd-ui.desktop",
                  "share/icons/hicolor/scalable/apps/putgd.svg",
                  "share/gnome-shell/extensions/putgd@put.gd/extension.js",
                  "share/gnome-shell/extensions/putgd@put.gd/metadata.json",
                  "share/putgd/autostart/putgd.desktop")


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
    if require_kde or (root / "usr/bin/putgd-kde-region").exists():
        programs.append("putgd-kde-region")
    required = ["usr/bin/" + name for name in programs]
    required += ["usr/" + name for name in DESKTOP_ASSETS]
    required += ["usr/share/putgd/" + name for name in
                 ("build-info.json", "dependency-lock.json", "aws-runtime-manifest.json")]
    required += ["usr/share/doc/putgd/" + name for name in
                 ("LICENSE", "runtime-notices.txt", "wayland-ext-data-control-v1.xml",
                  "wayland-wlr-data-control-unstable-v1.xml", *SDK_LICENSES)]
    for name in required:
        path = root / name
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Required release file missing or empty: {name}")
    for name in programs:
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
    if not runtime["libraries"] or module["contractVersion"] != 1 or not re.fullmatch(r"[0-9a-f]{64}", module["buildIdentity"]):
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
    return {**({"proton": proton} if proton else {}), "manifest": manifest, "programs": programs,
            "privateLibraryDirectory": str(private.relative_to(root)),
            "runtimeModule": module,
            "inventory": [{"path": str(path.relative_to(root)), "size": path.stat().st_size,
                           "mode": f"{stat.S_IMODE(path.stat().st_mode):04o}", "sha256": sha256(path)}
                          for path in files]}


def command(argv: list[str], *, env=None) -> str:
    result = subprocess.run(argv, check=True, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env, timeout=30)
    return result.stdout.strip()


def audit_tree(root: Path, expected_manifest=None, *, require_kde=False, require_proton=False,
               expected_proton_manifest: dict | None = None) -> dict:
    root = root.resolve()
    result = validate_inventory(root, expected_manifest, require_kde=require_kde,
                                require_proton=require_proton, expected_proton_manifest=expected_proton_manifest)
    private = root / result["privateLibraryDirectory"]
    env = {**os.environ, "LD_LIBRARY_PATH": str(private)}
    result["compiledIdentities"] = {}
    result["elfDependencies"] = {}
    for name in result["programs"]:
        if name == "putgd-launcher":
            continue
        path = root / "usr/bin" / name
        identity = json.loads(command([str(path), "--version-json"], env=env))
        if identity != result["manifest"]:
            raise ValueError(f"Compiled build manifest mismatch: {name}")
        if name != "putgd-s3-worker":
            version = command([str(path), "--version"], env=env)
            if version != name + " " + result["manifest"]["version"]:
                raise ValueError(f"Compiled version mismatch: {name}")
        result["compiledIdentities"][name] = identity
    for path in [*(root / "usr/bin" / name for name in result["programs"] if name != "putgd-launcher"),
                 *(path for path in private.iterdir() if path.is_file()),
                 *([root / result["proton"]["helper"]] if "proton" in result else [])]:
        dynamic = command(["readelf", "-d", str(path)])
        needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
        resolved = command(["ldd", str(path)], env=env)
        if "not found" in resolved:
            raise ValueError(f"Unresolved native dependency: {path.name}\n{resolved}")
        for soname, location in re.findall(r"^\s*(\S+)\s+=>\s+(\S+)", resolved, re.MULTILINE):
            if soname.startswith(("libaws", "libs2n")) and not Path(location).resolve().is_relative_to(private.resolve()):
                raise ValueError(f"Private SDK dependency resolved outside package: {soname} -> {location}")
        if path.name == result["runtimeModule"]["file"] and not re.search(r"\(RPATH\).*?\[\$ORIGIN\]", dynamic):
            raise ValueError("Installed S3 module must retain transitive $ORIGIN RPATH")
        result["elfDependencies"][str(path.relative_to(root))] = {"needed": needed, "resolved": resolved.splitlines()}
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
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    expected = json.loads(args.manifest.read_text()) if args.manifest else None
    proton_expected = json.loads(args.proton_manifest.read_text()) if args.proton_manifest else None
    result = audit_tree(args.root, expected, require_kde=args.require_kde,
                        require_proton=args.require_proton, expected_proton_manifest=proton_expected)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
