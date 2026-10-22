#!/usr/bin/env python3
"""The AppImage's own notices, licence texts and bundled-component inventory.

The DEB and RPM use the distribution's Qt. The AppImage bundles Qt, ICU,
QtKeychain, LayerShellQt, FFmpeg, the AWS SDK, the Proton helper and Ubuntu
libraries, so it carries its own notice text, an inventory of every bundled
component generated from the AppDir, and each component's licence texts
(#417). `finish` writes them after deployment and removes linuxdeploy's
leftovers and second copies (#421); `audit` checks a finished AppDir on its
own, so the AppImage job fails when the notices or that content regress.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
COMPONENTS = ROOT / "packaging/appimage/components.json"
DOC = "usr/share/doc/putgd"
NOTICE = DOC + "/runtime-notices.txt"
INVENTORY = DOC + "/bundled-components.json"
GPL_NOTICE = DOC + "/APPIMAGE-GPL-NOTICE.txt"
LICENSES = DOC + "/bundled-licenses"
DISTRO_LICENSES = LICENSES + "/distro"
COMMON_LICENSES = LICENSES + "/distro-common"
METAINFO = "usr/share/metainfo/gd.put.Putgd.metainfo.xml"
PRIVATE_LIBRARIES = "usr/lib/putgd"
NOTICE_TITLE = "put.gd AppImage {version}: bundled components and their licences"
# packaging/linux/runtime-notices.txt describes the DEB and RPM; it is false for the image.
NATIVE_NOTICE = "No compiler or Qt runtime is bundled"
# linuxdeploy keeps the launcher it wrapped (our AppRun, installed again
# afterwards) and the hook of its Qt plugin, which our AppRun never sources.
LINUXDEPLOY_LEFTOVERS = ("AppRun.wrapped", "apprun-hooks")
# The image is used under Qt's open-source terms.
QT_COMMERCIAL = "LicenseRef-Qt-Commercial"
SCHEMA = 1


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def relative(appdir: Path, path: Path) -> str:
    return path.relative_to(appdir).as_posix()


def entries(appdir: Path) -> list[Path]:
    """Every file and symbolic link in the AppDir, in a stable order."""
    return sorted(path for path in appdir.rglob("*") if path.is_symlink() or path.is_file())



def elf_dynamic(path: Path) -> dict | None:
    """DT_NEEDED, DT_SONAME and the library search paths of a 64-bit
    little-endian ELF file; None for anything else."""
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) < 64 or header[:4] != b"\x7fELF" or header[4] != 2 or header[5] != 1:
            return None
        program_offset, = struct.unpack_from("<Q", header, 0x20)
        entry_size, count = struct.unpack_from("<HH", header, 0x36)
        stream.seek(program_offset)
        table = stream.read(entry_size * count)
        loads, dynamic = [], None
        for index in range(count):
            kind, _, offset, address, _, size = struct.unpack_from("<IIQQQQ", table, index * entry_size)
            if kind == 1:
                loads.append((address, offset, size))
            elif kind == 2:
                dynamic = (offset, size)
        result = {"needed": [], "soname": None, "rpath": [], "runpath": []}
        if dynamic is None:
            return result
        stream.seek(dynamic[0])
        data = stream.read(dynamic[1])
        tags = []
        for offset in range(0, len(data) - 15, 16):
            tag, value = struct.unpack_from("<qQ", data, offset)
            if tag == 0:
                break
            tags.append((tag, value))
        table_address = next((value for tag, value in tags if tag == 5), None)
        if table_address is None:
            return result
        base = next((offset + table_address - address for address, offset, size in loads
                     if address <= table_address < address + size), None)
        if base is None:
            raise ValueError("ELF string table outside its loaded segments: " + str(path))

        def string(offset: int) -> str:
            stream.seek(base + offset)
            value = b""
            while b"\0" not in value:
                chunk = stream.read(256)
                if not chunk:
                    break
                value += chunk
            return value.split(b"\0", 1)[0].decode("utf-8", "replace")

        for tag, value in tags:
            if tag == 1:
                result["needed"].append(string(value))
            elif tag == 14:
                result["soname"] = string(value)
            elif tag == 15:
                result["rpath"] += string(value).split(":")
            elif tag == 29:
                result["runpath"] += string(value).split(":")
        return result


def elf_files(appdir: Path) -> dict[str, dict]:
    """The dynamic section of every ELF file (not link) in the AppDir."""
    result = {}
    for path in entries(appdir):
        if path.is_symlink():
            continue
        dynamic = elf_dynamic(path)
        if dynamic is not None:
            result[relative(appdir, path)] = dynamic
    return result


def set_origin_runpath(appdir: Path, path: Path) -> str:
    """Point a library copied in from a build prefix at the image's usr/lib,
    as linuxdeploy does for the libraries it deploys."""
    value = "$ORIGIN/" + os.path.relpath(appdir / "usr/lib", path.parent)
    subprocess.run(["patchelf", "--set-rpath", value, str(path)], check=True)
    return value



def remove_linuxdeploy_leftovers(appdir: Path) -> list[str]:
    removed = []
    for name in LINUXDEPLOY_LEFTOVERS:
        path = appdir / name
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path)
        elif path.is_symlink() or path.exists():
            path.unlink()
        else:
            continue
        removed.append(name)
    return removed


def place_dir_icon(appdir: Path) -> str:
    """Link .DirIcon to the root desktop entry's icon, as appimagetool does
    when it packs, so the inventory and its audit see the image's own files."""
    entry = (appdir / "putgd.desktop").read_text()
    icon = re.search(r"^Icon=(.+)$", entry, re.M)
    if not icon:
        raise ValueError("The root desktop entry names no icon")
    found = [path for path in sorted(appdir.glob(icon.group(1).strip() + ".*")) if path.suffix in (".svg", ".png")]
    if not found:
        raise ValueError("The root desktop entry's icon is not at the AppDir root: " + icon.group(1))
    link = appdir / ".DirIcon"
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(found[0].name)
    return found[0].name


