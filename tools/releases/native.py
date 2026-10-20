#!/usr/bin/env python3
"""Package an already-built Release tree with CPack, extract and audit its DEB/RPM."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tarfile

from audit_native import audit_tree, command, sha256


def package_metadata(package: Path, kind: str) -> dict:
    if kind == "DEB":
        with subprocess.Popen(["dpkg-deb", "--fsys-tarfile", str(package)], stdout=subprocess.PIPE) as process:
            with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
                for entry in archive:
                    if entry.uid != 0 or entry.gid != 0:
                        raise ValueError(f"DEB entry is not root-owned: {entry.name}")
                    if entry.isdir() and entry.mode != 0o755:
                        raise ValueError(f"DEB directory mode is not 0755: {entry.name}")
            if process.wait() != 0:
                raise ValueError("Cannot inspect DEB ownership and modes")
        return {"format": "DEB", "name": command(["dpkg-deb", "-f", str(package), "Package"]),
                "version": command(["dpkg-deb", "-f", str(package), "Version"]),
                "architecture": command(["dpkg-deb", "-f", str(package), "Architecture"]),
                "requires": command(["dpkg-deb", "-f", str(package), "Depends"]),
                "recommends": command(["dpkg-deb", "-f", str(package), "Recommends"])}
    listing = command(["rpm", "-qp", "--qf",
                       "[%{FILENAMES}\t%{FILEMODES:octal}\t%{FILEUSERNAME}\t%{FILEGROUPNAME}\n]", str(package)])
    for line in listing.splitlines():
        name, mode, user, group = line.split("\t")
        if user != "root" or group != "root":
            raise ValueError(f"RPM entry is not root-owned: {name}")
        bits = int(mode, 8)
        if stat.S_ISDIR(bits) and stat.S_IMODE(bits) != 0o755:
            raise ValueError(f"RPM directory mode is not 0755: {name}")
    requires = command(["rpm", "-qp", "--requires", str(package)]).splitlines()
    provides = command(["rpm", "-qp", "--provides", str(package)]).splitlines()
    if any(value.startswith(("libaws", "libs2n")) for value in requires + provides):
        raise ValueError("RPM exposes or requires private AWS SONAMEs")
    return {"format": "RPM", "name": command(["rpm", "-qp", "--qf", "%{NAME}", str(package)]),
            "version": command(["rpm", "-qp", "--qf", "%{VERSION}", str(package)]),
            "release": command(["rpm", "-qp", "--qf", "%{RELEASE}", str(package)]),
            "architecture": command(["rpm", "-qp", "--qf", "%{ARCH}", str(package)]),
            "requires": requires, "provides": provides,
            "recommends": command(["rpm", "-qp", "--recommends", str(package)]).splitlines()}


# Qt loads image plugins by name, so neither dpkg-shlibdeps nor RPM's
# dependency generator sees them: History reads WebP recordings and images
# through the WebP plugin (#189), which the distros package apart from Qt GUI.
RUNTIME_PACKAGES = {"DEB": ("qt6-image-formats-plugins",), "RPM": ("qt6-qtimageformats",)}


def require_runtime_packages(metadata: dict) -> None:
    requires = metadata["requires"]
    entries = requires.split(",") if isinstance(requires, str) else requires
    names = {alternative.split()[0] for entry in entries for alternative in entry.split("|") if alternative.strip()}
    missing = [name for name in RUNTIME_PACKAGES[metadata["format"]] if name not in names]
    if missing:
        raise ValueError(metadata["format"] + " does not require " + ", ".join(missing))


def extract_package(package: Path, kind: str, root: Path):
    root.mkdir(mode=0o755)
    if kind == "DEB":
        subprocess.run(["dpkg-deb", "-x", str(package), str(root)], check=True)
    else:
        # Use the native RPM payload tools, preserving their independent statuses.
        with subprocess.Popen(["rpm2cpio", str(package)], stdout=subprocess.PIPE) as producer:
            try:
                subprocess.run(["cpio", "-idm", "--no-absolute-filenames"], stdin=producer.stdout,
                               cwd=root, check=True)
            finally:
                producer.stdout.close()
            if producer.wait() != 0:
                raise ValueError("rpm2cpio failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path, help="Configured, fully built Release tree")
    parser.add_argument("--format", choices=("DEB", "RPM"), required=True)
    parser.add_argument("--output", required=True, type=Path, help="New, absent package/output directory")
    parser.add_argument("--require-kde", action="store_true")
    parser.add_argument("--require-proton", action="store_true")
    parser.add_argument("--proton-manifest", type=Path, help="Expected provisioned Proton runtime manifest")
    args = parser.parse_args()
    build = args.build.resolve()
    output = args.output.resolve()
    output.mkdir(mode=0o755, parents=True, exist_ok=False)
    config = build / "CPackConfig.cmake"
    expected = json.loads((build / "build-identity/build-info.json").read_text())
    # CPack inherits umask when creating data directories; published packages need
    # accessible 0755 directories even when the agent's own workspace is private.
    previous_umask = os.umask(0o022)
    try:
        with (output / "cpack.log").open("w") as log:
            subprocess.run(["cpack", "--config", str(config), "-G", args.format, "-B", str(output)],
                           check=True, stdout=log, stderr=subprocess.STDOUT)
        extension = ".deb" if args.format == "DEB" else ".rpm"
        packages = list(output.glob("*" + extension))
        if len(packages) != 1:
            raise ValueError(f"Expected one native package, found {len(packages)}")
        package = packages[0]
        metadata = package_metadata(package, args.format)
        require_runtime_packages(metadata)
        if metadata["name"] != "putgd" or metadata["version"] != expected["packageVersion"]:
            raise ValueError("Native package name/version differs from compiled build manifest")
        extracted = output / "extracted"
        extract_package(package, args.format, extracted)
        proton_expected = json.loads(args.proton_manifest.read_text()) if args.proton_manifest else None
        result = audit_tree(extracted, expected, require_kde=args.require_kde,
                            require_proton=args.require_proton, expected_proton_manifest=proton_expected)
        package_digest = sha256(package)
        result["package"] = {**metadata, "file": package.name, "sha256": package_digest, "size": package.stat().st_size}
        (output / "native-audit.json").write_text(json.dumps(result, indent=2) + "\n")
        shutil.copy2(build / "build-identity/build-info.json", output / "build-info.json")
        (output / "SHA256SUMS").write_text(package_digest + "  " + package.name + "\n")
        print(json.dumps({"package": str(package), "audit": str(output / "native-audit.json")}))
    finally:
        os.umask(previous_umask)


if __name__ == "__main__":
    main()
