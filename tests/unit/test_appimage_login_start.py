#!/usr/bin/env python3
"""The AppImage login script picks the image login starts (#307).

Fixture "AppImages" are a small compiled program with the AppImage type 2 magic
at offset 8. Each answers --appimage-updateinformation and --version from files
beside it and records a real start; no image is mounted.
"""
import grp
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "src/app/appimage-login-start.sh"
NIGHTLY = "zsync|https://app.put.gd/dl/nightly/appimage.zsync"
STABLE = "zsync|https://app.put.gd/dl/stable/appimage.zsync"
FIXTURE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void say(const char* self, const char* suffix, const char* prefix) {
    char path[4096], text[1024] = "";
    snprintf(path, sizeof path, "%s.%s", self, suffix);
    FILE* file = fopen(path, "r");
    if (!file) return;
    if (fgets(text, sizeof text, file)) printf("%s%s", prefix, text);
    fclose(file);
}
static void note(const char* variable, int argc, char** argv) {
    const char* path = getenv(variable);
    FILE* file = path ? fopen(path, "a") : NULL;
    if (!file) return;
    for (int i = 0; i < argc; ++i) fprintf(file, i ? " %s" : "%s", argv[i]);
    fputc('\n', file);
    fclose(file);
}
int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--appimage-updateinformation") == 0) {
        note("PG_PROBES", argc, argv);
        say(argv[0], "info", "");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        note("PG_PROBES", argc, argv);
        say(argv[0], "version", "putgd ");
        return 0;
    }
    note("PG_RECEIPT", argc, argv);
    return 0;
}
"""


def private_group():
    """Whether the primary group is the user's own: same name, no other members."""
    user = pwd.getpwuid(os.getuid()).pw_name
    group = grp.getgrgid(os.getgid())
    return group.gr_name == user and set(group.gr_mem) <= {user}


def shell_quoted(value):
    return "'" + value.replace("'", "'\\''") + "'"


