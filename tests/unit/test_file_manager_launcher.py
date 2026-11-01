#!/usr/bin/env python3
"""The AppImage's file-manager launcher and AppRun's --upload fast path (#1105).

The launcher is rendered from its template as FileManagerIntegration writes it.
Stand-in images and systemd-run record their arguments; no image is mounted.
"""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "src/app/file-manager-launcher.sh"
LOGIN = ROOT / "src/app/appimage-login-start.sh"
APP_RUN = ROOT / "packaging/appimage/AppRun"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_appimage_login_start import FIXTURE, NIGHTLY  # noqa: E402

SCOPE = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-gnome-org.gnome.Nautilus-4242.scope\n"
SERVICE = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/dbus-:1.2-org.gnome.Nautilus@0.service\n"
RECORDER = """#!/bin/sh
printf '%s\\0' "$@" >> "$PG_RECEIPT"
exit "${PG_STATUS:-0}"
"""


def shell_quoted(value):
    return "'" + value.replace("'", "'\\''") + "'"


def discovery():
    """The login script's AppImage discovery, spliced in as FileManagerIntegration does."""
    text = LOGIN.read_text()
    begin = text.index("# The user's private group")
    end = text.index("# Remove only this installation's managed startup files")
    return text[begin:end].strip()


class LauncherTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="pgfm-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.images = self.root / "Applications with 'quotes'"
        self.images.mkdir(mode=0o700)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.receipt = self.root / "receipt"
        self.calls = self.root / "systemd-run calls"
        self.cgroup = self.root / "cgroup"
        self.cgroup.write_text(SCOPE)
        self.image = self.images / "putgd-26.1031.1-x86_64.AppImage"
        self.write(self.image, RECORDER)
        self.own = self.root / "put.gd's data/file-manager"
        self.own.mkdir(parents=True)
        self.script = self.own / "putgd-upload"
        self.user_runtime = self.root / "user-runtime"

    def write(self, path, text, mode=0o755):
        path.write_text(text)
        path.chmod(mode)

    def render(self, configured=None, info="", cgroup=None):
        values = {"CONFIGURED": str(configured or self.image), "UPDATE_INFORMATION": info, "SCRIPT": str(self.script),
                  "RECORD": str(self.own / "appimage.owned"), "DIRECTORY": str(self.own),
                  "CGROUP": str(cgroup or self.cgroup), "WATCH_UNIT": "putgd-file-manager-fixture",
                  "USER_RUNTIME": str(self.user_runtime)}
        text = LAUNCHER.read_text().replace("# __PUTGD_DISCOVERY__", discovery())
        for key, value in values.items():
            text = text.replace("__PUTGD_" + key + "__", shell_quoted(value))
        text = text.replace("__PUTGD_ID__", "b" * 64)
        self.assertNotIn("__PUTGD_", text)
        self.write(self.script, text)

    def systemd_run(self, body):
        """A stand-in systemd-run: records its options, then runs `body`."""
        self.write(self.bin / "systemd-run", """#!/bin/sh
for argument do
    shift
    [ "$argument" = -- ] && break
    printf '%s\\n' "$argument" >> "$PG_CALLS"
done
""" + body)

    def run_launcher(self, *args, env=None, path_first=True):
        environment = dict(os.environ, PG_RECEIPT=str(self.receipt), PG_CALLS=str(self.calls))
        if path_first:
            environment["PATH"] = str(self.bin) + os.pathsep + os.environ["PATH"]
        environment.update(env or {})
        completed = subprocess.run(["/bin/sh", str(self.script), *args], env=environment, cwd=self.root,
                                   capture_output=True, text=True, timeout=60)
        started = self.receipt.read_bytes().split(b"\0")[:-1] if self.receipt.exists() else []
        return completed, [part.decode() for part in started]

    def calls_made(self):
        return self.calls.read_text().splitlines() if self.calls.exists() else []

    def test_a_scope_leaf_starts_the_image_directly(self):
        # user@1000.service is in every user process's path; only the leaf counts.
        self.systemd_run('exec "$@"\n')
        self.render()
        completed, started = self.run_launcher("--upload", "--", "/a b/${USER}.txt", "-leading")
        self.assertEqual((completed.returncode, completed.stderr), (0, ""))
        self.assertEqual(started, ["--upload", "--", "/a b/${USER}.txt", "-leading"])
        self.assertEqual(self.calls_made(), [])

    def test_a_service_leaf_reexecs_in_its_own_scope(self):
        self.systemd_run('exec "$@"\n')
        self.cgroup.write_text(SERVICE)
        self.render()
        completed, started = self.run_launcher("--upload", "--", "/a b/${USER}.txt", env={"PG_STATUS": "7"})
        self.assertEqual((completed.returncode, completed.stderr), (7, ""))
        self.assertEqual(started, ["--upload", "--", "/a b/${USER}.txt"])
        self.assertEqual(self.calls_made(), ["--user", "--scope", "--quiet", "--expand-environment=no"])

    def test_a_cgroup_v1_service_leaf_counts_too(self):
        self.systemd_run('exec "$@"\n')
        self.cgroup.write_text("1:name=systemd:/user.slice/user-1000.slice/user@1000.service/nautilus.service\n")
        self.render()
        completed, started = self.run_launcher("--upload", "--", "/a")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(started, ["--upload", "--", "/a"])
        self.assertEqual(len(self.calls_made()), 4)

    def test_without_a_scope_the_upload_runs_here_once(self):
        self.cgroup.write_text(SERVICE)
        for name, body in (("systemd-run fails", "exit 1\n"),
                           ("the scoped copy cannot signal", 'exec 3>&-\nexec "$@"\n')):
            with self.subTest(name):
                self.receipt.unlink(missing_ok=True)
                self.calls.unlink(missing_ok=True)
                self.systemd_run(body)
                self.render()
                completed, started = self.run_launcher("--upload", "--", "/a")
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertEqual(started, ["--upload", "--", "/a"])
                self.assertTrue(self.calls_made())

    def test_no_systemd_run_runs_the_upload_here(self):
        self.cgroup.write_text(SERVICE)
        self.render()
        environment = {"PATH": "/usr/bin:/bin"} if not shutil.which("systemd-run", path="/usr/bin:/bin") else None
        if environment is None:
            # Hide the host's systemd-run behind a directory without it.
            fake = self.root / "nosystemd"
            fake.mkdir()
            for tool in ("sed", "grep", "sort", "head", "tail", "od", "tr", "stat", "id", "getent", "env", "sleep", "rm", "cat"):
                found = shutil.which(tool)
                if found:
                    os.symlink(found, fake / tool)
            environment = {"PATH": str(fake)}
        completed, started = self.run_launcher("--upload", "--", "/a", env=environment, path_first=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(started, ["--upload", "--", "/a"])

    def test_a_missing_image_is_reported_and_its_files_removed(self):
        self.render(configured=self.images / "gone.AppImage")
        record = self.own / "appimage.owned"
        menu = self.root / "menu.desktop"
        menu.write_text("[Desktop Entry]\nX-putgd-Managed=true\n")
        foreign = self.root / "foreign.desktop"
        foreign.write_text("[Desktop Entry]\n")
        record.write_text("# X-putgd-Managed=true\n# X-putgd-IntegrationId=" + "b" * 64 + "\nfile " + str(menu) +
                          "\nfile " + str(foreign) + "\nfile " + str(self.script) + "\n")
        self.write(self.bin / "systemctl", "#!/bin/sh\nprintf 'not-found\\n'\n")
        completed, started = self.run_launcher("--upload", "--", "/a")
        self.assertEqual((completed.returncode, started), (1, []))
        self.assertIn("is missing", completed.stderr)
        self.assertFalse(menu.exists())
        self.assertTrue(foreign.exists())
        self.assertFalse(record.exists())
        self.assertFalse(self.script.exists())

    @unittest.skipUnless(shutil.which("cc") and shutil.which("sort"), "needs a C compiler for the fixture image")
    def test_an_update_beside_the_image_is_started(self):
        build = self.root / "fixture-build"
        build.mkdir()
        (build / "fixture.c").write_text(FIXTURE)
        subprocess.run(["cc", "-O1", "-o", str(build / "fixture"), str(build / "fixture.c")], check=True)
        data = bytearray((build / "fixture").read_bytes())
        data[8:11] = b"AI\x02"
        newer = self.images / "putgd-26.1101.1-x86_64.AppImage"
        newer.write_bytes(bytes(data))
        newer.chmod(0o700)
        Path(str(newer) + ".info").write_text(NIGHTLY + "\n")
        Path(str(newer) + ".version").write_text("26.1101.1\n")
        self.render(configured=self.images / "putgd-26.1031.1-x86_64.AppImage.removed", info=NIGHTLY)
        completed, started = self.run_launcher("--upload", "--", "/a b", env={"PG_RECEIPT": str(self.root / "fixture-receipt")})
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual((self.root / "fixture-receipt").read_text(), str(newer) + " --upload -- /a b\n")

    def manager_socket(self, runtime):
        """A runtime directory where a user manager listens; nothing answers it."""
        (runtime / "systemd").mkdir(parents=True, exist_ok=True)
        with socket.socket(socket.AF_UNIX) as listener:
            listener.bind(str(runtime / "systemd/private"))

    def stand_in_manager(self, run_status=0, run_message=""):
        """systemctl and systemd-run that record the XDG_RUNTIME_DIR they were given."""
        self.write(self.bin / "systemctl", """#!/bin/sh
printf 'systemctl %s\\n' "$XDG_RUNTIME_DIR" >> "$PG_CALLS"
case "$*" in
 *--property=LoadState*) echo not-found ;;
esac
""")
        self.write(self.bin / "systemd-run", f"""#!/bin/sh
printf 'systemd-run %s\\n' "$XDG_RUNTIME_DIR" >> "$PG_CALLS"
[ -z '{run_message}' ] || printf '%s\\n' '{run_message}' >&2
exit {run_status}
""")

    def test_a_private_runtime_still_reaches_the_login_user_manager(self):
        # systemd 259 finds the manager only through XDG_RUNTIME_DIR (#1112).
        self.manager_socket(self.user_runtime)
        self.stand_in_manager()
        self.render()
        private = self.root / "private-runtime"
        private.mkdir()
        completed, _ = self.run_launcher("--install-watch", env={"XDG_RUNTIME_DIR": str(private)})
        self.assertEqual(completed.returncode, 0, completed.stderr)
        calls = self.calls_made()
        self.assertIn("systemd-run " + str(self.user_runtime), calls)
        self.assertIn("systemctl " + str(self.user_runtime), calls)
        self.assertFalse([call for call in calls if str(private) in call], calls)

    def test_the_session_runtime_comes_first(self):
        session = self.root / "session-runtime"
        self.manager_socket(session)
        self.manager_socket(self.user_runtime)
        self.stand_in_manager()
        self.render()
        completed, _ = self.run_launcher("--install-watch", env={"XDG_RUNTIME_DIR": str(session)})
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual({call.split(" ", 1)[1] for call in self.calls_made()}, {str(session)})

    def test_no_user_manager_is_reported_apart_and_nothing_runs(self):
        self.stand_in_manager()
        self.render()
        private = self.root / "private-runtime"
        private.mkdir()
        completed, _ = self.run_launcher("--install-watch", env={"XDG_RUNTIME_DIR": str(private)})
        self.assertEqual(completed.returncode, 3)
        self.assertIn("no user service manager", completed.stderr)
        self.assertEqual(self.calls_made(), [])
        completed, _ = self.run_launcher("--stop-watch", env={"XDG_RUNTIME_DIR": str(private)})
        self.assertEqual((completed.returncode, self.calls_made()), (0, []))

    def test_a_watch_failure_says_why(self):
        self.manager_socket(self.user_runtime)
        self.stand_in_manager(run_status=1, run_message="Unit already exists")
        self.render()
        completed, _ = self.run_launcher("--install-watch")
        self.assertEqual(completed.returncode, 1)
        self.assertIn("systemd-run: Unit already exists", completed.stderr)

    @unittest.skipUnless(shutil.which("systemd-run") and subprocess.run(
        ["systemd-run", "--user", "--scope", "--quiet", "--expand-environment=no", "true"],
        capture_output=True).returncode == 0, "needs a user manager with systemd-run 254+")
    def test_the_real_user_manager_keeps_dollar_names_and_leaves_the_service(self):
        # The image reports the cgroup it runs in; the launcher sees a service leaf.
        self.cgroup.write_text(SERVICE)
        self.write(self.image, """#!/bin/sh
printf '%s\\0' "$@" >> "$PG_RECEIPT"
sed -n 's/^0:://p' /proc/self/cgroup > "$PG_RECEIPT.cgroup"
""")
        self.render()
        completed, started = self.run_launcher("--upload", "--", "/x/${USER}.txt", path_first=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(started, ["--upload", "--", "/x/${USER}.txt"])
        self.assertTrue(Path(str(self.receipt) + ".cgroup").read_text().strip().endswith(".scope"))


class AppRunUploadTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="pgfmrun-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.app = self.root / "mount with spaces"
        (self.app / "usr/bin").mkdir(parents=True)
        (self.root / "bin").mkdir()
        self.data = self.root / "data"
        self.receipt = self.root / "receipt"
        for name in ("putgd-ui", "putgd-record"):
            (self.app / "usr/bin" / name).write_text("#!/bin/sh\nexit 0\n")
            (self.app / "usr/bin" / name).chmod(0o755)
        launcher = self.app / "usr/bin/putgd-launcher"
        launcher.write_text("""#!/bin/sh
printf '%s\\n' "$$" "$PUTGD_HOST_ENVIRONMENT" "${PUTGD_HOST_LD_LIBRARY_PATH-unset}" > "$PG_TEST_RECEIPT"
printf '%s\\n' "$@" >> "$PG_TEST_RECEIPT"
""")
        launcher.chmod(0o755)
        sycoca = self.root / "bin/kbuildsycoca6"
        sycoca.write_text("#!/bin/sh\n: > \"$PG_TEST_RECEIPT.sycoca\"\n")
        sycoca.chmod(0o755)
        self.env = dict(os.environ, APPDIR=str(self.app), APPIMAGE=str(self.root / "putgd.AppImage"),
                        XDG_DATA_HOME=str(self.data), XDG_DATA_DIRS=str(self.root / "host-data"),
                        XDG_CURRENT_DESKTOP="KDE", DISPLAY=":9", LD_LIBRARY_PATH="/host/lib",
                        PATH=str(self.root / "bin") + ":" + os.environ["PATH"], PG_TEST_RECEIPT=str(self.receipt))

    def test_upload_execs_the_launcher_without_entries_or_supervisor(self):
        process = subprocess.Popen([str(APP_RUN), "--upload", "--", "/a b/${USER}.txt", "-x"], env=self.env)
        self.assertEqual(process.wait(timeout=30), 0)
        lines = self.receipt.read_text().splitlines()
        # exec: the launcher is the AppRun process itself.
        self.assertEqual(lines[0], str(process.pid))
        self.assertEqual(lines[1:3], ["LD_LIBRARY_PATH QT_PLUGIN_PATH", "/host/lib"])
        self.assertEqual(lines[3:], ["--upload", "--", "/a b/${USER}.txt", "-x"])
        applications = self.data / "applications"
        self.assertEqual(sorted(applications.iterdir()) if applications.exists() else [], [])
        self.assertFalse(Path(str(self.receipt) + ".sycoca").exists())

    def test_other_launches_still_register(self):
        process = subprocess.Popen([str(APP_RUN), "--background"], env=self.env)
        self.assertEqual(process.wait(timeout=30), 0)
        self.assertTrue(Path(str(self.receipt) + ".sycoca").exists())
        self.assertNotEqual(self.receipt.read_text().splitlines()[0], str(process.pid))


if __name__ == "__main__":
    unittest.main()
