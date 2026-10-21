#!/usr/bin/env python3
"""The AppDir root holds exactly one integrable desktop entry (#306)."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/releases"))
spec = importlib.util.spec_from_file_location("portable_appimage", ROOT / "tools/releases/portable_appimage.py")
portable = importlib.util.module_from_spec(spec)
spec.loader.exec_module(portable)


class RootDesktopEntryTest(unittest.TestCase):
    """Synthetic AppDirs laid out as the image job leaves them after linuxdeploy."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="pgroot-")
        self.addCleanup(directory.cleanup)
        self.appdir = Path(directory.name) / "AppDir"
        self.applications = self.appdir / "usr/share/applications"
        self.applications.mkdir(parents=True)
        (self.applications / "putgd.desktop").write_bytes((ROOT / "packaging/appimage/putgd.desktop").read_bytes())
        (self.applications / "putgd-ui.desktop").write_bytes((ROOT / "packaging/linux/putgd-ui.desktop").read_bytes())
        (self.applications / "putgd-record.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=put.gd screen recorder\nExec=putgd-record\nNoDisplay=true\n")
        self.link("putgd.desktop")

    def link(self, name):
        os.symlink("usr/share/applications/" + name, self.appdir / name)

    def test_linuxdeploy_root_link_to_the_ui_entry_is_removed(self):
        # What the nightly images of fbd9ce8e and 71f241b8 carried.
        self.link("putgd-ui.desktop")
        with self.assertRaisesRegex(ValueError, "exactly one desktop entry.*putgd-ui.desktop"):
            portable.audit_root_desktop_entry(self.appdir)
        self.assertEqual(portable.keep_one_root_desktop_entry(self.appdir), ["putgd-ui.desktop"])
        self.assertEqual(portable.audit_root_desktop_entry(self.appdir), "putgd.desktop")
        self.assertEqual(sorted(path.name for path in self.appdir.glob("*.desktop")), ["putgd.desktop"])

    def test_entries_apprun_and_the_helpers_need_stay_in_applications(self):
        self.link("putgd-ui.desktop")
        self.link("putgd-record.desktop")
        portable.keep_one_root_desktop_entry(self.appdir)
        self.assertEqual(sorted(path.name for path in self.applications.iterdir()),
                         ["putgd-record.desktop", "putgd-ui.desktop", "putgd.desktop"])
        self.assertIn('"$applications"/putgd-ui.desktop', (ROOT / "packaging/appimage/AppRun").read_text())

    def test_clean_root_is_accepted_unchanged(self):
        self.assertEqual(portable.keep_one_root_desktop_entry(self.appdir), [])
        self.assertEqual(portable.audit_root_desktop_entry(self.appdir), "putgd.desktop")

    def test_unknown_root_entry_is_not_silently_removed(self):
        (self.appdir / "other.desktop").write_text("[Desktop Entry]\nType=Application\nExec=other\n")
        with self.assertRaisesRegex(ValueError, "Unexpected desktop entry.*other.desktop"):
            portable.keep_one_root_desktop_entry(self.appdir)
        self.assertTrue((self.appdir / "other.desktop").exists())

    def test_missing_or_misnamed_root_entry_is_rejected(self):
        (self.appdir / "putgd.desktop").unlink()
        with self.assertRaisesRegex(ValueError, "found: none"):
            portable.audit_root_desktop_entry(self.appdir)
        self.link("putgd-ui.desktop")
        with self.assertRaisesRegex(ValueError, "found: putgd-ui.desktop"):
            portable.audit_root_desktop_entry(self.appdir)

    def test_root_entry_that_asks_not_to_be_integrated_is_rejected(self):
        for key in ("NoDisplay", "Hidden"):
            with self.subTest(key=key):
                entry = self.applications / "putgd.desktop"
                entry.write_text("[Desktop Entry]\nType=Application\nName=put.gd\nExec=putgd-launcher\n" + key + "=true\n")
                with self.assertRaisesRegex(ValueError, "must not set " + key):
                    portable.audit_root_desktop_entry(self.appdir)

    def test_root_entry_pointing_outside_the_appdir_is_rejected(self):
        (self.appdir / "putgd.desktop").unlink()
        outside = self.appdir.parent / "putgd.desktop"
        outside.write_bytes((ROOT / "packaging/appimage/putgd.desktop").read_bytes())
        os.symlink(outside, self.appdir / "putgd.desktop")
        with self.assertRaisesRegex(ValueError, "inside the AppDir"):
            portable.audit_root_desktop_entry(self.appdir)

    def test_shipped_main_entry_is_integrable(self):
        text = (ROOT / "packaging/appimage/putgd.desktop").read_text()
        self.assertNotIn("NoDisplay", text)
        self.assertNotIn("Hidden", text)


if __name__ == "__main__":
    unittest.main()