@unittest.skipUnless(shutil.which("cc") and shutil.which("sort"), "needs a C compiler for the fixture image")
class LoginStartTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="pglogin-build-")
        source = Path(cls.build.name) / "fixture.c"
        source.write_text(FIXTURE)
        cls.binary = Path(cls.build.name) / "fixture"
        subprocess.run(["cc", "-O1", "-o", str(cls.binary), str(source)], check=True)
        data = bytearray(cls.binary.read_bytes())
        data[8:11] = b"AI\x02"
        cls.binary.write_bytes(bytes(data))

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="pglogin-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.images = self.root / "Applications with 'quotes'"
        self.images.mkdir(mode=0o700)
        self.receipt = self.root / "receipt"
        self.probes = self.root / "probes"

    def image(self, name, version, info=NIGHTLY, magic=True, mode=0o755):
        path = self.images / name
        data = bytearray(self.binary.read_bytes())
        if not magic:
            data[8:11] = b"\0\0\0"
        path.write_bytes(bytes(data))
        path.chmod(mode)
        Path(str(path) + ".info").write_text(info + "\n")
        Path(str(path) + ".version").write_text(version + "\n")
        return path

    def start(self, configured, info=NIGHTLY):
        script = self.root / "appimage-start.sh"
        text = SCRIPT.read_text()
        text = text.replace("__PUTGD_CONFIGURED__", shell_quoted(str(configured)))
        text = text.replace("__PUTGD_UPDATE_INFORMATION__", shell_quoted(info))
        script.write_text(text)
        env = dict(os.environ, PG_RECEIPT=str(self.receipt), PG_PROBES=str(self.probes))
        completed = subprocess.run(["/bin/sh", str(script), "--background"], env=env, cwd=self.root,
                                   capture_output=True, text=True, timeout=60)
        started = self.receipt.read_text().splitlines() if self.receipt.exists() else []
        return completed, started

    def assertStarts(self, configured, expected, info=NIGHTLY):
        completed, started = self.start(configured, info)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(started, [str(expected) + " --background"])
        return completed

    def test_gear_lever_replaced_in_place_starts_the_configured_image_without_probes(self):
        configured = self.image("put.gd.appimage", "0.1.1-nightly.801")
        self.assertStarts(configured, configured)
        self.assertFalse(self.probes.exists())

    def test_appimagelauncher_update_beside_the_old_image_starts_the_new_one(self):
        configured = self.image("putgd-0.1.1-nightly.800-x86_64_6b9be45c7ea9f6243711c461b0f77065.AppImage",
                                "0.1.1-nightly.800")
        self.image("putgd-0.1.1-nightly.799-x86_64.AppImage", "0.1.1-nightly.799")
        new = self.image("putgd-0.1.1-nightly.801-x86_64.AppImage", "0.1.1-nightly.801")
        completed = self.assertStarts(configured, new)
        self.assertIn("newer than", completed.stderr)

    def test_updater_that_removed_the_old_image_starts_the_new_one(self):
        configured = self.images / "putgd-0.1.1-nightly.800-x86_64.AppImage"
        new = self.image("putgd-0.1.1-nightly.801-x86_64.AppImage", "0.1.1-nightly.801")
        completed = self.assertStarts(configured, new)
        self.assertIn("is missing; starting", completed.stderr)

    def test_newer_image_of_another_channel_is_not_started(self):
        configured = self.image("putgd-0.1.1-nightly.800-x86_64.AppImage", "0.1.1-nightly.800")
        self.image("putgd-0.1.2-x86_64.AppImage", "0.1.2", info=STABLE)
        self.image("putgd-0.1.3-x86_64.AppImage", "0.1.3", info="")
        self.assertStarts(configured, configured)

    def test_files_that_are_not_private_appimages_are_not_started(self):
        configured = self.image("putgd-0.1.1-nightly.800-x86_64.AppImage", "0.1.1-nightly.800")
        self.image("putgd-0.1.1-nightly.901-x86_64.AppImage", "0.1.1-nightly.901", mode=0o757)
        self.image("putgd-0.1.1-nightly.902-x86_64.AppImage", "0.1.1-nightly.902", magic=False)
        self.image("putgd-0.1.1-nightly.903-x86_64.AppImage", "0.1.1-nightly.903", mode=0o644)
        self.image("putgd-latest-x86_64.AppImage", "0.1.1-nightly.904")
        self.assertStarts(configured, configured)

    def test_group_write_is_trusted_only_for_the_users_private_group(self):
        # umask 002 (Debian 13, Kubuntu 26.04, Mint 22.3 desktops) leaves files and folders 0775.
        configured = self.image("putgd-0.1.1-nightly.800-x86_64.AppImage", "0.1.1-nightly.800", mode=0o775)
        new = self.image("putgd-0.1.1-nightly.801-x86_64.AppImage", "0.1.1-nightly.801", mode=0o775)
        self.images.chmod(0o775)
        self.addCleanup(self.images.chmod, 0o700)
        self.assertStarts(configured, new if private_group() else configured)

    def test_configured_image_only_has_to_be_executable(self):
        # As before #307: the image put.gd last ran from starts even if others could write it.
        configured = self.image("putgd-0.1.1-nightly.800-x86_64.AppImage", "0.1.1-nightly.800", mode=0o757)
        self.assertStarts(configured, configured)
        configured.chmod(0o644)
        completed, started = self.start(configured)
        self.assertEqual(completed.returncode, 1)
        self.assertIn("missing or not executable", completed.stderr)

    def test_symbolic_link_to_a_newer_image_is_not_started(self):
        configured = self.image("putgd-0.1.1-nightly.800-x86_64.AppImage", "0.1.1-nightly.800")
        elsewhere = self.root / "elsewhere"
        elsewhere.mkdir(mode=0o700)
        target = self.image("target.AppImage", "0.1.1-nightly.900")
        moved = elsewhere / "putgd-0.1.1-nightly.900-x86_64.AppImage"
        target.rename(moved)
        Path(str(moved) + ".info").write_text(NIGHTLY + "\n")
        (self.images / moved.name).symlink_to(moved)
        self.assertStarts(configured, configured)

    def test_group_write_by_a_shared_group_is_not_trusted(self):
        shared = [gid for gid in os.getgroups() if gid != os.getgid()]
        if not shared:
            self.skipTest("the user has no supplementary group to share a file with")
        configured = self.image("putgd-0.1.1-nightly.800-x86_64.AppImage", "0.1.1-nightly.800")
        newer = self.image("putgd-0.1.1-nightly.801-x86_64.AppImage", "0.1.1-nightly.801", mode=0o775)
        os.chown(newer, -1, shared[0])
        newer.chmod(0o775)
        self.assertStarts(configured, configured)
        # The same file in a folder that group can write is not searched either.
        self.receipt.unlink()
        os.chown(newer, -1, os.getgid())
        os.chown(self.images, -1, shared[0])
        self.images.chmod(0o770)
        self.addCleanup(os.chown, self.images, -1, os.getgid())
        self.assertStarts(configured, configured)

    def test_folder_others_can_write_is_not_searched(self):
        configured = self.images / "putgd-0.1.1-nightly.800-x86_64.AppImage"
        self.image("putgd-0.1.1-nightly.801-x86_64.AppImage", "0.1.1-nightly.801")
        self.images.chmod(0o777)
        self.addCleanup(self.images.chmod, 0o700)
        completed, started = self.start(configured)
        self.assertEqual(completed.returncode, 0)
        self.assertEqual(started, [])
        self.assertEqual(completed.stderr, "")

    def test_release_orders_after_its_candidate(self):
        configured = self.image("putgd-0.1.2-rc.1-x86_64.AppImage", "0.1.2-rc.1", info=STABLE)
        release = self.image("putgd-0.1.2-x86_64.AppImage", "0.1.2", info=STABLE)
        self.assertStarts(configured, release, info=STABLE)
        (self.images / release.name).unlink()
        self.receipt.unlink()
        configured = self.image("putgd-0.1.2-x86_64.AppImage", "0.1.2", info=STABLE)
        self.image("putgd-0.1.2-rc.2-x86_64.AppImage", "0.1.2-rc.2", info=STABLE)
        self.image("putgd-0.1.10-x86_64.AppImage", "0.1.10", info=STABLE)
        self.assertStarts(configured, self.images / "putgd-0.1.10-x86_64.AppImage", info=STABLE)

    def test_calendar_versions_order_after_pre_calendar_ones_and_by_date(self):
        # #373: a zsync updater writes 26.1022.950-nightly beside 0.1.1-nightly.943.
        configured = self.image("putgd-0.1.1-nightly.943-x86_64.AppImage", "0.1.1-nightly.943")
        calendar = self.image("putgd-26.1022.950-nightly-x86_64.AppImage", "26.1022.950-nightly")
        self.assertStarts(configured, calendar)
        self.receipt.unlink()
        # October 22 (1022) is after January 5 (105) of the same year, and a later count wins on one day.
        self.image("putgd-26.105.990-nightly-x86_64.AppImage", "26.105.990-nightly")
        later = self.image("putgd-26.1022.951-nightly-x86_64.AppImage", "26.1022.951-nightly")
        self.assertStarts(configured, later)
        self.receipt.unlink()
        # A nightly orders before the stable release of the same YY.MDD.N.
        configured = self.image("putgd-26.1101.960-nightly-x86_64.AppImage", "26.1101.960-nightly", info=STABLE)
        release = self.image("putgd-26.1101.960-x86_64.AppImage", "26.1101.960", info=STABLE)
        self.assertStarts(configured, release, info=STABLE)

    def test_unversioned_configured_name_asks_the_image_for_its_version(self):
        configured = self.image("put.gd.appimage", "0.1.1-nightly.801")
        self.image("putgd-0.1.1-nightly.800-x86_64.AppImage", "0.1.1-nightly.800")
        self.assertStarts(configured, configured)
        self.assertIn(str(configured) + " --version", self.probes.read_text())
        newer = self.image("putgd-0.1.1-nightly.802-x86_64.AppImage", "0.1.1-nightly.802")
        self.receipt.unlink()
        self.assertStarts(configured, newer)

    def test_in_place_update_keeps_the_old_name_and_still_starts_the_configured_image(self):
        # put.gd's own update (#293 phase 3) writes 805 over the file named 800;
        # an AppImageLauncher copy of 802 beside it is older than what the file holds.
        configured = self.image("putgd-0.1.1-nightly.800-x86_64.AppImage", "0.1.1-nightly.805")
        self.image("putgd-0.1.1-nightly.802-x86_64_0123456789abcdef0123456789abcdef.AppImage", "0.1.1-nightly.802")
        self.assertStarts(configured, configured)
        self.assertIn(str(configured) + " --version", self.probes.read_text())
        # Its backup and staging folder are hidden and never candidates.
        self.image(".putgd-update-backup-0.1.1-nightly.800.AppImage", "0.1.1-nightly.900")
        (self.images / ".putgd-update-AbC123").mkdir()
        self.receipt.unlink()
        self.assertStarts(configured, configured)

    def test_same_version_prefers_the_configured_image(self):
        configured = self.image("putgd-0.1.1-nightly.801-x86_64_0123456789abcdef0123456789abcdef.AppImage",
                                "0.1.1-nightly.801")
        self.image("putgd-0.1.1-nightly.801-x86_64.AppImage", "0.1.1-nightly.801")
        self.assertStarts(configured, configured)

    def test_removed_images_leave_a_quiet_retained_preference(self):
        completed, started = self.start(self.images / "putgd-0.1.1-nightly.800-x86_64.AppImage")
        self.assertEqual(completed.returncode, 0)
        self.assertEqual(started, [])
        self.assertEqual(completed.stdout, "")
        self.assertEqual(completed.stderr, "")

    def test_removed_image_folder_leaves_a_quiet_retained_preference(self):
        configured = self.images / "putgd-26.1023.1058-nightly-x86_64.AppImage"
        self.images.rmdir()
        completed, started = self.start(configured)
        self.assertEqual(completed.returncode, 0)
        self.assertEqual(started, [])
        self.assertEqual(completed.stderr, "")

    def test_image_without_update_information_starts_only_itself(self):
        configured = self.images / "putgd-0.1.1-dev.795+g71f241b8f5d3-x86_64.AppImage"
        self.image("putgd-0.1.1-dev.796+g0000000-x86_64.AppImage", "0.1.1-dev.796+g0000000", info="")
        completed, started = self.start(configured, info="")
        self.assertEqual(completed.returncode, 0)
        self.assertEqual(started, [])
        self.assertEqual(completed.stderr, "")


if __name__ == "__main__":
    unittest.main()
