"""Exercise the dlopened backend with real ELF dependency resolution."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/releases"))
import portable_appimage as portable


@unittest.skipUnless(sys.platform == "linux" and shutil.which("cc"), "Linux ELF compiler required")
class SecretServiceDeployment(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="pgsecret-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.appdir = self.root / "AppDir"
        self.lib = self.appdir / "usr/lib"
        self.lib.mkdir(parents=True)
        self.compile("glib", "int baseline_api(void) { return 1; }", "libglib-2.0.so.0")

    def compile(self, name, code, soname, *libraries):
        source = self.root / (name + ".c")
        source.write_text(code)
        target = self.lib / soname
        subprocess.run(["cc", "-shared", "-fPIC", str(source), "-Wl,-soname," + soname,
                        "-Wl,--no-as-needed", "-L" + str(self.lib), *libraries, "-o", str(target)],
                       check=True, capture_output=True)
        return target

    def backend(self, symbol="baseline_api"):
        return self.compile("secret", f"extern int {symbol}(void);\n"
                            f"int secret_backend(void) {{ return {symbol}(); }}\n",
                            "libsecret-1.so.0", "-l:libglib-2.0.so.0")

    def test_matching_backend_and_glib_load_from_the_image(self):
        self.backend()
        result = portable.audit_secret_service(self.appdir)
        self.assertTrue(result["backendLoaded"])
        self.assertTrue(result["dependenciesResolvedInsideImage"])
        self.assertEqual(result["libraries"], ["usr/lib/libglib-2.0.so.0", "usr/lib/libsecret-1.so.0"])

    def test_host_backend_cannot_hide_an_omission(self):
        backend = self.backend()
        host = self.root / "host"
        host.mkdir()
        backend.rename(host / backend.name)
        with patch.dict(os.environ, LD_LIBRARY_PATH=str(host) + ":" + str(self.lib)):
            with self.assertRaisesRegex(ValueError, "must bundle libsecret"):
                portable.audit_secret_service(self.appdir)

    def test_newer_backend_with_older_glib_is_rejected(self):
        # The production Ubuntu failure: host libsecret asks for a symbol
        # introduced in GLib 2.84, absent from the bundled 2.80 baseline.
        self.backend("g_variant_builder_init_static")
        with self.assertRaises(subprocess.CalledProcessError) as error:
            portable.audit_secret_service(self.appdir)
        self.assertIn("g_variant_builder_init_static", error.exception.stderr)

    def test_backend_symlink_cannot_escape_the_image(self):
        backend = self.backend()
        outside = self.root / "libsecret-1.so.0"
        backend.rename(outside)
        backend.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "must bundle libsecret"):
            portable.audit_secret_service(self.appdir)


if __name__ == "__main__":
    unittest.main()