def remove_library_copies(appdir: Path) -> list[dict]:
    """Remove the second copies linuxdeploy's `--library` leaves in usr/lib.

    It copies each library it is given into usr/lib. The S3 module's AWS SDK
    libraries are installed, and verified by aws-runtime-manifest.json, in
    usr/lib/putgd, and Qt loads plugins only from usr/plugins. A usr/lib copy
    is removed only when it is byte-identical to the private library or is a
    plugin that no ELF file in the image names as a dependency."""
    library = appdir / "usr/lib"
    private = appdir / PRIVATE_LIBRARIES
    removed = []
    for path in sorted(private.iterdir()) if private.is_dir() else []:
        copy = library / path.name
        if path.is_symlink() or not path.is_file() or not (copy.is_file() and not copy.is_symlink()):
            continue
        if sha256(copy) != sha256(path):
            raise ValueError("usr/lib copy differs from the private library: " + path.name)
        copy.unlink()
        removed.append({"path": relative(appdir, copy), "keptCopy": relative(appdir, path)})
    # A versioned private library (libaws-c-common.so.0.14.5) is also copied
    # under its SONAME (libaws-c-common.so.1).
    sonames = {}
    for path in sorted(private.iterdir()) if private.is_dir() else []:
        if path.is_file() and not path.is_symlink():
            dynamic = elf_dynamic(path)
            if dynamic and dynamic["soname"]:
                sonames[dynamic["soname"]] = path
    for name, path in sonames.items():
        copy = library / name
        if copy.is_file() and not copy.is_symlink():
            if sha256(copy) != sha256(path):
                raise ValueError("usr/lib copy differs from the private library: " + name)
            copy.unlink()
            removed.append({"path": relative(appdir, copy), "keptCopy": relative(appdir, path)})
    needed = {name for dynamic in elf_files(appdir).values() for name in dynamic["needed"]}
    for plugin in sorted((appdir / "usr/plugins").rglob("*.so")):
        copy = library / plugin.name
        if not copy.is_file() or copy.is_symlink():
            continue
        if plugin.name in needed:
            raise ValueError("A library in the image depends on the plugin copy " + relative(appdir, copy))
        copy.unlink()
        removed.append({"path": relative(appdir, copy), "keptCopy": relative(appdir, plugin)})
    return removed


def remove_distro_doc_copies(appdir: Path) -> list[str]:
    """linuxdeploy also copies each deployed library's distro copyright file to
    usr/share/doc/PACKAGE; bundled-licenses/distro holds the same files."""
    removed = []
    for directory in sorted((appdir / "usr/share/doc").iterdir()):
        if directory.name == "putgd" or directory.is_symlink() or not directory.is_dir():
            continue
        ours = appdir / DISTRO_LICENSES / directory.name / "copyright"
        files = sorted(path.name for path in directory.iterdir())
        if files != ["copyright"] or not ours.is_file() or sha256(ours) != sha256(directory / "copyright"):
            raise ValueError("Unexpected distro documentation in the image: usr/share/doc/" + directory.name)
        shutil.rmtree(directory)
        removed.append("usr/share/doc/" + directory.name)
    return removed


def link_proton_doc_copies(appdir: Path) -> list[str]:
    """The Proton helper's notices are installed beside it and in the doc
    directory. Keep the helper's copies, which its manifest binds, and make the
    identical doc entries relative links to them."""
    helper = appdir / "usr/libexec/putgd/proton"
    docs = appdir / DOC / "proton"
    if not docs.is_dir():
        return []
    linked = []
    for path in sorted(docs.iterdir()):
        if path.is_symlink() or not path.is_file():
            continue
        for kept in (helper / "redistribution" / path.name, helper / path.name):
            if kept.is_file() and not kept.is_symlink() and sha256(kept) == sha256(path):
                path.unlink()
                path.symlink_to(os.path.relpath(kept, path.parent))
                linked.append(relative(appdir, path))
                break
    return linked


def audit_runpaths(appdir: Path, elves: dict[str, dict] | None = None) -> int:
    """Every RPATH/RUNPATH entry must be $ORIGIN-relative."""
    elves = elf_files(appdir) if elves is None else elves
    absolute = sorted(path + ": " + entry for path, dynamic in elves.items()
                      for entry in dynamic["rpath"] + dynamic["runpath"]
                      if entry and not entry.startswith(("$ORIGIN", "${ORIGIN}")))
    if absolute:
        raise ValueError("Absolute library search path in the image: " + "; ".join(absolute))
    return len(elves)


