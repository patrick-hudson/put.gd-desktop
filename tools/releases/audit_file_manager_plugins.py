#!/usr/bin/env python3
"""Audit a native package's file-manager menu plugins (#1105): where each is
installed, that none needs a file-manager library or Qt (readelf -d), that the
Peony plugin's undefined symbols are all Qt, libstdc++ or libc ones and none is
libpeony's (nm -D), and that each loads the way its file manager loads it
(the build tree's putgd-<fm>-check, QPluginLoader for Peony). Not desktop acceptance."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

# name: (directory the file manager loads from, module file)
PLUGINS = {"nautilus": ("nautilus/extensions-4", "libputgd-nautilus.so"),
           "caja": ("caja/extensions-2.0", "libputgd-caja.so"),
           "peony": ("peony-extensions", "libputgd-peony.so")}
# Package matrix names in .github/workflows/package.yml.
REQUIRED = {"ubuntu24.04": ("nautilus", "caja", "peony"), "ubuntu26.04": ("nautilus", "caja", "peony"),
            "debian13": ("nautilus", "caja", "peony"), "fedora44": ("nautilus", "caja")}
FORBIDDEN_NEEDED = re.compile(r"^lib(nautilus-extension|caja-extension|peony|Qt\d)")
FORBIDDEN_DEPENDENCY = re.compile(r"nautilus|caja|peony|qt5", re.IGNORECASE)
PEONY_SYMBOL_SOURCES = re.compile(r"^lib(Qt\d\w+|stdc\+\+|c|m|gcc_s)[.]so")


def run(argv: list[str], **options) -> str:
    return subprocess.run(argv, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=60, **options).stdout


def dynamic_symbols(path: str | Path, *flags: str) -> set[str]:
    symbols = set()
    for line in run(["nm", "-D", *flags, str(path)]).splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[-2] not in ("w", "v"):
            symbols.add(fields[-1].split("@")[0])
    return symbols


def peony_symbols(plugin: Path, check: Path) -> dict:
    undefined = dynamic_symbols(plugin, "--undefined-only")
    sources = re.findall(r"^\s*(\S+)\s+=>\s+(\S+)", run(["ldd", str(check)]), re.MULTILINE)
    allowed = set()
    for soname, location in sources:
        if PEONY_SYMBOL_SOURCES.match(soname):
            allowed |= dynamic_symbols(location, "--defined-only")
    peony = [line.split()[-1] for line in run(["ldconfig", "-p"]).splitlines() if line.strip().startswith("libpeony.so")]
    from_peony = sorted(undefined & dynamic_symbols(peony[0], "--defined-only")) if peony else []
    if from_peony:
        raise ValueError(f"Peony plugin uses libpeony symbols: {from_peony[:10]}")
    unknown = sorted(undefined - allowed)
    if unknown:
        raise ValueError(f"Peony plugin needs symbols outside Qt, libstdc++ and libc: {unknown[:10]}")
    return {"undefined": len(undefined), "checkedAgainstLibpeony": bool(peony)}


def audit_plugins(root: Path, build: Path, *, package_target: str | None = None,
                  package: Path | None = None, metadata: dict | None = None) -> dict:
    root = root.resolve()
    required = REQUIRED.get(package_target, ())
    result = {}
    for name, (directory, filename) in PLUGINS.items():
        found = [path for path in root.glob("usr/lib*/**/" + filename) if path.parent.as_posix().endswith("/" + directory)]
        if not found:
            if name in required:
                raise ValueError(f"{package_target} package lacks the {name} plugin {directory}/{filename}")
            continue
        if len(found) != 1:
            raise ValueError(f"Expected one {name} plugin, found {len(found)}")
        plugin = found[0]
        needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", run(["readelf", "-d", str(plugin)]))
        if forbidden := [soname for soname in needed if FORBIDDEN_NEEDED.match(soname)]:
            raise ValueError(f"{name} plugin needs {forbidden}; it must resolve them from the file manager")
        check = build / "src/filemanager" / f"putgd-{name}-check"
        if not check.is_file():
            raise ValueError(f"Build tree lacks {check.name} for the packaged {name} plugin")
        environment = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
        loaded = json.loads(run([str(check), str(plugin)], env=environment).strip().splitlines()[-1])
        record = {"path": "/" + plugin.relative_to(root).as_posix(), "needed": needed, "loaded": loaded}
        if name == "peony":
            record["symbols"] = peony_symbols(plugin, check)
        if name == "caja" and not (root / "usr/share/caja/extensions/libputgd-caja.caja-extension").is_file():
            raise ValueError("Caja plugin lacks /usr/share/caja/extensions/libputgd-caja.caja-extension")
        result[name] = record
    if metadata is not None:
        requires = metadata["requires"]
        entries = requires.split(",") if isinstance(requires, str) else requires
        if dependencies := [entry.strip() for entry in entries if FORBIDDEN_DEPENDENCY.search(entry)]:
            raise ValueError(f"Package depends on a file manager or Qt 5: {dependencies}")
        if provided := [entry for entry in metadata.get("provides", []) if entry.startswith("libputgd-")]:
            raise ValueError(f"RPM provides its file-manager plugins: {provided}")
    if package is not None and package.suffix == ".rpm" and result:
        # Co-owned, as Fedora asks for directories whose owner the package doesn't require.
        listed = set(run(["rpm", "-qlp", str(package)]).splitlines())
        for record in result.values():
            directory = str(Path(record["path"]).parent)
            if directory not in listed:
                raise ValueError(f"RPM does not co-own {directory}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path, help="Extracted package or DESTDIR tree")
    parser.add_argument("--build", required=True, type=Path, help="Build tree with the putgd-<fm>-check programs")
    parser.add_argument("--package-target", help="Package matrix name; requires its plugins")
    args = parser.parse_args()
    print(json.dumps(audit_plugins(args.root, args.build.resolve(), package_target=args.package_target), indent=2))


if __name__ == "__main__":
    main()
