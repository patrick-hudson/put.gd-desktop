#!/usr/bin/env python3
"""AppImage notices, inventory and content checks (#417, #421) on synthetic AppDirs."""
import json
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/releases"))
import appimage_notices as notices  # noqa: E402

QT_LICENSE = "LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only"
QTBASE_COMMIT = "c07c2d5a527a644d36e7853d55132ae38921682f"


def make_elf(path: Path, *, needed=(), soname=None, runpath=None, padding=b""):
    """A minimal 64-bit little-endian shared object with a dynamic section."""
    strings, offsets = b"\0", {}
    for name in [*needed, *([soname] if soname else []), *([runpath] if runpath else [])]:
        offsets[name] = len(strings)
        strings += name.encode() + b"\0"
    strtab = 64 + 2 * 56
    dynamic = strtab + len(strings) + (-len(strings) % 8)
    tags = [(1, offsets[name]) for name in needed]
    if soname:
        tags.append((14, offsets[soname]))
    if runpath:
        tags.append((29, offsets[runpath]))
    tags += [(5, strtab), (10, len(strings)), (0, 0)]
    body = b"".join(struct.pack("<qQ", tag, value) for tag, value in tags)
    total = dynamic + len(body) + len(padding)
    header = b"\x7fELF" + bytes([2, 1, 1]) + b"\0" * 9
    header += struct.pack("<HHIQQQIHHHHHH", 3, 62, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
    programs = struct.pack("<IIQQQQQQ", 1, 4, 0, 0, 0, total, total, 0x1000)
    programs += struct.pack("<IIQQQQQQ", 2, 6, dynamic, dynamic, dynamic, len(body), len(body), 8)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + programs + strings + b"\0" * (-len(strings) % 8) + body + padding)


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


SPEC = {"schemaVersion": 1, "components": [
    {"id": "putgd", "name": "put.gd", "version": {"from": "build"},
     "source": {"from": "build", "repository": "https://example.invalid/putgd"}, "license": "MIT",
     "licenseTexts": {"MIT": "usr/share/doc/putgd/LICENSE"},
     "files": ["AppRun", "usr/bin/putgd", "usr/share/metainfo/gd.put.Putgd.metainfo.xml", "usr/lib/putgd/libputgd-s3.so"],
     "noticeFiles": ["usr/share/doc/putgd/LICENSE"]},
    {"id": "aws", "name": "AWS SDK", "version": {"from": "dependencyLock", "key": "awsSdk"},
     "source": {"from": "dependencyLock", "key": "awsSdk"}, "license": "Apache-2.0",
     "licenseTexts": {"Apache-2.0": "usr/share/doc/putgd/aws-sdk-LICENSE"},
     "files": ["usr/lib/putgd/libaws-*", "usr/share/putgd/dependency-lock.json"],
     "noticeFiles": ["usr/share/doc/putgd/aws-sdk-LICENSE"]},
    {"id": "ffmpeg", "name": "FFmpeg", "version": {"from": "lock", "key": "ffmpeg"},
     "source": {"from": "lock", "key": "ffmpeg"}, "license": "GPL-2.0-or-later",
     "licenseTexts": {"GPL-2.0-or-later": "COPYING.GPLv2"}, "licenseFiles": ["COPYING.*"],
     "files": ["usr/lib/libavcodec.so*"]}]}


class NoticesTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="pgnot-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.app = self.root / "AppDir"
        self.qt = self.root / "qt"
        self.sources = self.root / "sources"
        spec = write(self.root / "components.json", json.dumps(SPEC))
        patcher = patch.object(notices, "COMPONENTS", spec)
        patcher.start()
        self.addCleanup(patcher.stop)
        patcher = patch.object(notices, "dpkg_fields", lambda package: ["1.2-3", package + "-src", "1.2-3"])
        patcher.start()
        self.addCleanup(patcher.stop)
        self.build = {"version": "26.1022.1-nightly", "gitCommit": "a" * 40, "gitTree": "b" * 40}
        self.lock = {"tools": [], "buildInputs": {
            "distribution": "Ubuntu 24.04",
            "ffmpeg": {"repository": "https://example.invalid/ffmpeg.git", "release": "7.1.5", "commit": "f" * 40},
            "qtLicenseSources": [{"repository": "https://example.invalid/qtbase.git", "release": "6.8.3",
                                  "commit": QTBASE_COMMIT}]}}
        self.make_qt()
        self.make_sources()
        self.make_appdir()

    def make_qt(self):
        make_elf(self.qt / "lib/libQt6Core.so.6.8.3", soname="libQt6Core.so.6")
        (self.qt / "lib/libQt6Core.so.6").symlink_to("libQt6Core.so.6.8.3")
        make_elf(self.qt / "plugins/platforms/libqxcb.so")
        attribution = ("\nInformation extracted from:\n     /src_dir/qtbase/src/3rdparty/tinycbor/qt_attribution.json\n"
                       "    Entry index: 0\n    Id: tinycbor\n")
        document = {
            "files": [{"SPDXID": "SPDXRef-File-core", "fileName": "./lib/libQt6Core.so.6.8.3"},
                      {"SPDXID": "SPDXRef-File-xcb", "fileName": "./plugins/platforms/libqxcb.so"}],
            "packages": [
                {"SPDXID": "SPDXRef-Package-qtbase", "name": "qtbase", "versionInfo": QTBASE_COMMIT[:8] + "+HEAD"},
                {"SPDXID": "SPDXRef-Package-qtbase-qt-module-Core", "name": "Core", "versionInfo": "6.8.3",
                 "licenseConcluded": QT_LICENSE},
                {"SPDXID": "SPDXRef-Package-qtbase-qt-plugin-QXcbIntegrationPlugin", "name": "QXcbIntegrationPlugin",
                 "versionInfo": "6.8.3", "licenseConcluded": QT_LICENSE},
                {"SPDXID": "SPDXRef-Package-qtbase-qt-3rdparty-sources-Core-Attribution-tinycbor",
                 "name": "Core_Attribution_tinycbor", "versionInfo": "0.6.1", "licenseConcluded": "MIT",
                 "comment": attribution, "copyrightText": "Copyright (C) Intel"},
                {"SPDXID": "SPDXRef-Package-qtbase-system-3rdparty-GLIB2", "name": "GLIB2"}],
            "relationships": [
                {"spdxElementId": "SPDXRef-Package-qtbase-qt-module-Core", "relationshipType": "CONTAINS",
                 "relatedSpdxElement": "SPDXRef-File-core"},
                {"spdxElementId": "SPDXRef-Package-qtbase-qt-plugin-QXcbIntegrationPlugin",
                 "relationshipType": "CONTAINS", "relatedSpdxElement": "SPDXRef-File-xcb"},
                {"spdxElementId": "SPDXRef-Package-qtbase-qt-module-Core", "relationshipType": "DEPENDS_ON",
                 "relatedSpdxElement": "SPDXRef-Package-qtbase-qt-3rdparty-sources-Core-Attribution-tinycbor"},
                {"spdxElementId": "SPDXRef-Package-qtbase-qt-module-Core", "relationshipType": "DEPENDS_ON",
                 "relatedSpdxElement": "SPDXRef-Package-qtbase-system-3rdparty-GLIB2"}]}
        write(self.qt / "sbom/qtbase-6.8.3.spdx.json", json.dumps(document))

    def make_sources(self):
        qtbase = self.sources / "qtbase"
        for name in ("LGPL-3.0-only", "GPL-2.0-only", "GPL-3.0-only", "MIT", "LicenseRef-Qt-Commercial"):
            write(qtbase / "LICENSES" / (name + ".txt"), name + " text\n")
        write(qtbase / "src/3rdparty/tinycbor/qt_attribution.json",
              json.dumps({"Id": "tinycbor", "Name": "TinyCBOR", "LicenseFile": "LICENSE"}))
        write(qtbase / "src/3rdparty/tinycbor/LICENSE", "Copyright (C) Intel\nMIT terms\n")
        write(qtbase / "tests/manual/fonts/LICENSE.txt", "a test font licence\n")
        write(self.sources / "ffmpeg/COPYING.GPLv2", "GPL 2 text\n")
        write(self.sources / "ffmpeg/COPYING.LGPLv2.1", "LGPL 2.1 text\n")

    def make_appdir(self):
        app = self.app
        write(app / "AppRun", "#!/bin/sh\n")
        write(app / "AppRun.wrapped", "#!/bin/sh\n")
        write(app / "apprun-hooks/linuxdeploy-plugin-qt-hook.sh", "export QT_QPA_PLATFORMTHEME=gtk2\n")
        make_elf(app / "usr/bin/putgd", needed=["libQt6Core.so.6"], runpath="$ORIGIN/../lib")
        make_elf(app / "usr/lib/libQt6Core.so.6", soname="libQt6Core.so.6", runpath="$ORIGIN")
        make_elf(app / "usr/plugins/platforms/libqxcb.so", runpath="$ORIGIN/../../lib")
        make_elf(app / "usr/lib/libqxcb.so", runpath="$ORIGIN")  # linuxdeploy's copy of the plugin
        make_elf(app / "usr/lib/libavcodec.so.61", soname="libavcodec.so.61")
        make_elf(app / "usr/lib/libfoo.so.1", soname="libfoo.so.1")
        make_elf(app / "usr/lib/putgd/libputgd-s3.so", needed=["libaws-cpp-sdk-core.so"], runpath="$ORIGIN")
        make_elf(app / "usr/lib/putgd/libaws-cpp-sdk-core.so", runpath="$ORIGIN")
        make_elf(app / "usr/lib/putgd/libaws-c-common.so.0.14.5", soname="libaws-c-common.so.1", runpath="$ORIGIN")
        (app / "usr/lib/putgd/libaws-c-common.so.1").symlink_to("libaws-c-common.so.0.14.5")
        for name in ("libputgd-s3.so", "libaws-cpp-sdk-core.so"):
            (app / "usr/lib" / name).write_bytes((app / "usr/lib/putgd" / name).read_bytes())
        (app / "usr/lib/libaws-c-common.so.1").write_bytes((app / "usr/lib/putgd/libaws-c-common.so.0.14.5").read_bytes())
        write(app / "usr/share/putgd/dependency-lock.json", json.dumps(
            {"awsSdk": {"repository": "https://example.invalid/aws.git", "release": "1.11.907", "commit": "c" * 40}}))
        write(app / "usr/share/metainfo/gd.put.Putgd.metainfo.xml",
              "<component>\n  <project_license>MIT AND Apache-2.0</project_license>\n</component>\n")
        write(app / "usr/share/doc/putgd/LICENSE", "MIT put.gd\n")
        write(app / "usr/share/doc/putgd/aws-sdk-LICENSE", "Apache 2.0\n")
        write(app / notices.NOTICE, "put.gd package dependency notice\n" + notices.NATIVE_NOTICE + "\n")
        write(app / notices.GPL_NOTICE, "put.gd AppImage GPL notice\n")
        write(app / "usr/share/doc/libfoo1/copyright", "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n\n"
              "Files: *\nCopyright: Foo\nLicense: LGPL-2.1+\n")

    def distro_packages(self, appdir, claimed):
        self.assertNotIn("usr/lib/libfoo.so.1", claimed)
        self.assertIn("usr/lib/libavcodec.so.61", claimed)
        target = appdir / notices.DISTRO_LICENSES / "libfoo1/copyright"
        target.parent.mkdir(parents=True)
        target.write_bytes((appdir / "usr/share/doc/libfoo1/copyright").read_bytes())
        write(appdir / notices.COMMON_LICENSES / "LGPL-2.1", "LGPL 2.1\n")
        return {"libfoo1": ["usr/lib/libfoo.so.1"]}

    def finish(self, sources=("qtbase", "ffmpeg")):
        return notices.finish(self.app, qt=self.qt, lock=self.lock,
                              license_sources=[self.sources / name for name in sources],
                              distro_packages=self.distro_packages, version=self.build["version"],
                              build=self.build, gpl=[{"binaryPackage": "libx264-164"}])

    def inventory(self):
        return json.loads((self.app / notices.INVENTORY).read_text())

    def test_elf_dynamic_reads_needed_soname_and_runpath(self):
        path = self.root / "lib.so"
        make_elf(path, needed=["libc.so.6", "libm.so.6"], soname="libx.so.1", runpath="$ORIGIN:/opt/x")
        self.assertEqual(notices.elf_dynamic(path), {"needed": ["libc.so.6", "libm.so.6"], "soname": "libx.so.1",
                                                     "rpath": [], "runpath": ["$ORIGIN", "/opt/x"]})
        self.assertIsNone(notices.elf_dynamic(write(self.root / "script", "#!/bin/sh\n")))

    def test_finished_image_passes_its_audit(self):
        summary = self.finish()
        result = notices.audit(self.app)
        inventory = self.inventory()
        components = {component["id"]: component for component in inventory["components"]}
        self.assertEqual(sorted(components), ["aws", "ffmpeg", "putgd", "qtbase", "ubuntu:libfoo1"])
        qtbase = components["qtbase"]
        self.assertEqual(qtbase["license"], "LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only")
        self.assertEqual(qtbase["version"], "6.8.3")
        self.assertEqual(qtbase["files"], ["usr/lib/libQt6Core.so.6", "usr/plugins/platforms/libqxcb.so"])
        self.assertEqual(sorted(qtbase["licenseTexts"]), ["GPL-2.0-only", "GPL-3.0-only", "LGPL-3.0-only", "MIT"])
        self.assertEqual([(item["name"], item["version"], item["usedBy"]) for item in qtbase["embedded"]],
                         [("TinyCBOR", "0.6.1", ["libQt6Core.so.6"])])
        licenses = self.app / notices.LICENSES
        self.assertTrue((licenses / "qtbase/LICENSES/LGPL-3.0-only.txt").is_file())
        self.assertTrue((licenses / "qtbase/src/3rdparty/tinycbor/LICENSE").is_file())
        self.assertFalse((licenses / "qtbase/LICENSES/LicenseRef-Qt-Commercial.txt").exists())
        self.assertFalse((licenses / "qtbase/tests").exists())
        self.assertEqual(components["ubuntu:libfoo1"]["license"], "LGPL-2.1-or-later")
        self.assertEqual(components["aws"]["files"], ["usr/lib/putgd/libaws-c-common.so.0.14.5",
                                                      "usr/lib/putgd/libaws-c-common.so.1",
                                                      "usr/lib/putgd/libaws-cpp-sdk-core.so",
                                                      "usr/share/putgd/dependency-lock.json"])
        expression = ("MIT AND Apache-2.0 AND GPL-2.0-or-later AND (LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only)"
                      " AND LGPL-2.1-or-later")
        self.assertEqual(inventory["appstreamLicense"], expression)
        self.assertIn("<project_license>" + expression + "</project_license>",
                      (self.app / notices.METAINFO).read_text())
        notice = (self.app / notices.NOTICE).read_text()
        self.assertTrue(notice.startswith("put.gd AppImage 26.1022.1-nightly: "))
        self.assertNotIn(notices.NATIVE_NOTICE, notice)
        self.assertIn("Qt qtbase 6.8.3", notice)
        self.assertEqual(sorted(item["path"] for item in summary["removed"]["libraryCopies"]),
                         ["usr/lib/libaws-c-common.so.1", "usr/lib/libaws-cpp-sdk-core.so",
                          "usr/lib/libputgd-s3.so", "usr/lib/libqxcb.so"])
        self.assertEqual(summary["removed"]["linuxdeployLeftovers"], ["AppRun.wrapped", "apprun-hooks"])
        self.assertEqual(summary["removed"]["distroDocCopies"], ["usr/share/doc/libfoo1"])
        self.assertEqual(result["components"], 5)

    def test_different_usr_lib_copy_of_a_private_library_is_rejected(self):
        (self.app / "usr/lib/libaws-cpp-sdk-core.so").write_bytes(b"\x7fELF other bytes")
        with self.assertRaisesRegex(ValueError, "copy differs from the private library"):
            notices.remove_library_copies(self.app)

    def test_plugin_copy_that_a_library_needs_is_kept(self):
        make_elf(self.app / "usr/lib/libuser.so", needed=["libqxcb.so"])
        with self.assertRaisesRegex(ValueError, "depends on the plugin copy"):
            notices.remove_library_copies(self.app)

    def test_qt_sbom_mismatch_with_the_lock_is_rejected(self):
        self.lock["buildInputs"]["qtLicenseSources"][0]["commit"] = "0" * 40
        with self.assertRaisesRegex(ValueError, "the lock pins"):
            self.finish()

    def test_missing_qt_licence_text_is_rejected(self):
        (self.sources / "qtbase/LICENSES/LGPL-3.0-only.txt").unlink()
        with self.assertRaisesRegex(ValueError, "no LICENSES/LGPL-3.0-only.txt"):
            self.finish()

    def test_licence_source_without_bundled_files_is_rejected(self):
        write(self.sources / "qtdeclarative/LICENSES/LGPL-3.0-only.txt", "text\n")
        self.lock["buildInputs"]["qtLicenseSources"].append(
            {"repository": "https://example.invalid/qtdeclarative.git", "release": "6.8.3", "commit": "d" * 40})
        with self.assertRaisesRegex(ValueError, "no files in the image: qtdeclarative"):
            self.finish(("qtbase", "ffmpeg", "qtdeclarative"))

    def test_qt_file_outside_the_sbom_is_rejected(self):
        make_elf(self.qt / "lib/libQt6Extra.so.6")
        make_elf(self.app / "usr/lib/libQt6Extra.so.6")
        with self.assertRaisesRegex(ValueError, "without an entry in Qt's SPDX documents: usr/lib/libQt6Extra.so.6"):
            self.finish()

    def test_audit_rejects_regressions(self):
        self.finish()
        notices.audit(self.app)
        cases = {
            "native notice": (lambda: write(self.app / notices.DOC / "extra-notice.txt", notices.NATIVE_NOTICE),
                              "native packages' notice text"),
            "unlisted file": (lambda: write(self.app / "usr/share/putgd/new.json", "{}"), "missing from the inventory"),
            "missing licence text": (lambda: (self.app / notices.LICENSES / "qtbase/LICENSES/GPL-3.0-only.txt").unlink(),
                                     "lists files the image lacks|Missing or empty licence file"),
            "absolute runpath": (lambda: make_elf(self.app / "usr/plugins/platforms/libqxcb.so", runpath="/home/runner/qt/lib"),
                                 "Absolute library search path"),
            "leftover": (lambda: write(self.app / "AppRun.wrapped", "#!/bin/sh\n"), "linuxdeploy leftovers|missing from the inventory"),
            "commercial licence": (lambda: write(self.app / notices.LICENSES / "qtbase/LICENSES/LicenseRef-Qt-Commercial.txt", "x"),
                                   "missing from the inventory|do not apply"),
            "appstream": (lambda: notices.write_metainfo_license(self.app, "MIT"), "AppStream project_license differs"),
        }
        for name, (change, message) in cases.items():
            with self.subTest(name):
                saved = self.root / "saved"
                shutil.copytree(self.app, saved, symlinks=True)
                try:
                    change()
                    with self.assertRaisesRegex(ValueError, message):
                        notices.audit(self.app)
                finally:
                    shutil.rmtree(self.app)
                    saved.rename(self.app)

    def test_audit_rejects_second_copies(self):
        self.finish()
        (self.app / "usr/lib/libqxcb.so").write_bytes((self.app / "usr/plugins/platforms/libqxcb.so").read_bytes())
        with self.assertRaisesRegex(ValueError, "Second copies in the image: usr/lib/libqxcb.so"):
            notices.audit_hygiene(self.app, notices.elf_files(self.app))

    def test_proton_doc_copies_become_links_to_the_helpers_copies(self):
        helper = self.app / "usr/libexec/putgd/proton"
        write(helper / "redistribution/SOURCES.json", "{}\n")
        write(helper / "THIRD-PARTY-NOTICES.txt", "notices\n")
        write(self.app / notices.DOC / "proton/SOURCES.json", "{}\n")
        write(self.app / notices.DOC / "proton/THIRD-PARTY-NOTICES.txt", "notices\n")
        write(self.app / notices.DOC / "proton/README.md", "doc only\n")
        linked = notices.link_proton_doc_copies(self.app)
        self.assertEqual(linked, ["usr/share/doc/putgd/proton/SOURCES.json",
                                  "usr/share/doc/putgd/proton/THIRD-PARTY-NOTICES.txt"])
        link = self.app / notices.DOC / "proton/SOURCES.json"
        self.assertTrue(link.is_symlink())
        self.assertEqual(link.resolve(), (helper / "redistribution/SOURCES.json").resolve())
        self.assertFalse((self.app / notices.DOC / "proton/README.md").is_symlink())