def audit_hygiene(appdir: Path, elves: dict[str, dict]) -> dict:
    left = [name for name in LINUXDEPLOY_LEFTOVERS if (appdir / name).is_symlink() or (appdir / name).exists()]
    if left:
        raise ValueError("linuxdeploy leftovers in the image: " + ", ".join(left))
    library = appdir / "usr/lib"
    copies = sorted(relative(appdir, library / path.name) for path in (appdir / "usr/plugins").rglob("*.so")
                    if (library / path.name).exists())
    private = appdir / PRIVATE_LIBRARIES
    copies += sorted(relative(appdir, library / path.name) for path in (private.iterdir() if private.is_dir() else [])
                     if (library / path.name).exists())
    copies += sorted(relative(appdir, library / soname) for path, dynamic in elves.items()
                     if path.startswith(PRIVATE_LIBRARIES + "/") and dynamic["soname"]
                     and (library / dynamic["soname"]).exists())
    copies += sorted("usr/share/doc/" + path.name for path in (appdir / "usr/share/doc").iterdir() if path.name != "putgd")
    if copies:
        raise ValueError("Second copies in the image: " + ", ".join(sorted(set(copies))))
    dangling = [relative(appdir, path) for path in entries(appdir)
                if path.is_symlink() and (not path.exists() or not path.resolve().is_relative_to(appdir.resolve()))]
    if dangling:
        raise ValueError("Links that leave the image or point nowhere: " + ", ".join(dangling))
    # Identical regular files outside the licence texts (where packages share
    # their copyright text) are second copies, apart from the Proton runtime
    # manifest, which the helper and the app each read from their own place.
    seen, duplicates = {}, []
    for path in entries(appdir):
        name = relative(appdir, path)
        if path.is_symlink() or name.startswith(DOC + "/") or name in (
                "usr/share/putgd/proton-runtime-manifest.json",):
            continue
        digest = sha256(path)
        if digest in seen:
            duplicates.append(seen[digest] + " = " + name)
        seen.setdefault(digest, name)
    if duplicates:
        raise ValueError("Identical files in the image: " + "; ".join(duplicates))
    return {"linuxdeployLeftovers": [], "secondCopies": [], "identicalFiles": [],
            "originRelativeSearchPaths": audit_runpaths(appdir, elves)}



def spdx_ids(expression: str) -> list[str]:
    ids = []
    for token in re.findall(r"[A-Za-z0-9.+:-]+", expression):
        if token not in ("AND", "OR", "WITH") and token not in ids:
            ids.append(token)
    return ids


def open_source_terms(expression: str) -> str:
    """Qt's SBOM offers the commercial licence as an alternative; drop it."""
    terms = [term.strip() for term in expression.split(" OR ")]
    if QT_COMMERCIAL not in terms:
        return expression
    return " OR ".join(term for term in terms if term != QT_COMMERCIAL)


def conjunct(expression: str) -> str:
    return "(" + expression + ")" if " OR " in expression and not expression.startswith("(") else expression


def combined(expressions: list[str]) -> str:
    """AND of distinct licence expressions, keeping each alternative together."""
    parts = []
    for expression in expressions:
        if expression == "NOASSERTION":
            continue
        for part in split_and(expression):
            part = conjunct(part)
            if part not in parts:
                parts.append(part)
    if len(parts) == 1:
        return split_and(parts[0])[0]
    return " AND ".join(parts)


def split_and(expression: str) -> list[str]:
    """Top-level AND operands of an SPDX expression."""
    parts, depth, start = [], 0, 0
    for match in re.finditer(r"\(|\)| AND ", expression):
        if match.group() == "(":
            depth += 1
        elif match.group() == ")":
            depth -= 1
        elif depth == 0:
            parts.append(expression[start:match.start()].strip())
            start = match.end()
    parts.append(expression[start:].strip())
    return [part[1:-1] if part.startswith("(") and part.endswith(")") and part.count("(") == 1 else part
            for part in parts]


# Debian machine-readable copyright short names that are SPDX identifiers under another name.
DEP5_SPDX = {
    "gpl-2": "GPL-2.0-only", "gpl-2.0": "GPL-2.0-only", "gpl-2+": "GPL-2.0-or-later", "gpl-2.0+": "GPL-2.0-or-later",
    "gpl-3": "GPL-3.0-only", "gpl-3.0": "GPL-3.0-only", "gpl-3+": "GPL-3.0-or-later", "gpl-3.0+": "GPL-3.0-or-later",
    "lgpl-2": "LGPL-2.0-only", "lgpl-2.0": "LGPL-2.0-only", "lgpl-2+": "LGPL-2.0-or-later", "lgpl-2.0+": "LGPL-2.0-or-later",
    "lgpl-2.1": "LGPL-2.1-only", "lgpl-2.1+": "LGPL-2.1-or-later", "lgpl-3": "LGPL-3.0-only", "lgpl-3.0": "LGPL-3.0-only",
    "lgpl-3+": "LGPL-3.0-or-later", "lgpl-3.0+": "LGPL-3.0-or-later", "expat": "MIT", "mit": "MIT",
    "bsd-2-clause": "BSD-2-Clause", "bsd-3-clause": "BSD-3-Clause", "apache-2.0": "Apache-2.0", "isc": "ISC",
    "zlib": "Zlib", "curl": "curl", "libpng": "Libpng", "openldap-2.8": "OLDAP-2.8", "mpl-1.1": "MPL-1.1",
    "mpl-2.0": "MPL-2.0", "cc0-1.0": "CC0-1.0", "afl-2.1": "AFL-2.1", "x11": "X11", "ijg": "IJG",
    "bsl-1.0": "BSL-1.0", "ftl": "FTL", "hpnd": "HPND", "unicode-dfs-2016": "Unicode-DFS-2016",
}


