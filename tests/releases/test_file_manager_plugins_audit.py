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
        self.write("usr/share/caja/extensions/libputgd-caja.caja-extension")

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
        self.assertEqual(sorted(result), ["caja", "nautilus", "peony"])
        self.assertEqual(result["nautilus"]["path"], "/usr/lib/x86_64-linux-gnu/nautilus/extensions-4/libputgd-nautilus.so")
        (self.root / "usr/lib/x86_64-linux-gnu/peony-extensions/libputgd-peony.so").unlink()
        self.assertEqual(sorted(self.audit(package_target="fedora44")), ["caja", "nautilus"])
        with self.assertRaisesRegex(ValueError, "lacks the peony plugin"):
            self.audit(package_target="debian13")

    def test_plugins_link_no_file_manager_or_qt(self):
        for soname in ("libnautilus-extension.so.4", "libcaja-extension.so.1", "libpeony.so.3", "libQt5Core.so.5"):
            self.needed["libputgd-peony.so"] = ["libc.so.6", soname]
            with self.subTest(soname), self.assertRaisesRegex(ValueError, "must resolve them from the file manager"):
                self.audit()

    def test_package_depends_on_no_file_manager(self):
        for requires in ("libc6, nautilus", ["libc.so.6()(64bit)", "libcaja-extension.so.1()(64bit)"], "libqt5core5t64"):
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
