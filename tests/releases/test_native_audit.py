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
        if argv[0] == "readelf":
            return " 0x000000000000000f (RPATH) Library rpath: [$ORIGIN]"
        if argv[0] == "ldd":
            return "libc.so.6 => /lib/libc.so.6 (0x1234)"
        if argv[1] == "--version-json":
            return json.dumps(self.manifest)
        return Path(argv[0]).name + " " + self.manifest["version"]

    def test_full_audit_compares_every_compiled_identity(self):
        with patch.object(audit, "command", side_effect=self.commands):
            result = audit.audit_tree(self.root, self.manifest)
        self.assertEqual(set(result["compiledIdentities"]), set(audit.PROGRAMS) - {"putgd-launcher"})

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
            if argv[0] == "readelf":
                return " 0x000000000000001d (RUNPATH) Library runpath: [$ORIGIN]"
            return self.commands(argv, env=env)
        with patch.object(audit, "command", side_effect=commands):
            with self.assertRaisesRegex(ValueError, r"transitive \$ORIGIN RPATH"):
                audit.audit_tree(self.root, self.manifest)

if __name__ == "__main__":
    unittest.main()