class ExpressionTest(unittest.TestCase):
    def test_dep5_files_star_licence_maps_to_spdx(self):
        header = "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n\n"
        cases = {"LGPL-2+ and LGPL-2.1+ and Expat": "LGPL-2.0-or-later AND LGPL-2.1-or-later AND MIT",
                 "GPL-2+": "GPL-2.0-or-later", "BSD-3-clause or GPL-2": "BSD-3-Clause OR GPL-2.0-only",
                 "GPL-2+ or AFL-2.1, and Expat and Tcl-BSDish": "NOASSERTION", "public-domain": "NOASSERTION"}
        for declared, expected in cases.items():
            with self.subTest(declared):
                text = header + "Files: debian/*\nLicense: GPL-3+\n\nFiles: *\nCopyright: x\nLicense: " + declared + "\n text\n"
                self.assertEqual(notices.dep5_license(text), (declared, expected))
        self.assertEqual(notices.dep5_license("This package was debianized by\n"), (None, "NOASSERTION"))
        self.assertEqual(notices.dep5_license(header + "Files: src/*\nLicense: MIT\n"), (None, "NOASSERTION"))

    def test_combined_expression_keeps_alternatives_and_drops_duplicates(self):
        self.assertEqual(notices.combined(["MIT", "MIT AND GPL-3.0-only", "LGPL-3.0-only OR GPL-2.0-only",
                                           "(A OR B) AND MIT", "NOASSERTION"]),
                         "MIT AND GPL-3.0-only AND (LGPL-3.0-only OR GPL-2.0-only) AND (A OR B)")
        self.assertEqual(notices.open_source_terms(QT_LICENSE), "LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only")
        self.assertEqual(notices.spdx_ids("GPL-3.0-only WITH Qt-GPL-exception-1.0 OR (MIT AND Zlib)"),
                         ["GPL-3.0-only", "Qt-GPL-exception-1.0", "MIT", "Zlib"])


