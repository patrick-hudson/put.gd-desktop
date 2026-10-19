#!/usr/bin/env python3
"""AppRun lifecycle tests with synthetic executables; not desktop acceptance."""
import os
import importlib.util
import json
import sys
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
APP_RUN = ROOT / "packaging/appimage/AppRun"
sys.path.insert(0, str(ROOT / "tools/releases"))
spec = importlib.util.spec_from_file_location("portable_appimage", ROOT / "tools/releases/portable_appimage.py")
portable = importlib.util.module_from_spec(spec)
spec.loader.exec_module(portable)


class AppRunTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="pgair-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.app = self.root / "mount with spaces"
        (self.app / "usr/bin").mkdir(parents=True)
        (self.root / "bin").mkdir()
        self.data = self.root / "data"
        self.receipt = self.root / "receipt"
        self.env = dict(os.environ, APPDIR=str(self.app), XDG_DATA_HOME=str(self.data),
                        XDG_CURRENT_DESKTOP="KDE", PATH=str(self.root / "bin") + ":" + os.environ["PATH"],
                        PG_TEST_RECEIPT=str(self.receipt))
        self.write(self.root / "bin/kbuildsycoca6", "#!/bin/sh\nexit 0\n")

    def write(self, path, data):
        path.write_text(data)
        path.chmod(0o755)

    def test_kde_registers_current_mount_and_cleans_only_owned_entry(self):
        applications = self.data / "applications"
        applications.mkdir(parents=True)
        existing = applications / "putgd.desktop"
        existing.write_text("user entry\n")
        self.write(self.app / "usr/bin/putgd-launcher", """#!/bin/sh
for entry in "$XDG_DATA_HOME"/applications/putgd-appimage-*.desktop; do
    cat "$entry" > "$PG_TEST_RECEIPT"
    stat -c '%a' "$entry" >> "$PG_TEST_RECEIPT"
done
printf '%s\n' "$@" >> "$PG_TEST_RECEIPT"
exit 7
""")
        completed = subprocess.run([str(APP_RUN), "--background", "argument with spaces"], env=self.env)
        self.assertEqual(completed.returncode, 7)
        receipt = self.receipt.read_text()
        self.assertIn('Exec="' + str(self.app / "usr/bin/putgd") + '"', receipt)
        self.assertIn("X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2", receipt)
        self.assertIn("600\n--background\nargument with spaces\n", receipt)
        self.assertEqual(existing.read_text(), "user entry\n")
        self.assertEqual(list(applications.glob("putgd-appimage-*.desktop")), [])

    def test_kde_registers_mounted_recorder_for_screencast_and_cleans_it(self):
        self.write(self.app / "usr/bin/putgd-record", "#!/bin/sh\nexit 0\n")
        self.write(self.app / "usr/bin/putgd-launcher", """#!/bin/sh
for entry in "$XDG_DATA_HOME"/applications/putgd-record-appimage-*.desktop; do
    cat "$entry" > "$PG_TEST_RECEIPT"
    stat -c '%a' "$entry" >> "$PG_TEST_RECEIPT"
done
exit 0
""")
        completed = subprocess.run([str(APP_RUN)], env=self.env)
        self.assertEqual(completed.returncode, 0)
        receipt = self.receipt.read_text()
        self.assertIn('Exec="' + str(self.app / "usr/bin/putgd-record") + '"', receipt)
        self.assertIn("X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1", receipt)
        self.assertNotIn("ScreenShot2", receipt)
        self.assertTrue(receipt.endswith("600\n"))
        self.assertEqual(list((self.data / "applications").glob("putgd-*appimage-*.desktop")), [])

    def test_bundled_libva_is_used_only_without_a_host_copy(self):
        (self.app / "usr/lib/putgd-host-fallback").mkdir(parents=True)
        self.env["XDG_CURRENT_DESKTOP"] = "GNOME"
        self.write(self.app / "usr/bin/putgd-launcher", "#!/bin/sh\nprintf '%s\\n' \"$LD_LIBRARY_PATH\" > \"$PG_TEST_RECEIPT\"\n")
        fallback = str(self.app / "usr/lib/putgd-host-fallback")
        for cache, expected in (("\tlibva.so.2 (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/libva.so.2\n", False),
                                ("\tlibva-drm.so.2 (libc6,x86-64) => /usr/lib/x86_64-linux-gnu/libva-drm.so.2\n", True)):
            with self.subTest(cache=cache):
                self.write(self.root / "bin/ldconfig", "#!/bin/sh\nprintf '" + cache.replace("\\", "\\\\") + "'\n")
                self.assertEqual(subprocess.run([str(APP_RUN)], env=self.env).returncode, 0)
                self.assertEqual(fallback in self.receipt.read_text(), expected)

    def test_termination_stops_exact_child_and_cleans_permission(self):
        self.write(self.app / "usr/bin/putgd-launcher", """#!/bin/sh
trap 'exit 0' TERM
printf '%s\n' "$$" > "$PG_TEST_RECEIPT"
while :; do sleep 0.05; done
""")
        process = subprocess.Popen([str(APP_RUN)], env=self.env)
        try:
            deadline = time.monotonic() + 5
            while not self.receipt.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(self.receipt.exists())
            child = int(self.receipt.read_text())
            process.send_signal(signal.SIGTERM)
            self.assertEqual(process.wait(timeout=5), 143)
            with self.assertRaises(ProcessLookupError):
                os.kill(child, 0)
            self.assertEqual(list((self.data / "applications").glob("putgd-appimage-*.desktop")), [])
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    def test_host_tool_uses_original_environment_and_child_keeps_umask(self):
        host_receipt = self.root / "host-receipt"
        self.env.update(LD_LIBRARY_PATH="/ambient/libraries", QT_PLUGIN_PATH="/ambient/plugins",
                        PG_HOST_RECEIPT=str(host_receipt))
        self.write(self.root / "bin/kbuildsycoca6", "#!/bin/sh\nprintf '%s\\n' \"$LD_LIBRARY_PATH\" \"$QT_PLUGIN_PATH\" >> \"$PG_HOST_RECEIPT\"\n")
        self.write(self.app / "usr/bin/putgd-launcher", "#!/bin/sh\numask > \"$PG_TEST_RECEIPT\"\n")
        result = subprocess.run([str(APP_RUN)], env=self.env, umask=0o022)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(host_receipt.read_text(), "/ambient/libraries\n/ambient/plugins\n" * 2)
        self.assertEqual(self.receipt.read_text().strip(), "0022")

    def test_headless_identity_avoids_kde_registration(self):
        self.write(self.root / "bin/kbuildsycoca6", "#!/bin/sh\nexit 9\n")
        self.write(self.app / "usr/bin/putgd-launcher", "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$PG_TEST_RECEIPT\"\n")
        completed = subprocess.run([str(APP_RUN), "--version-json"], env=self.env)
        self.assertEqual(completed.returncode, 0)
        self.assertEqual(self.receipt.read_text(), "--version-json\n")
        self.assertFalse((self.data / "applications").exists())

    def test_non_kde_does_not_register_permission(self):
        self.env["XDG_CURRENT_DESKTOP"] = "X-Cinnamon"
        self.write(self.app / "usr/bin/putgd-launcher", "#!/bin/sh\nexit 0\n")
        completed = subprocess.run([str(APP_RUN)], env=self.env)
        self.assertEqual(completed.returncode, 0)
        self.assertFalse((self.data / "applications").exists())


