#!/usr/bin/env python3
"""AppRun lifecycle tests with synthetic executables; not desktop acceptance."""
import os
import importlib.util
import json
import sys
import re
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
        self.host_data = self.root / "host-data"
        self.env = dict(os.environ, APPDIR=str(self.app), XDG_DATA_HOME=str(self.data),
                        XDG_DATA_DIRS=str(self.host_data),
                        XDG_CURRENT_DESKTOP="KDE", PATH=str(self.root / "bin") + ":" + os.environ["PATH"],
                        PG_TEST_RECEIPT=str(self.receipt))
        for name in ("WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "APPIMAGE"):
            self.env.pop(name, None)
        self.write(self.root / "bin/kbuildsycoca6", "#!/bin/sh\nexit 0\n")

    def write(self, path, data):
        path.write_text(data)
        path.chmod(0o755)

    def mount_windowed_helpers(self):
        """The image's windowed helpers and the icon AppRun names; synthetic files."""
        for name in ("putgd-ui", "putgd-record"):
            self.write(self.app / "usr/bin" / name, "#!/bin/sh\nexit 0\n")
        icon = self.app / portable.WINDOW_ICON
        icon.parent.mkdir(parents=True)
        icon.write_text("<svg/>\n")
        return icon

    @staticmethod
    def identity(pid):
        fields = Path("/proc/%d/stat" % pid).read_text().rsplit(") ", 1)[1].split()
        return "%d %s" % (pid, fields[19])

    def entries(self):
        applications = self.data / "applications"
        return sorted(path.name for path in applications.iterdir()) if applications.exists() else []

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

    def test_host_ca_bundle_is_named_for_the_image(self):
        """#329: the first readable host bundle, SSL_CERT_FILE first; never a stale inherited value."""
        self.env["XDG_CURRENT_DESKTOP"] = "GNOME"
        self.write(self.app / "usr/bin/putgd-launcher", "#!/bin/sh\nprintf '%s' \"${PUTGD_CA_BUNDLE-unset}\" > \"$PG_TEST_RECEIPT\"\n")
        chosen = self.root / "chosen bundle.pem"
        chosen.write_text("synthetic\n")
        self.env.update(SSL_CERT_FILE=str(chosen), PUTGD_CA_BUNDLE="/stale/inherited.pem")
        self.assertEqual(subprocess.run([str(APP_RUN)], env=self.env).returncode, 0)
        self.assertEqual(self.receipt.read_text(), str(chosen))
        host = ("/etc/ssl/certs/ca-certificates.crt", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem")
        expected = next((path for path in host if os.path.isfile(path) and os.access(path, os.R_OK)), "unset")
        for missing in (str(self.root / "absent.pem"), str(self.root)):
            with self.subTest(ssl_cert_file=missing):
                self.env["SSL_CERT_FILE"] = missing
                self.assertEqual(subprocess.run([str(APP_RUN)], env=self.env).returncode, 0)
                self.assertEqual(self.receipt.read_text(), expected)

    def test_termination_stops_exact_child_and_cleans_permission(self):
        self.mount_windowed_helpers()
        self.env.update(WAYLAND_DISPLAY="wayland-0")
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
            self.assertEqual(self.entries(), [])
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

    def test_wayland_adds_icon_entries_for_window_app_ids_and_removes_them(self):
        icon = self.mount_windowed_helpers()
        self.env.update(XDG_CURRENT_DESKTOP="GNOME", WAYLAND_DISPLAY="wayland-0",
                        APPIMAGE=str(self.root / "put.gd $1.AppImage"))
        # Each section starts with its own marker line: the entries' owner key
        # ends in a start time, which can end in the digits of a file mode.
        self.write(self.app / "usr/bin/putgd-launcher", """#!/bin/sh
for id in putgd-ui putgd-record; do
    printf '@@%s\\n' "$id" >> "$PG_TEST_RECEIPT"
    cat "$XDG_DATA_HOME/applications/$id.desktop" >> "$PG_TEST_RECEIPT"
    stat -c 'mode=%a' "$XDG_DATA_HOME/applications/$id.desktop" >> "$PG_TEST_RECEIPT"
done
printf '@@listing\\n' >> "$PG_TEST_RECEIPT"
ls -A "$XDG_DATA_HOME/applications" >> "$PG_TEST_RECEIPT"
printf 'parent=%s\\n' "$PPID" >> "$PG_TEST_RECEIPT"
exit 0
""")
        process = subprocess.Popen([str(APP_RUN)], env=self.env)
        self.assertEqual(process.wait(timeout=10), 0)
        sections = re.split(r"^@@(\S+)\n", self.receipt.read_text(), flags=re.MULTILINE)
        self.assertEqual(sections[0], "")
        sections = dict(zip(sections[1::2], sections[2::2]))
        self.assertEqual(list(sections), ["putgd-ui", "putgd-record", "listing"])
        listing = sections["listing"]
        self.assertIn("parent=%d" % process.pid, listing)
        for app_id, name in (("putgd-ui", "put.gd"), ("putgd-record", "put.gd screen recorder")):
            lines = sections[app_id].splitlines()
            self.assertEqual(lines[-1], "mode=600")
            self.assertEqual(lines[0], "[Desktop Entry]")
            self.assertIn("Name=" + name, lines)
            self.assertIn("Icon=" + str(icon), lines)
            self.assertIn("NoDisplay=true", lines)
            self.assertIn("StartupWMClass=" + app_id, lines)
            self.assertIn('Exec="' + str(self.root) + '/put.gd \\$1.AppImage"', lines)
            owner = [line for line in lines if line.startswith("X-PutGD-AppImage-Owner=")]
            self.assertEqual(len(owner), 1)
            self.assertRegex(owner[0], r"^X-PutGD-AppImage-Owner=%d [0-9]+$" % process.pid)
        # No staged copy and no KDE permission entry off Plasma.
        self.assertEqual(listing.split("parent=")[0].split(), ["putgd-record.desktop", "putgd-ui.desktop"])
        self.assertEqual(self.entries(), [])

    def test_wayland_session_type_alone_adds_icon_entries(self):
        self.mount_windowed_helpers()
        self.env.update(XDG_CURRENT_DESKTOP="GNOME", XDG_SESSION_TYPE="wayland")
        self.write(self.app / "usr/bin/putgd-launcher",
                   "#!/bin/sh\nls \"$XDG_DATA_HOME/applications\" > \"$PG_TEST_RECEIPT\"\n")
        self.assertEqual(subprocess.run([str(APP_RUN)], env=self.env).returncode, 0)
        self.assertEqual(self.receipt.read_text().split(), ["putgd-record.desktop", "putgd-ui.desktop"])
        self.assertEqual(self.entries(), [])

    def test_wayland_keeps_host_and_foreign_entries_and_skips_supervisor(self):
        self.mount_windowed_helpers()
        self.env.update(XDG_CURRENT_DESKTOP="GNOME", WAYLAND_DISPLAY="wayland-0")
        (self.host_data / "applications").mkdir(parents=True)
        (self.host_data / "applications/putgd-ui.desktop").write_text("distro entry\n")
        applications = self.data / "applications"
        applications.mkdir(parents=True)
        foreign = applications / "putgd-record.desktop"
        foreign.write_text("[Desktop Entry]\nName=user entry\n")
        self.write(self.app / "usr/bin/putgd-launcher", "#!/bin/sh\nprintf '%s\\n' \"$$\" > \"$PG_TEST_RECEIPT\"\n")
        process = subprocess.Popen([str(APP_RUN)], env=self.env)
        self.assertEqual(process.wait(timeout=10), 0)
        # Nothing to remove: AppRun executes the launcher in place.
        self.assertEqual(int(self.receipt.read_text()), process.pid)
        self.assertEqual(self.entries(), ["putgd-record.desktop"])
        self.assertEqual(foreign.read_text(), "[Desktop Entry]\nName=user entry\n")
        self.assertEqual((self.host_data / "applications/putgd-ui.desktop").read_text(), "distro entry\n")

    def test_entries_of_killed_launch_are_replaced_and_running_launch_kept(self):
        self.mount_windowed_helpers()
        applications = self.data / "applications"
        applications.mkdir(parents=True)
        live = "[Desktop Entry]\nX-PutGD-AppImage-Owner=%s\n" % self.identity(os.getpid())
        dead_pid = os.getpid()
        dead = "[Desktop Entry]\nX-PutGD-AppImage-Owner=%d 1\n" % dead_pid
        (applications / "putgd-ui.desktop").write_text(dead)
        (applications / "putgd-record.desktop").write_text(live)
        (applications / "putgd-appimage-STALE001.desktop").write_text(dead)
        (applications / "putgd-record-appimage-LIVE0001.desktop").write_text(live)
        (applications / "putgd-appimage-OLDFORMAT.desktop").write_text("[Desktop Entry]\nExec=/gone\n")
        self.env.update(WAYLAND_DISPLAY="wayland-0")
        self.write(self.app / "usr/bin/putgd-launcher", """#!/bin/sh
cat "$XDG_DATA_HOME/applications/putgd-ui.desktop" > "$PG_TEST_RECEIPT"
""")
        process = subprocess.Popen([str(APP_RUN)], env=self.env)
        self.assertEqual(process.wait(timeout=10), 0)
        # The killed launch's entries go; this launch wrote its own putgd-ui entry.
        self.assertIn("X-PutGD-AppImage-Owner=%d " % process.pid, self.receipt.read_text())
        self.assertEqual(self.entries(), ["putgd-appimage-OLDFORMAT.desktop", "putgd-record-appimage-LIVE0001.desktop",
                                          "putgd-record.desktop"])
        self.assertEqual((applications / "putgd-record.desktop").read_text(), live)

    def test_kde_wayland_rebuilds_cache_after_removing_all_entries(self):
        self.mount_windowed_helpers()
        calls = self.root / "sycoca-calls"
        self.env.update(WAYLAND_DISPLAY="wayland-0", PG_SYCOCA=str(calls))
        self.write(self.root / "bin/kbuildsycoca6", "#!/bin/sh\nls \"$XDG_DATA_HOME/applications\" >> \"$PG_SYCOCA\"\necho -- >> \"$PG_SYCOCA\"\n")
        self.write(self.app / "usr/bin/putgd-launcher", "#!/bin/sh\nexit 0\n")
        self.assertEqual(subprocess.run([str(APP_RUN)], env=self.env).returncode, 0)
        registered, removed, _ = calls.read_text().split("--\n")
        self.assertIn("putgd-ui.desktop", registered.split())
        self.assertIn("putgd-record.desktop", registered.split())
        self.assertEqual(len(registered.split()), 4)
        self.assertEqual(removed.split(), [])
        self.assertEqual(self.entries(), [])

    def test_non_kde_does_not_register_permission(self):
        self.env["XDG_CURRENT_DESKTOP"] = "X-Cinnamon"
        self.write(self.app / "usr/bin/putgd-launcher", "#!/bin/sh\nexit 0\n")
        completed = subprocess.run([str(APP_RUN)], env=self.env)
        self.assertEqual(completed.returncode, 0)
        self.assertFalse((self.data / "applications").exists())


class WindowIconAuditTest(unittest.TestCase):
    """The icon AppRun's Wayland entries name must be inside the image."""

    def test_deployed_icon_is_accepted_and_missing_one_rejected(self):
        with tempfile.TemporaryDirectory(prefix="pgicon-") as directory:
            appdir = Path(directory)
            with self.assertRaisesRegex(ValueError, "window icon is missing"):
                portable.audit_window_icon(appdir)
            icon = appdir / portable.WINDOW_ICON
            icon.parent.mkdir(parents=True)
            icon.write_text("<svg/>\n")
            self.assertEqual(portable.audit_window_icon(appdir), portable.WINDOW_ICON)

    def test_apprun_names_the_audited_icon(self):
        self.assertIn("$appdir/" + portable.WINDOW_ICON, APP_RUN.read_text())


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


class PlatformAuditTest(unittest.TestCase):
    """The image worker's start contract, with a synthetic putgd-ui script."""
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="pgplatform-")
        self.addCleanup(self.directory.cleanup)
        self.app = Path(self.directory.name)
        (self.app / "usr/bin").mkdir(parents=True)
        self.platforms = self.app / "usr/plugins/platforms"
        self.platforms.mkdir(parents=True)
        for name in ("libqxcb.so", *portable.EXTRA_PLATFORM_PLUGINS):
            (self.platforms / name).write_bytes(b"\x7fELFsynthetic")
        self.formats = self.app / "usr/plugins/imageformats"
        self.formats.mkdir(parents=True)
        for name in portable.IMAGE_FORMAT_PLUGINS:
            (self.formats / name).write_bytes(b"\x7fELFsynthetic")
        self.inventory = {"privateLibraryDirectory": "usr/lib/putgd"}
        self.worker("""[ "$1" = --image-job ] && [ "$QT_QPA_PLATFORM" = offscreen ] || exit 9
[ -z "${DISPLAY-}" ] && [ -z "${WAYLAND_DISPLAY-}" ] && [ -z "${QT_PLUGIN_PATH-}" ] || exit 8
[ -p /dev/stdin ] && [ -p /dev/stdout ] || exit 2
cat >/dev/null
exit 3
""")

    def worker(self, body):
        path = self.app / "usr/bin/putgd-ui"
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)

    def audit(self):
        with patch.dict(os.environ, {"DISPLAY": ":99", "WAYLAND_DISPLAY": "wayland-9",
                                     "QT_PLUGIN_PATH": "/usr/lib/qt6/plugins"}):
            return portable.audit_platforms(self.app, self.inventory)

    def test_offscreen_is_deployed_for_the_image_worker(self):
        self.assertIn("libqoffscreen.so", portable.EXTRA_PLATFORM_PLUGINS)
        result = self.audit()
        self.assertTrue(result["imageWorkerStartedOffscreen"])
        self.assertIn("libqoffscreen.so", result["platformPlugins"])

    def test_webp_plugin_is_required_for_history_previews(self):
        self.assertIn("libqwebp.so", self.audit()["imageFormatPlugins"])
        (self.formats / "libqwebp.so").unlink()
        with self.assertRaisesRegex(ValueError, "image format plugins missing from the image: libqwebp.so"):
            self.audit()

    def test_missing_platform_plugin_is_rejected(self):
        (self.platforms / "libqoffscreen.so").unlink()
        with self.assertRaisesRegex(ValueError, "missing from the image: libqoffscreen.so"):
            self.audit()

    def test_aborting_worker_is_rejected_with_its_qt_message(self):
        self.worker("""printf '%s\\n' 'qt.qpa.plugin: Could not find the Qt platform plugin "offscreen" in ""' >&2
kill -ABRT $$
""")
        with self.assertRaisesRegex(ValueError, 'Could not find the Qt platform plugin "offscreen"'):
            self.audit()