def dep5_license(text: str) -> tuple[str | None, str]:
    """The licence of a package's `Files: *` paragraph in a machine-readable
    debian/copyright, raw and as SPDX (NOASSERTION where it does not map)."""
    lines = text.splitlines()
    if not lines or not lines[0].lower().startswith("format:"):
        return None, "NOASSERTION"
    declared = None
    for paragraph in re.split(r"\n\s*\n", text):
        fields, current = {}, None
        for line in paragraph.splitlines():
            if line[:1] in (" ", "\t"):
                continue
            if ":" in line:
                current, value = line.split(":", 1)
                fields[current.strip().lower()] = value.strip()
        if fields.get("files", "").split()[:1] == ["*"] and fields.get("license"):
            declared = fields["license"]
            break
    if declared is None:
        return None, "NOASSERTION"
    words = declared.replace(",", " , ").split()
    if "," in words and len({word.lower() for word in words if word.lower() in ("and", "or")}) > 1:
        return declared, "NOASSERTION"
    expression = []
    for word in words:
        if word == ",":
            continue
        if word.lower() in ("and", "or"):
            expression.append(word.upper())
        elif word.lower() in DEP5_SPDX:
            expression.append(DEP5_SPDX[word.lower()])
        else:
            return declared, "NOASSERTION"
    if not expression or expression[0] in ("AND", "OR") or expression[-1] in ("AND", "OR"):
        return declared, "NOASSERTION"
    return declared, " ".join(expression)



class QtSbom:
    """The SPDX documents of the official Qt binaries (QT_ROOT/sbom): which
    module or plugin each file belongs to, its licence, and the third-party
    code compiled into it."""

    def __init__(self, qt: Path):
        self.qt = qt.resolve()
        self.files, self.packages, self.depends, self.versions = {}, {}, {}, {}
        for path in sorted((self.qt / "sbom").glob("*.spdx.json")):
            match = re.fullmatch(r"(.+)-([0-9][0-9.]*)\.spdx\.json", path.name)
            if not match:
                continue
            repository = match.group(1)
            document = json.loads(path.read_text())
            self.versions[repository] = match.group(2)
            files = {item["SPDXID"]: item["fileName"] for item in document.get("files", [])}
            for package in document.get("packages", []):
                self.packages[package["SPDXID"]] = dict(package, repository=repository, document=path.name)
            for relation in document.get("relationships", []):
                source, kind, target = relation["spdxElementId"], relation["relationshipType"], relation["relatedSpdxElement"]
                if kind == "CONTAINS" and target in files and source in self.packages:
                    self.files[files[target]] = source
                elif kind == "DEPENDS_ON":
                    self.depends.setdefault(source, []).append(target)
        if not self.files:
            raise ValueError("The Qt install has no SPDX documents under sbom/: " + str(self.qt))

    def install_path(self, name: str) -> Path | None:
        """The Qt install file an AppDir path was deployed from, if any."""
        parts = PurePosixPath(name).parts
        if parts[:2] == ("usr", "lib") and len(parts) == 3:
            candidate = self.qt / "lib" / parts[2]
        elif parts[:2] in (("usr", "plugins"), ("usr", "translations")) and len(parts) > 2:
            candidate = self.qt / parts[1] / Path(*parts[2:])
        else:
            return None
        return candidate if candidate.is_file() else None

    def package(self, name: str) -> str | None:
        candidate = self.install_path(name)
        if candidate is None:
            return None
        return self.files.get("./" + candidate.resolve().relative_to(self.qt).as_posix())

    def embedded(self, package: str) -> list[dict]:
        """Third-party code compiled into a module or plugin: its bundled
        third-party libraries and attributed sources, not system libraries."""
        result, queue, seen = [], list(self.depends.get(package, [])), set()
        while queue:
            item = queue.pop(0)
            if item in seen or item not in self.packages:
                continue
            seen.add(item)
            if "-qt-bundled-3rdparty-module-" in item or "-qt-3rdparty-sources-" in item:
                result.append(self.packages[item])
                queue += self.depends.get(item, [])
        return result


def attribution(package: dict, source: Path) -> tuple[Path, dict] | None:
    """The qt_attribution.json entry Qt's SBOM extracted a package from."""
    comment = package.get("comment", "")
    found = re.search(r"Information extracted from:\s*/src_dir/[^/]+/(\S+qt_attribution\.json)", comment)
    if not found:
        return None
    path = source / found.group(1)
    if not path.is_file():
        raise ValueError("Licence source lacks " + found.group(1) + " for " + package["name"])
    data = json.loads(path.read_text(), strict=False)  # some hold literal tabs in strings
    index = re.search(r"Entry index:\s*(\d+)", comment)
    entry = (data if isinstance(data, list) else [data])[int(index.group(1)) if index else 0]
    return path, entry



def matching(patterns: list[str], names: list[str]) -> list[str]:
    return [name for name in names if any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)]


def lock_reference(lock: dict, key: str) -> dict:
    item = lock["buildInputs"][key]
    return {"version": item.get("release") or item["commit"][:12], "repository": item["repository"],
            "commit": item["commit"]}


def component_identity(appdir: Path, spec: dict, lock: dict, build: dict) -> dict:
    """A component's version and source location, read from the build."""
    kind = spec["version"]["from"]
    key = spec["version"].get("key")
    if kind == "build":
        return {"version": build["version"], "source": {"repository": spec["source"]["repository"],
                                                       "commit": build["gitCommit"]}}
    if kind == "lock":
        reference = lock_reference(lock, key)
        return {"version": reference["version"], "source": {"repository": reference["repository"],
                                                            "commit": reference["commit"]}}
    if kind == "dependencyLock":
        item = json.loads((appdir / "usr/share/putgd/dependency-lock.json").read_text())[key]
        return {"version": item["release"], "source": {"repository": item["repository"], "commit": item["commit"]}}
    if kind == "protonManifest":
        manifest = json.loads((appdir / "usr/share/putgd/proton-runtime-manifest.json").read_text())
        return {"version": manifest["appVersion"] + " (SDK " + manifest["sdkVersion"] + ", Bun " + manifest["bunVersion"] + ")",
                "source": {"sourceCatalog": spec["source"]["catalog"], "commit": manifest["sourceReference"]}}
    if kind == "tool":
        item = next(tool for tool in lock["tools"] if tool["name"] == key)
        return {"version": item["release"], "source": {"repository": "https://github.com/" + item["repository"],
                                                       "commit": item["commit"]}}
    raise ValueError("Unknown component version source: " + kind)