class DeploymentRejectionTest(unittest.TestCase):
    """Reject unsafe assembly receipts; synthetic subprocess data is no ELF proof."""
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="pgdeploy-")
        self.addCleanup(self.directory.cleanup)
        self.app = Path(self.directory.name)
        (self.app / "usr/bin").mkdir(parents=True)
        (self.app / "usr/bin/putgd").write_bytes(b"\x7fELFsynthetic")
        self.manifest = {"version": "0.1.1-rc.1", "gitCommit": "a" * 40}
        self.inventory = {"manifest": self.manifest, "programs": ["putgd"],
                          "privateLibraryDirectory": "usr/lib/putgd"}

    def run_audit(self, identity=None, dependencies=""):
        def command(argv, **kwargs):
            result = dependencies if argv[0] == "ldd" else json.dumps(identity or self.manifest)
            return subprocess.CompletedProcess(argv, 0, stdout=result)
        with patch.object(portable.subprocess, "run", side_effect=command):
            return portable.audit_deployed(self.app, self.inventory)

    def test_same_version_helper_from_different_commit_is_rejected(self):
        wrong = {**self.manifest, "gitCommit": "b" * 40}
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            self.run_audit(identity=wrong)

    def test_unresolved_dependency_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unresolved"):
            self.run_audit(dependencies="libmissing.so => not found\n")

    def test_recording_probe_must_stay_inside_the_image(self):
        media = {"gstreamer": {"requiredElements": ["ximagesrc"]}, "hostLibraries": ["libva.so.*"]}
        (self.app / "usr/lib/gstreamer-1.0").mkdir(parents=True)
        inside = str(self.app / "usr/lib/gstreamer-1.0")
        good = {"gstreamer": {"available": True, "version": "1.24.2", "elements": {"ximagesrc": "1.24.2"},
                              "pluginDirectories": [inside]},
                "ffmpeg": {"h264Decoder": {"usable": True, "name": "h264"}}, "formats": ["mp4", "webm", "gif", "webp"],
                "exportEncoders": {"h264": ["libx264"], "h265": ["libx265"], "vp9": ["libvpx-vp9"]}}
        good["gstreamer"]["usableH264Encoders"] = ["x264enc"]
        self.assertEqual(portable.audit_media(self.app, media, json.dumps(good))["pluginDirectories"], [inside])
        leaked = json.loads(json.dumps(good))
        leaked["gstreamer"]["pluginDirectories"].append("/usr/lib/x86_64-linux-gnu/gstreamer-1.0")
        with self.assertRaisesRegex(ValueError, "outside the image"):
            portable.audit_media(self.app, media, json.dumps(leaked))
        # Without a GPU the bundled x264/x265 must be what works.
        no_fallback = json.loads(json.dumps(good))
        no_fallback["exportEncoders"]["h265"] = []
        with self.assertRaisesRegex(ValueError, "libx264/libx265"):
            portable.audit_media(self.app, media, json.dumps(no_fallback))
        no_live = json.loads(json.dumps(good))
        no_live["gstreamer"]["usableH264Encoders"] = []
        with self.assertRaisesRegex(ValueError, "x264enc"):
            portable.audit_media(self.app, media, json.dumps(no_live))
        missing = json.loads(json.dumps(good))
        missing["gstreamer"]["elements"]["ximagesrc"] = None
        with self.assertRaisesRegex(ValueError, "elements missing"):
            portable.audit_media(self.app, media, json.dumps(missing))
        (self.app / "usr/lib/libva.so.2").write_bytes(b"\x7fELFsynthetic")
        with self.assertRaisesRegex(ValueError, "Host-provided libraries were bundled"):
            portable.audit_media(self.app, media, json.dumps(good))

    def test_host_qt_or_aws_dependency_is_rejected(self):
        for library in ("libQt6Core.so.6", "libaws-cpp-sdk-core.so"):
            with self.subTest(library=library), self.assertRaisesRegex(ValueError, "outside AppDir"):
                self.run_audit(dependencies=library + " => /usr/lib/" + library + " (0x1234)\n")


if __name__ == "__main__":
    unittest.main()
