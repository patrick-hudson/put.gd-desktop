"""File-manager plugin package checks (#1105) with synthetic tool output; the
real readelf/nm/load checks run in the package builds."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/releases"))
import audit_file_manager_plugins as audit

LOADED = json.dumps({"label": "Upload with put.gd"})


class FileManagerPluginAuditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pgfm-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "root"
        self.build = Path(self.tmp.name) / "build"
        self.needed = {}
        for name, (directory, filename) in audit.PLUGINS.items():
            self.write(f"usr/lib/x86_64-linux-gnu/{directory}/{filename}")
            (self.build / "src/filemanager").mkdir(parents=True, exist_ok=True)
            (self.build / f"src/filemanager/putgd-{name}-check").write_text("")
            self.needed[filename] = ["libc.so.6"]
        self.needed["putgd.so"] = ["libKF6KIOWidgets.so.6", "libQt6Core.so.6", "libc.so.6"]
        self.write("usr/share/caja/extensions/libputgd-caja.caja-extension")
        self.write(audit.KIO_SERVICE_MENU)
        (self.root / audit.KIO_SERVICE_MENU).write_text("[Desktop Entry]\nActions=putgdUpload;\n")

    def write(self, relative):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x")

    def run_tool(self, argv, **options):
        if argv[0] == "readelf":
            return "".join(f" 0x1 (NEEDED) Shared library: [{soname}]\n" for soname in self.needed[Path(argv[-1]).name])
        if argv[0].endswith("-check"):
            return LOADED + "\n"
        raise AssertionError(argv)

    def audit(self, **options):
        with patch.object(audit, "run", side_effect=self.run_tool), \
                patch.object(audit, "peony_symbols", return_value={"undefined": 1}):
            return audit.audit_plugins(self.root, self.build, **options)

    def test_each_suite_needs_its_plugins(self):
        result = self.audit(package_target="ubuntu24.04", metadata={"requires": "libc6 (>= 2.14), libstdc++6 (>= 5)"})
        self.assertEqual(sorted(result), ["caja", "kio", "nautilus", "peony"])
        self.assertEqual(result["nautilus"]["path"], "/usr/lib/x86_64-linux-gnu/nautilus/extensions-4/libputgd-nautilus.so")
        (self.root / "usr/lib/x86_64-linux-gnu/peony-extensions/libputgd-peony.so").unlink()
        self.assertEqual(sorted(self.audit(package_target="fedora44")), ["caja", "kio", "nautilus"])
        with self.assertRaisesRegex(ValueError, "lacks the peony plugin"):
            self.audit(package_target="debian13")
        # #2136: KF6 targets need the Dolphin plugin; Ubuntu 24.04 has no KF6.
        self.write("usr/lib/x86_64-linux-gnu/peony-extensions/libputgd-peony.so")
        (self.root / "usr/lib/x86_64-linux-gnu/kf6/kfileitemaction/putgd.so").unlink()
        (self.root / audit.KIO_SERVICE_MENU).write_text("[Desktop Entry]\nActions=putgdUpload;putgdUploadEncrypted;\n")
        self.assertEqual(sorted(self.audit(package_target="ubuntu24.04")), ["caja", "nautilus", "peony"])
        for target in ("ubuntu26.04", "fedora45", "archlinux"):
            with self.subTest(target), self.assertRaisesRegex(ValueError, "lacks the kio plugin"):
                self.audit(package_target=target)

    def test_one_encrypted_entry_in_dolphin(self):
        menu = self.root / audit.KIO_SERVICE_MENU
        menu.parent.mkdir(parents=True, exist_ok=True)
        plain = "[Desktop Entry]\nActions=putgdUpload;\n\n[Desktop Action putgdUpload]\nName=Upload with put.gd\n"
        both = plain.replace("putgdUpload;", "putgdUpload;putgdUploadEncrypted;") + "\n[Desktop Action putgdUploadEncrypted]\nName=Upload encrypted with put.gd\n"
        menu.write_text(plain)
        self.assertIn("kio", self.audit())
        menu.write_text(both)
        with self.assertRaisesRegex(ValueError, "beside the KIO plugin"):
            self.audit()
        (self.root / "usr/lib/x86_64-linux-gnu/kf6/kfileitemaction/putgd.so").unlink()
        self.assertNotIn("kio", self.audit())
        menu.write_text(plain)
        with self.assertRaisesRegex(ValueError, "no KIO plugin offers it"):
            self.audit()

    def test_plugins_link_no_file_manager_or_qt(self):
        for soname in ("libnautilus-extension.so.4", "libcaja-extension.so.1", "libpeony.so.3", "libQt5Core.so.5", "libKF6KIOWidgets.so.6"):
            self.needed["libputgd-peony.so"] = ["libc.so.6", soname]
            with self.subTest(soname), self.assertRaisesRegex(ValueError, "must resolve them from the file manager"):
                self.audit()

    def test_every_package_has_the_kio_service_menu(self):
        (self.root / audit.KIO_SERVICE_MENU).unlink()
        self.assertIn("kio", self.audit())
        with self.assertRaisesRegex(ValueError, "lacks /usr/share/kio/servicemenus/putgd-upload.desktop"):
            self.audit(package_target="ubuntu24.04")

    def test_kio_plugin_links_kio(self):
        # #2136: the Dolphin plugin links KF6 KIO and Qt, unlike the others.
        self.assertIn("libKF6KIOWidgets.so.6", self.audit()["kio"]["needed"])
        self.needed["putgd.so"] = ["libQt6Core.so.6", "libc.so.6"]
        with self.assertRaisesRegex(ValueError, "doesn't link libKF6KIOWidgets"):
            self.audit()

    def test_package_depends_on_no_file_manager(self):
        for requires in ("libc6, nautilus", ["libc.so.6()(64bit)", "libcaja-extension.so.1()(64bit)"], "libqt5core5t64", "libkf6kiowidgets6",
                         ["libKF6KIOWidgets.so.6()(64bit)"]):
            with self.subTest(requires), self.assertRaisesRegex(ValueError, "depends on a file manager"):
                self.audit(metadata={"requires": requires})

    def test_rpm_provides_no_plugin(self):
        with self.assertRaisesRegex(ValueError, "provides its file-manager plugins"):
            self.audit(metadata={"requires": [], "provides": ["putgd = 1", "libputgd-nautilus.so()(64bit)"]})

    def test_caja_needs_its_extension_description(self):
        (self.root / "usr/share/caja/extensions/libputgd-caja.caja-extension").unlink()
        with self.assertRaisesRegex(ValueError, "caja-extension"):
            self.audit()


if __name__ == "__main__":
    unittest.main()