def copy_license_file(source: Path, path: str, destination: Path, appdir: Path) -> str:
    original = (source / path).resolve()
    if not original.is_relative_to(source.resolve()) or not original.is_file():
        raise ValueError("Licence file missing from " + source.name + ": " + path)
    target = destination / original.relative_to(source.resolve())
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(original, target)
    return relative(appdir, target)


def upstream_component(appdir: Path, spec: dict, lock: dict, build: dict, source: Path | None,
                       names: list[str], qt: Path) -> dict:
    """`names` are the AppDir's paths; licence and notice files are under usr/share/doc/putgd."""
    """A component that components.json describes: its files, licence texts and identity."""
    identity = component_identity(appdir, spec, lock, build)
    if spec.get("versionFile") and not Path(spec["versionFile"].format(qt=qt, version=identity["version"])).is_file():
        raise ValueError(spec["name"] + " " + identity["version"] + " is not the version in the Qt install; update the lock")
    files = matching(spec["files"], [name for name in names if not name.startswith(DOC + "/")])
    if spec["files"] and not files:
        raise ValueError("No files of " + spec["name"] + " in the image")
    destination = appdir / LICENSES / spec["id"]
    license_files = matching(spec.get("noticeFiles", []), names)
    if spec.get("licenseFiles"):
        if source is None:
            raise ValueError("No licence source for " + spec["name"] + ": pass --license-source .../" + spec["id"])
        for pattern in spec["licenseFiles"]:
            matches = sorted(path for path in source.glob(pattern) if path.is_file())
            if not matches:
                raise ValueError("Licence source " + spec["id"] + " has no " + pattern)
            license_files += [copy_license_file(source, path.relative_to(source).as_posix(), destination, appdir)
                              for path in matches if QT_COMMERCIAL not in path.name]
    texts = {}
    for identifier in spdx_ids(spec["license"]):
        path = spec.get("licenseTexts", {}).get(identifier, "LICENSES/" + identifier + ".txt")
        if not path.startswith("usr/"):
            path = relative(appdir, destination / path)
        if not (appdir / path).is_file():
            raise ValueError("No licence text for " + identifier + " of " + spec["name"])
        texts[identifier] = path
        if path not in license_files:
            license_files.append(path)
    embedded = []
    for item in spec.get("embedded", []):
        record = {"name": item["name"], "license": item["license"]}
        if item.get("licenseText"):
            record["licenseText"] = item["licenseText"]  # audit() checks it is in the image
        embedded.append(record)
    component = {"id": spec["id"], "name": spec["name"], "version": identity["version"], "license": spec["license"],
                 "source": identity["source"], "files": files, "licenseFiles": sorted(set(license_files)),
                 "licenseTexts": texts}
    if embedded:
        component["embedded"] = embedded
    if spec.get("note"):
        component["note"] = spec["note"]
    return component