class ComponentSpecTest(unittest.TestCase):
    def test_component_spec_matches_the_lock(self):
        spec = json.loads(notices.COMPONENTS.read_text())
        lock = json.loads((ROOT / "packaging/appimage/tools.lock.json").read_text())
        ids = [item["id"] for item in spec["components"]]
        self.assertEqual(len(ids), len(set(ids)))
        tools = {item["name"] for item in lock["tools"]}
        for item in spec["components"]:
            with self.subTest(item["id"]):
                for key in ("name", "version", "source", "license", "files"):
                    self.assertIn(key, item)
                if item["version"]["from"] == "lock":
                    self.assertIn("commit", lock["buildInputs"][item["version"]["key"]])
                if item["version"]["from"] == "tool":
                    self.assertIn(item["version"]["key"], tools)
                for identifier in notices.spdx_ids(item["license"]):
                    if "licenseFiles" not in item:
                        self.assertIn(identifier, item.get("licenseTexts", {}))
        # Every licence source the build passes is a component or a Qt module the image bundles.
        script = (ROOT / "tools/releases/build-appimage.sh").read_text()
        passed = sorted(line.split("build/dependencies/appimage/", 1)[1].split()[0]
                        for line in script.splitlines() if "--license-source build/dependencies/appimage/" in line)
        qt = sorted(item["repository"].rsplit("/", 1)[1].removesuffix(".git") for item in lock["buildInputs"]["qtLicenseSources"])
        self.assertEqual(passed, sorted([*qt, *(item["id"] for item in spec["components"] if "licenseFiles" in item)]))


if __name__ == "__main__":
    unittest.main()
