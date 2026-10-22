#!/usr/bin/env python3
"""Measure an extracted put.gd AppImage's notices and content (#417, #421).

Standard library only. argv: IMAGE SQUASHFS_ROOT; prints JSON.
"""
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

DOC = "usr/share/doc/putgd"
LICENSES = DOC + "/bundled-licenses"
NATIVE = b"No compiler or Qt runtime is bundled"


def search_paths(path):
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) < 64 or header[:4] != b"\x7fELF" or header[4] != 2:
            return None
        phoff, = struct.unpack_from("<Q", header, 0x20)
        size, count = struct.unpack_from("<HH", header, 0x36)
        stream.seek(phoff)
        table = stream.read(size * count)
        loads, dynamic = [], None
        for i in range(count):
            kind, _, offset, address, _, filesz = struct.unpack_from("<IIQQQQ", table, i * size)
            if kind == 1:
                loads.append((address, offset, filesz))
            elif kind == 2:
                dynamic = (offset, filesz)
        if not dynamic:
            return []
        stream.seek(dynamic[0])
        data = stream.read(dynamic[1])
        tags = []
        for o in range(0, len(data) - 15, 16):
            tag, value = struct.unpack_from("<qQ", data, o)
            if tag == 0:
                break
            tags.append((tag, value))
        strtab = next((v for t, v in tags if t == 5), None)
        base = next((o + strtab - a for a, o, s in loads if strtab is not None and a <= strtab < a + s), None)
        result = []
        for tag, value in tags:
            if tag in (15, 29) and base is not None:
                stream.seek(base + value)
                result += stream.read(4096).split(b"\0", 1)[0].decode().split(":")
        return result


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    image, root = Path(sys.argv[1]), Path(sys.argv[2])
    entries = sorted(p for p in root.rglob("*") if p.is_file() or p.is_symlink())
    files = [p for p in entries if not p.is_symlink()]
    rel = lambda p: p.relative_to(root).as_posix()  # noqa: E731
    lic = root / LICENSES
    per_component = {}
    for name in ("qtbase", "qtwayland", "qtsvg", "qtimageformats", "qttranslations", "qtdeclarative",
                 "layershell", "qtkeychain", "ffmpeg", "icu"):
        d = lic / name
        if not d.exists():
            per_component[name] = None
            continue
        texts = sorted(p.name for p in (d / "LICENSES").glob("*")) if (d / "LICENSES").is_dir() else []
        allfiles = [p for p in d.rglob("*") if p.is_file()]
        per_component[name] = {
            "files": len(allfiles), "LICENSES": texts,
            "hasLGPL3": "LGPL-3.0-only.txt" in texts, "hasCommercial": any("Qt-Commercial" in p.name for p in allfiles),
            "testExampleFiles": sum(1 for p in allfiles if {"tests", "examples", "manual"} & set(p.relative_to(d).parts)),
        }
    runpaths = {}
    for p in files:
        try:
            paths = search_paths(p)
        except Exception:
            paths = None
        if paths:
            runpaths[rel(p)] = paths
    absolute = {k: v for k, v in runpaths.items() if any(e and not e.startswith(("$ORIGIN", "${ORIGIN}")) for e in v)}
    lib = root / "usr/lib"
    plugin_copies = sorted(p.name for p in (root / "usr/plugins").rglob("*.so") if (lib / p.name).exists())
    private = root / "usr/lib/putgd"
    aws_copies = sorted(p.name for p in private.iterdir() if (lib / p.name).exists() and not (lib / p.name).is_symlink())
    soname_copies = sorted(n for n in ("libaws-c-common.so.1", "libs2n.so.1", "libaws-c-s3.so.0unstable")
                           if (lib / n).exists())
    doc_copies = sorted(p.name for p in (root / "usr/share/doc").iterdir() if p.name != "putgd")
    proton_doc = root / DOC / "proton"
    proton_regular = sorted(p.name for p in proton_doc.iterdir() if not p.is_symlink()) if proton_doc.is_dir() else []
    proton_links = sorted(p.name for p in proton_doc.iterdir() if p.is_symlink()) if proton_doc.is_dir() else []
    notice = root / DOC / "runtime-notices.txt"
    inventory = root / DOC / "bundled-components.json"
    inv = json.loads(inventory.read_text()) if inventory.is_file() else None
    metainfo = (root / "usr/share/metainfo/gd.put.Putgd.metainfo.xml").read_text()
    seen, identical = {}, []
    for p in files:
        name = rel(p)
        if name.startswith(DOC + "/"):
            continue
        d = sha(p)
        if d in seen:
            identical.append([seen[d], name])
        seen.setdefault(d, name)
    result = {
        "image": {"file": image.name, "sha256": sha(image), "bytes": image.stat().st_size},
        "extracted": {"bytes": sum(p.stat().st_size for p in files), "files": len(files),
                      "symlinks": len(entries) - len(files)},
        "runtimeNotices": {"firstLine": notice.read_text().splitlines()[0], "bytes": notice.stat().st_size,
                           "nativeText": NATIVE in notice.read_bytes()},
        "nativeTextAnywhereInDocs": sorted(rel(p) for p in files if rel(p).startswith(DOC + "/") and NATIVE in p.read_bytes()),
        "inventory": None if inv is None else {
            "components": len(inv["components"]),
            "ubuntuPackages": sum(c["id"].startswith("ubuntu:") for c in inv["components"]),
            "upstream": {c["id"]: [c["version"], c["license"], len(c["files"]), len(c["licenseFiles"])]
                         for c in inv["components"] if not c["id"].startswith("ubuntu:")},
            "listedPaths": sum(len(c["files"]) + len(c["licenseFiles"]) for c in inv["components"])
                           + len(inv["notices"]) + len(inv["sharedLicenseTexts"])},
        "appstreamLicense": re.findall(r"<project_license>([^<]*)</project_license>", metainfo),
        "licenceTexts": per_component,
        "distroCopyrightFiles": len(list((lic / "distro").glob("*/copyright"))),
        "searchPaths": {"elfWithPaths": len(runpaths), "absolute": absolute},
        "linuxdeployLeftovers": sorted(n for n in ("AppRun.wrapped", "apprun-hooks") if (root / n).exists()),
        "duplicates": {"qtPluginCopiesInUsrLib": plugin_copies, "awsCopiesInUsrLib": sorted(set(aws_copies + soname_copies)),
                       "distroDocCopies": len(doc_copies), "protonDocRegularFiles": proton_regular,
                       "protonDocLinks": proton_links, "identicalFilesOutsideDocs": identical},
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