def qt_components(appdir: Path, sbom: QtSbom, lock: dict, sources: dict[str, Path], names: list[str],
                  claimed: set[str]) -> list[dict]:
    """One component per Qt repository with files in the image, from Qt's SBOM."""
    owners: dict[str, dict[str, list[str]]] = {}
    for name in names:
        if name in claimed or (appdir / name).is_symlink():
            continue
        candidate = sbom.install_path(name)
        if candidate is None:
            continue
        package = sbom.package(name)
        if package is None:
            raise ValueError("Qt file without an entry in Qt's SPDX documents: " + name)
        owners.setdefault(sbom.packages[package]["repository"], {}).setdefault(package, []).append(name)
    pins = {item["repository"].rsplit("/", 1)[1].removesuffix(".git"): item for item in lock["buildInputs"]["qtLicenseSources"]}
    components = []
    for repository in sorted(owners):
        pin = pins.get(repository)
        if pin is None:
            raise ValueError("Qt files from " + repository + " but it is not a qtLicenseSources entry in the lock")
        if repository not in sources:
            raise ValueError("No licence source for Qt " + repository + ": pass --license-source .../" + repository)
        # The SBOM names the module's commit (c07c2d5a+HEAD) or its release.
        built = sbom.packages.get("SPDXRef-Package-" + repository, {}).get("versionInfo", "")
        if sbom.versions[repository] != pin["release"] or (
                re.fullmatch(r"[0-9a-f]{7,40}(\+.*)?", built) and not pin["commit"].startswith(built.split("+", 1)[0])):
            raise ValueError("Qt " + repository + " in the image is " + sbom.versions[repository] + " " + built +
                             "; the lock pins " + pin["release"] + " " + pin["commit"])
        source = sources[repository]
        destination = appdir / LICENSES / repository
        license_files, texts, licenses, embedded = [], {}, [], []
        for package, files in sorted(owners[repository].items()):
            licenses.append(open_source_terms(sbom.packages[package].get("licenseConcluded", "NOASSERTION")))
            for item in sbom.embedded(package):
                record = next((record for record in embedded if record["spdxId"] == item["SPDXID"]), None)
                if record is None:
                    record = {"spdxId": item["SPDXID"], "name": item["name"].split("_Attribution_")[-1],
                              "version": item.get("versionInfo", "NOASSERTION"),
                              "license": open_source_terms(item.get("licenseConcluded", "NOASSERTION")),
                              "download": item.get("downloadLocation", "NOASSERTION"),
                              "copyright": item.get("copyrightText", "NOASSERTION"), "usedBy": []}
                    found = attribution(item, source)
                    record["licenseFiles"], record["text"] = [], False
                    if found:
                        path, entry = found
                        record["name"] = entry.get("Name", record["name"])
                        record["licenseFiles"].append(copy_license_file(source, path.relative_to(source).as_posix(),
                                                                        destination, appdir))
                        named = entry.get("LicenseFiles") or ([entry["LicenseFile"]] if entry.get("LicenseFile") else [])
                        for file in named:
                            record["licenseFiles"].append(copy_license_file(
                                source, (path.parent / file).resolve().relative_to(source.resolve()).as_posix(),
                                destination, appdir))
                            record["text"] = True
                    embedded.append(record)
                record["usedBy"] += [PurePosixPath(name).name for name in files]
        expression = combined(licenses)
        for identifier in spdx_ids(expression):
            path = source / "LICENSES" / (identifier + ".txt")
            if not path.is_file():
                raise ValueError("Qt " + repository + " has no LICENSES/" + identifier + ".txt")
            texts[identifier] = copy_license_file(source, "LICENSES/" + path.name, destination, appdir)
        for record in embedded:
            for identifier in spdx_ids(record["license"]):
                path = source / "LICENSES" / (identifier + ".txt")
                if path.is_file():
                    texts[identifier] = copy_license_file(source, "LICENSES/" + path.name, destination, appdir)
                elif not record["text"] and identifier != "NOASSERTION":
                    raise ValueError("No licence text for " + identifier + " of " + record["name"] + " in Qt " + repository)
        for record in embedded:
            license_files += record["licenseFiles"]
            record["usedBy"] = sorted(set(record["usedBy"]))
            del record["spdxId"], record["text"]
        files = sorted(name for package in owners[repository].values() for name in package)
        components.append({"id": repository, "name": "Qt " + repository, "version": sbom.versions[repository],
                           "license": expression, "source": {"repository": pin["repository"], "commit": pin["commit"]},
                           "files": files, "licenseFiles": sorted(set(license_files) | set(texts.values())),
                           "licenseTexts": texts, "embedded": sorted(embedded, key=lambda item: item["name"].lower()),
                           "sbom": repository + "-" + sbom.versions[repository] + ".spdx.json"})
    unused = sorted(set(sources) & set(pins) - set(owners))
    if unused:
        raise ValueError("Qt licence sources with no files in the image: " + ", ".join(unused))
    return components


def dpkg_fields(package: str) -> list[str]:
    """An installed package's version, source package and source version."""
    return subprocess.run(["dpkg-query", "-W", "-f", "${Version}\t${source:Package}\t${source:Version}", package],
                          check=True, text=True, capture_output=True).stdout.split("\t")


def distro_components(appdir: Path, packages: dict[str, list[str]], distribution: str) -> list[dict]:
    components = []
    for package in sorted(packages):
        fields = dpkg_fields(package)
        copyright_file = DISTRO_LICENSES + "/" + package + "/copyright"
        declared, spdx = dep5_license((appdir / copyright_file).read_text(errors="replace"))
        component = {"id": "ubuntu:" + package, "name": package, "version": fields[0], "license": spdx,
                     "source": {"distribution": distribution, "sourcePackage": fields[1], "sourceVersion": fields[2],
                                "url": "https://launchpad.net/ubuntu/+source/" + fields[1] + "/" + fields[2]},
                     "files": sorted(packages[package]), "licenseFiles": [copyright_file]}
        if declared is not None:
            component["declaredLicense"] = declared
        components.append(component)
    return components


def classify_links(appdir: Path, components: list[dict], names: list[str]) -> None:
    """A link belongs to the component of the file it points to."""
    owner = {name: component for component in components for name in component["files"]}
    licence_owner = {name: component for component in components for name in component["licenseFiles"]}
    for name in names:
        path = appdir / name
        if not path.is_symlink() or name in owner or name in licence_owner:
            continue
        target = relative(appdir, path.resolve())
        if target in owner:
            owner[target]["files"].append(name)
            owner[name] = owner[target]
        elif target in licence_owner:
            licence_owner[target]["licenseFiles"].append(name)
            licence_owner[name] = licence_owner[target]
    for component in components:
        component["files"] = sorted(set(component["files"]))
        component["licenseFiles"] = sorted(set(component["licenseFiles"]))



