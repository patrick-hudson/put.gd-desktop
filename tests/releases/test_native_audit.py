"""Extracted native-package checks; actual ELF/desktop validation is separate."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools/releases"))
import audit_native as audit
import native

class NativeInventoryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="pgpkg-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.manifest = {"version": "0.1.2-rc.1", "gitCommit": "a" * 40, "dirty": False}
        self.write("usr/share/putgd/build-info.json", json.dumps(self.manifest))
        self.write("usr/share/putgd/dependency-lock.json", "{}")
        self.write("usr/share/doc/putgd/LICENSE", "MIT License\nCopyright (c) 2026 Patrick Hudson")
        for license_name in audit.SDK_LICENSES:
            self.write("usr/share/doc/putgd/" + license_name, "license")
        self.write("usr/share/doc/putgd/runtime-notices.txt", "notice")
        self.write("usr/share/doc/putgd/wayland-ext-data-control-v1.xml", "protocol")
        self.write("usr/share/doc/putgd/wayland-wlr-data-control-unstable-v1.xml", "protocol")
        for asset in audit.DESKTOP_ASSETS:
            self.write("usr/" + asset, "asset")
        for name in audit.PROGRAMS:
            self.write("usr/bin/" + name, "binary", 0o755)
        self.write("usr/lib64/putgd/libaws-c-common.so", "SDK")
        self.write("usr/lib64/putgd/libputgd-s3.so", "module")
        runtime = {"libraries": [{"file": "libaws-c-common.so", "sha256": audit.sha256(self.root / "usr/lib64/putgd/libaws-c-common.so")}],
                   "module": {"file": "libputgd-s3.so", "contractVersion": 1, "buildIdentity": "b" * 64,
                              "sha256": audit.sha256(self.root / "usr/lib64/putgd/libputgd-s3.so")}}
        self.write("usr/share/putgd/aws-runtime-manifest.json", json.dumps(runtime))

    def write(self, name, data, mode=0o644):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data)
        path.chmod(mode)
        return path

    def test_valid_native_inventory(self):
        result = audit.validate_inventory(self.root, self.manifest)
        self.assertEqual(result["manifest"], self.manifest)
        self.assertEqual(result["privateLibraryDirectory"], "usr/lib64/putgd")

    def test_release_excludes_credentials_and_fixtures(self):
        for name in ("usr/share/putgd/credentials.txt", "usr/bin/putgd-desktop-fixture", "usr/bin/putgd-view-bench"):
            with self.subTest(name=name):
                path = self.write(name, "synthetic", 0o755)
                with self.assertRaisesRegex(ValueError, "Forbidden release file"):
                    audit.validate_inventory(self.root, self.manifest)
                path.unlink()

    def test_runtime_digest_detects_package_postprocessing(self):
        self.write("usr/lib64/putgd/libputgd-s3.so", "stripped module")
        with self.assertRaisesRegex(ValueError, "Runtime digest mismatch"):
            audit.validate_inventory(self.root, self.manifest)

    def test_helpers_must_be_executable(self):
        (self.root / "usr/bin/putgd-s3-worker").chmod(0o644)
        with self.assertRaisesRegex(ValueError, "Executable mode"):
            audit.validate_inventory(self.root, self.manifest)

    def test_required_license_missing(self):
        (self.root / "usr/share/doc/putgd/LICENSE").unlink()
        with self.assertRaisesRegex(ValueError, "Required release file"):
            audit.validate_inventory(self.root, self.manifest)

    def test_distro_qt_cannot_be_bundled(self):
        self.write("usr/lib64/putgd/libQt6Core.so.6", "Qt")
        with self.assertRaisesRegex(ValueError, "Bundled Qt"):
            audit.validate_inventory(self.root, self.manifest)

    def test_expected_manifest_must_match_extracted_package(self):
        with self.assertRaisesRegex(ValueError, "Build manifest mismatch"):
            audit.validate_inventory(self.root, {**self.manifest, "gitCommit": "c" * 40})

    def test_kde_helper_required_when_build_declares_it(self):
        with self.assertRaisesRegex(ValueError, "Required release file"):
            audit.validate_inventory(self.root, self.manifest, require_kde=True)

    def commands(self, argv, *, env=None):
        if argv[:2] == ["readelf", "--dyn-syms"]:
            return ("     5: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND pthread_create@GLIBC_2.34 (2)\n"
                    "    42: 00000000000109ee   180 FUNC    GLOBAL DEFAULT   16 pthread_create")
        if argv[0] == "readelf":
            return " 0x000000000000000f (RPATH) Library rpath: [$ORIGIN]"
        if argv[0] == "ldd":
            return "libc.so.6 => /lib/libc.so.6 (0x1234)"
        if argv[1] == "--version-json":
            return json.dumps(self.manifest)
        if argv[1] == "--update-info":
            return json.dumps({"packageTarget": "debian13", "supported": False, "reason": "development_build"})
        return Path(argv[0]).name + " " + self.manifest["version"]

    def test_full_audit_compares_every_compiled_identity(self):
        with patch.object(audit, "command", side_effect=self.commands):
            result = audit.audit_tree(self.root, self.manifest)
        self.assertEqual(set(result["compiledIdentities"]), set(audit.PROGRAMS) - {"putgd-launcher"})
        self.assertEqual(result["updateCheck"]["packageTarget"], "debian13")

    def test_package_target_must_match_the_package_job(self):
        # #293: a package built without its target would never check for updates.
        with patch.object(audit, "command", side_effect=self.commands):
            audit.audit_tree(self.root, self.manifest, package_target="debian13")
            with self.assertRaisesRegex(ValueError, "Update check package target mismatch"):
                audit.audit_tree(self.root, self.manifest, package_target="fedora44")

    def test_test_only_crash_trigger_rejected(self):
        # Development builds read PUTGD_TEST_CRASH (#124); release builds never contain it.
        self.write("usr/bin/putgd-ui", "binary PUTGD_TEST_CRASH", 0o755)
        with patch.object(audit, "command", side_effect=self.commands):
            with self.assertRaisesRegex(ValueError, "test-only crash trigger: putgd-ui"):
                audit.audit_tree(self.root, self.manifest)

    def test_crash_handled_programs_export_their_thread_wrapper(self):
        # Without it, threads have no alternate signal stack (#156). The CLI has no crash handler.
        def commands(argv, *, env=None):
            if argv[:2] == ["readelf", "--dyn-syms"]:
                if Path(argv[-1]).name == "putgd-cli":
                    raise AssertionError("putgd-cli has no crash handler")
                if Path(argv[-1]).name == "putgd-capture":
                    return "     5: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND pthread_create@GLIBC_2.34 (2)"
            return self.commands(argv, env=env)
        with patch.object(audit, "command", side_effect=commands):
            with self.assertRaisesRegex(ValueError, "export the crash handler's pthread_create: putgd-capture"):
                audit.audit_tree(self.root, self.manifest)

    def test_mixed_helper_manifest_rejected(self):
        def commands(argv, *, env=None):
            if Path(argv[0]).name == "putgd-ui" and argv[1] == "--version-json":
                return json.dumps({**self.manifest, "gitCommit": "c" * 40})
            return self.commands(argv, env=env)
        with patch.object(audit, "command", side_effect=commands):
            with self.assertRaisesRegex(ValueError, "Compiled build manifest mismatch"):
                audit.audit_tree(self.root, self.manifest)

    def test_sdk_cannot_resolve_from_host(self):
        def commands(argv, *, env=None):
            if argv[0] == "ldd":
                return "libaws-c-common.so => /usr/lib64/libaws-c-common.so (0x1234)"
            return self.commands(argv, env=env)
        with patch.object(audit, "command", side_effect=commands):
            with self.assertRaisesRegex(ValueError, "Private SDK dependency resolved outside package"):
                audit.audit_tree(self.root, self.manifest)

    def test_transitive_module_rpath_required(self):
        def commands(argv, *, env=None):
            if argv[:2] == ["readelf", "-d"]:
                return " 0x000000000000001d (RUNPATH) Library runpath: [$ORIGIN]"
            return self.commands(argv, env=env)
        with patch.object(audit, "command", side_effect=commands):
            with self.assertRaisesRegex(ValueError, r"transitive \$ORIGIN RPATH"):
                audit.audit_tree(self.root, self.manifest)

class RuntimePackageTests(unittest.TestCase):
    """Qt image plugins the distros package apart from Qt GUI (#189)."""
    def test_deb_and_rpm_require_the_webp_plugin_package(self):
        native.require_runtime_packages({"format": "DEB", "requires": "libqt6sql6-sqlite, qt6-qpa-plugins, qt6-wayland, "
                                         "qt6-image-formats-plugins, libc6 (>= 2.38) | libc6-alt"})
        native.require_runtime_packages({"format": "RPM", "requires": ["qt6-qtbase-gui", "qt6-qtimageformats", "libc.so.6()(64bit)"]})

    def test_missing_plugin_package_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "DEB does not require qt6-image-formats-plugins"):
            native.require_runtime_packages({"format": "DEB", "requires": "qt6-qpa-plugins, qt6-image-formats-plugins-extra"})
        with self.assertRaisesRegex(ValueError, "RPM does not require qt6-qtimageformats"):
            native.require_runtime_packages({"format": "RPM", "requires": ["qt6-qtbase-gui"]})

    def test_package_configuration_declares_them(self):
        text = (REPO / "packaging/linux/CMakeLists.txt").read_text()
        for names in native.RUNTIME_PACKAGES.values():
            for name in names:
                self.assertRegex(text, r"CPACK_(DEBIAN_PACKAGE_DEPENDS|RPM_PACKAGE_REQUIRES) \"[^\"]*\b" + name + r"\b")


if __name__ == "__main__":
    unittest.main()