class WaylandAuditTest(unittest.TestCase):
    """The Wayland start contract, with a synthetic compositor and putgd-ui; not desktop acceptance."""
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="pgwayland-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.app = self.root / "AppDir"
        (self.app / "usr/bin").mkdir(parents=True)
        self.plugins = self.app / "usr/plugins"
        for name in ("wayland-shell-integration/liblayer-shell.so", *portable.WAYLAND_CLIENT_PLUGINS):
            (self.plugins / name).parent.mkdir(parents=True, exist_ok=True)
            (self.plugins / name).write_bytes(b"\x7fELFsynthetic")
        self.inventory = {"privateLibraryDirectory": "usr/lib/putgd"}
        self.receipt = self.root / "compositor-pid"
        # Like weston: create the named socket in the private runtime directory, run until TERM.
        self.compositor = self.script("compositor", """for argument; do
    case $argument in --socket=*) socket=${argument#--socket=} ;; esac
done
printf '%s\\n' "$$" > "$PG_COMPOSITOR_PID"
trap 'exit 0' TERM
: > "$XDG_RUNTIME_DIR/$socket"
while :; do sleep 0.05; done
""")
        self.worker("""[ "$1" = --image-job ] && [ "$QT_QPA_PLATFORM" = wayland ] || exit 9
[ -e "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && [ "$XDG_SESSION_TYPE" = wayland ] || exit 8
[ -z "${DISPLAY-}" ] && [ -z "${QT_PLUGIN_PATH-}" ] || exit 7
[ -p /dev/stdin ] && [ -p /dev/stdout ] || exit 2
printf '%s\\n' "qt.qpa.wayland: Using the 'xdg-shell' shell integration" >&2
cat >/dev/null
exit 3
""")

    def script(self, name, body):
        path = self.root / name
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)
        return path

    def worker(self, body):
        path = self.app / "usr/bin/putgd-ui"
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)

    def audit(self, compositor=None):
        with patch.dict(os.environ, {"DISPLAY": ":99", "WAYLAND_DISPLAY": "wayland-9",
                                     "QT_PLUGIN_PATH": "/usr/lib/qt6/plugins",
                                     "PG_COMPOSITOR_PID": str(self.receipt)}):
            return portable.audit_wayland(self.app, self.inventory,
                                          compositor or (str(self.compositor), "--socket={socket}"))

    def assert_compositor_stopped(self):
        pid = int(self.receipt.read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_distro_client_plugin_set_is_deployed(self):
        self.assertEqual(set(portable.WAYLAND_CLIENT_PLUGINS), {
            "wayland-shell-integration/libxdg-shell.so", "wayland-decoration-client/libbradient.so",
            "wayland-decoration-client/libadwaita.so",
            "wayland-graphics-integration-client/libqt-plugin-wayland-egl.so"})
        self.assertEqual(portable.HEADLESS_COMPOSITOR[0], "weston")

    def test_worker_starts_with_xdg_shell_and_compositor_is_stopped(self):
        result = self.audit()
        self.assertTrue(result["imageWorkerStartedOnWayland"])
        self.assertEqual(result["waylandShellIntegration"], "xdg-shell")
        self.assertIn("wayland-shell-integration/libxdg-shell.so", result["waylandClientPlugins"])
        self.assert_compositor_stopped()

    def test_missing_shell_integration_is_rejected(self):
        (self.plugins / "wayland-shell-integration/libxdg-shell.so").unlink()
        with self.assertRaisesRegex(ValueError, "missing from the image: wayland-shell-integration/libxdg-shell.so"):
            self.audit()

    def test_aborting_worker_is_rejected_with_its_qt_message(self):
        self.worker("""printf '%s\\n' 'qt.qpa.wayland: No shell integration named "xdg-shell" found' \\
    'qt.qpa.wayland: Loading shell integration failed.' \\
    'qt.qpa.plugin: Could not load the Qt platform plugin "wayland" in "" even though it was found.' >&2
kill -ABRT $$
""")
        with self.assertRaisesRegex(ValueError, "Loading shell integration failed"):
            self.audit()
        self.assert_compositor_stopped()

    def test_other_shell_integration_is_rejected(self):
        self.worker("""printf '%s\\n' "qt.qpa.wayland: Using the 'wl-shell' shell integration" >&2
cat >/dev/null
exit 3
""")
        with self.assertRaisesRegex(ValueError, "with xdg-shell"):
            self.audit()

    def test_compositor_that_exits_is_rejected_with_its_message(self):
        failing = self.script("failing", "printf '%s\\n' $$ > \"$PG_COMPOSITOR_PID\"; echo 'fatal: no backend' >&2; exit 1\n")
        with self.assertRaisesRegex(ValueError, "compositor did not start: fatal: no backend"):
            self.audit((str(failing),))

    def test_missing_compositor_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "requires " + str(self.root / "absent")):
            self.audit((str(self.root / "absent"), "--socket={socket}"))


if __name__ == "__main__":
    unittest.main()