def finish(appdir: Path, *, qt: Path, lock: dict, license_sources: list[Path], distro_packages,
           version: str, build: dict, gpl: list[dict]) -> dict:
    """Write the image's notices after deployment; return a receipt summary.

    `distro_packages(appdir, claimed)` copies the distro copyright notices and
    returns {binary package: [AppDir paths]} for the ELF files not in `claimed`."""
    appdir = appdir.resolve()
    sources = {}
    for path in license_sources:
        if path.name in sources:
            raise ValueError("Licence source given twice: " + path.name)
        sources[path.name] = path.resolve()
    spec = json.loads(COMPONENTS.read_text())
    place_dir_icon(appdir)
    removed = {"linuxdeployLeftovers": remove_linuxdeploy_leftovers(appdir),
               "libraryCopies": remove_library_copies(appdir)}
    names = [relative(appdir, path) for path in entries(appdir)]
    upstream = [upstream_component(appdir, item, lock, build, sources.get(item["id"]), names, qt)
                for item in spec["components"]]
    claimed = [name for component in upstream for name in component["files"]]
    twice = sorted({name for name in claimed if claimed.count(name) > 1})
    if twice:
        raise ValueError("Files claimed by two components: " + ", ".join(twice))
    qt_parts = qt_components(appdir, QtSbom(qt), lock, sources, [name for name in names if not name.startswith(DOC + "/")],
                             set(claimed))
    claimed = set(claimed) | {name for component in qt_parts for name in component["files"]}
    packages = distro_packages(appdir, claimed)
    removed["distroDocCopies"] = remove_distro_doc_copies(appdir)
    removed["protonDocLinks"] = link_proton_doc_copies(appdir)
    distro = distro_components(appdir, packages, lock["buildInputs"]["distribution"])
    components = upstream + qt_parts + distro
    unused = sorted(set(sources) - {component["id"] for component in upstream + qt_parts})
    if unused:
        raise ValueError("Licence sources that no bundled component uses: " + ", ".join(unused))
    classify_links(appdir, components, [relative(appdir, path) for path in entries(appdir)])
    common = sorted(relative(appdir, path) for path in (appdir / COMMON_LICENSES).rglob("*")
                    if path.is_file() or path.is_symlink())
    expression = combined([component["license"] for component in components])
    write_metainfo_license(appdir, expression)
    inventory = {"schemaVersion": SCHEMA, "format": "AppImage", "version": version,
                 "build": {"commit": build["gitCommit"], "tree": build.get("gitTree")},
                 "appstreamLicense": expression, "notices": [NOTICE, INVENTORY, GPL_NOTICE],
                 "sharedLicenseTexts": common, "components": components}
    (appdir / INVENTORY).write_text(json.dumps(inventory, indent=2) + "\n")
    (appdir / NOTICE).write_text(notice_text(inventory, gpl))
    return {"inventory": inventory, "removed": removed}


def write_metainfo_license(appdir: Path, expression: str) -> None:
    path = appdir / METAINFO
    text = path.read_text()
    updated, count = re.subn(r"<project_license>[^<]*</project_license>",
                             "<project_license>" + expression + "</project_license>", text)
    if count != 1:
        raise ValueError("AppStream metadata must hold one project_license")
    path.write_text(updated)


def source_text(source: dict) -> str:
    if "sourceCatalog" in source:
        return "the assets listed in " + source["sourceCatalog"] + " (helper source " + source["commit"] + ")"
    return source["repository"] + " at commit " + source["commit"]


def notice_text(inventory: dict, gpl: list[dict]) -> str:
    components = inventory["components"]
    upstream = [component for component in components if not component["id"].startswith("ubuntu:")]
    distro = [component for component in components if component["id"].startswith("ubuntu:")]
    qt = next((component["version"] for component in components if component["id"] == "qtbase"), "")
    distribution = distro[0]["source"]["distribution"] if distro else ""
    lines = [NOTICE_TITLE.format(version=inventory["version"]), "",
             "Unlike the DEB and RPM packages, which use the distribution's Qt, this AppImage carries",
             "its own runtime: Qt " + qt + " with its ICU, QtKeychain, LayerShellQt, the AWS SDK for C++,",
             "FFmpeg, the Proton Drive helper and " + str(len(distro)) + " " + distribution + " packages (GStreamer, GTK, GLib,",
             "codec and X11/Wayland client libraries). Every component keeps its own licence.",
             "",
             "This file and bundled-components.json beside it list every component with its version,",
             "SPDX licence expression, source location, licence texts and the image files that belong",
             "to it. Both are generated from the image's contents when it is built. Licence texts:",
             "  bundled-licenses/<component>/          pinned upstream licence texts and Qt's third-party",
             "                                         attributions (qt_attribution.json)",
             "  bundled-licenses/distro/<package>/     each Ubuntu package's copyright file",
             "  bundled-licenses/distro-common/        the licence texts those files refer to",
             "  APPIMAGE-GPL-NOTICE.txt                the GPL terms of the image and where its codec",
             "                                         source is published",
             "",
             "The LGPL libraries are separate shared objects in usr/lib, found through AppRun's",
             "LD_LIBRARY_PATH. To use a modified version, extract the image (--appimage-extract) and",
             "replace them in squashfs-root/usr/lib.",
             "",
             "AppStream licence of the image: " + inventory["appstreamLicense"], ""]
    if gpl:
        lines += ["GPL codec libraries: " + ", ".join(sorted({item["binaryPackage"] for item in gpl})) +
                  " (see APPIMAGE-GPL-NOTICE.txt).", ""]
    lines += ["Components", "=========="]
    for component in upstream:
        lines += ["", component["name"] + " " + component["version"],
                  "  Licence: " + component["license"],
                  "  Source:  " + source_text(component["source"])]
        if component.get("note"):
            lines.append("  Note:    " + component["note"])
        if component["licenseTexts"]:
            lines.append("  Licence texts:")
            lines += ["    " + identifier + ": " + path for identifier, path in sorted(component["licenseTexts"].items())]
        for item in component.get("embedded", []):
            lines.append("  Includes " + item["name"] + (" " + item["version"] if item.get("version") else "") +
                         " (" + item["license"] + ")" + (" in " + ", ".join(item["usedBy"]) if item.get("usedBy") else ""))
        lines.append("  Files:")
        lines += ["    " + name for name in component["files"]] or ["    (the start of the AppImage file)"]
    lines += ["", distribution + " packages", "=" * len(distribution + " packages"),
              "Binary package, version, source package and version, licence of its Files: * stanza",
              "(NOASSERTION where its copyright file is not machine-readable; the copyright file applies)."]
    for component in distro:
        lines += ["", component["name"] + " " + component["version"] + "  (source " +
                  component["source"]["sourcePackage"] + " " + component["source"]["sourceVersion"] + ")",
                  "  Licence: " + component["license"] + "    Copyright: " + component["licenseFiles"][0],
                  "  Source:  " + component["source"]["url"]]
        lines += ["    " + name for name in component["files"]]
    return "\n".join(lines) + "\n"



def audit(appdir: Path) -> dict:
    """Check a finished AppDir's notices and contents on their own (#417, #421)."""
    appdir = appdir.resolve()
    inventory_path = appdir / INVENTORY
    if not inventory_path.is_file():
        raise ValueError("The image has no bundled-component inventory: " + INVENTORY)
    inventory = json.loads(inventory_path.read_text())
    if inventory.get("schemaVersion") != SCHEMA or inventory.get("format") != "AppImage":
        raise ValueError("Unsupported bundled-component inventory")
    notice = (appdir / NOTICE).read_text() if (appdir / NOTICE).is_file() else ""
    if not notice.startswith(NOTICE_TITLE.format(version=inventory["version"])):
        raise ValueError("runtime-notices.txt is not the AppImage's notice text")
    names = [relative(appdir, path) for path in entries(appdir)]
    for name in names:
        if name.startswith(DOC + "/") and not (appdir / name).is_symlink() and NATIVE_NOTICE.encode() in (appdir / name).read_bytes():
            raise ValueError("The native packages' notice text is in the image: " + name)
    listed: dict[str, str] = {}

    def claim(name: str, owner: str) -> None:
        if name in listed:
            raise ValueError("Listed twice in the inventory: " + name + " (" + listed[name] + ", " + owner + ")")
        listed[name] = owner

    for name in inventory["notices"] + inventory["sharedLicenseTexts"]:
        claim(name, "notices")
    components = inventory["components"]
    if not components:
        raise ValueError("The inventory lists no components")
    for component in components:
        for key in ("id", "name", "version", "license", "source", "files", "licenseFiles"):
            if key not in component or component[key] in ("", None):
                raise ValueError("Inventory component without " + key + ": " + str(component.get("id")))
        for name in component["files"] + component["licenseFiles"]:
            claim(name, component["id"])
        if not component["licenseFiles"]:
            raise ValueError("No licence text for " + component["name"])
        empty = [name for name in component["licenseFiles"] if not (appdir / name).is_file() or not (appdir / name).stat().st_size]
        if empty:
            raise ValueError("Missing or empty licence file: " + ", ".join(empty))
        if component["license"] != "NOASSERTION":
            texts = component.get("licenseTexts")
            if texts is not None:
                missing = [identifier for identifier in spdx_ids(component["license"]) if identifier not in texts]
                if missing:
                    raise ValueError("No licence text for " + ", ".join(missing) + " of " + component["name"])
                for path in texts.values():
                    if path not in component["licenseFiles"]:
                        raise ValueError("Licence text outside its component's licence files: " + path)
        for item in component.get("embedded", []):
            for path in item.get("licenseFiles", []) + ([item["licenseText"]] if item.get("licenseText") else []):
                if path not in listed:
                    raise ValueError("Licence file of " + item["name"] + " is not listed: " + path)
        if component["id"].startswith("ubuntu:") and component["licenseFiles"] != [DISTRO_LICENSES + "/" + component["name"] + "/copyright"]:
            raise ValueError("Ubuntu package without its copyright file: " + component["name"])
        if (component["name"] + " " + component["version"]) not in notice:
            raise ValueError("runtime-notices.txt does not list " + component["name"] + " " + component["version"])
    missing = sorted(set(listed) - set(names))
    if missing:
        raise ValueError("Inventory lists files the image lacks: " + ", ".join(missing[:20]))
    unlisted = sorted(set(names) - set(listed))
    if unlisted:
        raise ValueError("Image files missing from the inventory: " + ", ".join(unlisted[:20]))
    irrelevant = [name for name in names if name.startswith(LICENSES + "/") and (
        QT_COMMERCIAL in name or set(PurePosixPath(name).parts) & {"tests", "examples", "manual"})]
    if irrelevant:
        raise ValueError("Licence texts that do not apply to the image: " + ", ".join(irrelevant))
    expression = combined([component["license"] for component in components])
    if inventory["appstreamLicense"] != expression:
        raise ValueError("Inventory AppStream licence differs from its components")
    found = re.findall(r"<project_license>([^<]*)</project_license>", (appdir / METAINFO).read_text())
    if found != [expression]:
        raise ValueError("AppStream project_license differs from the inventory: " + ", ".join(found))
    hygiene = audit_hygiene(appdir, elf_files(appdir))
    return {"components": len(components), "files": len(listed), "appstreamLicense": expression,
            "licenseTextsPresent": True, "nativeNoticeAbsent": True, **hygiene}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--appdir", type=Path, required=True, help="Extracted image (squashfs-root) or AppDir")
    parser.add_argument("--output", type=Path, help="Write the audit result here")
    args = parser.parse_args()
    result = audit(args.appdir)
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
