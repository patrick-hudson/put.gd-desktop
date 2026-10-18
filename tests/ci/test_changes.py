"""Path selection must never skip a job that a changed input can affect."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ci_changes', ROOT / 'tools/ci/changes.py')
changes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(changes)


class Selection(unittest.TestCase):
    def assertSelects(self, paths, build, packages):
        result = changes.select(paths)
        self.assertEqual((result['build'], result['packages']), (build, packages), paths)

    def test_documentation_runs_contracts_only(self):
        self.assertSelects(['docs/ci.md', 'README.md', 'AGENTS.md', 'CLAUDE.md',
                            'docs/evidence/live-s3-memory/source114/manifest.json'], False, False)

    def test_python_lab_and_release_tests_run_contracts_only(self):
        self.assertSelects(['tools/lab/lab.py', 'tests/desktop/test_lab.py', 'tests/desktop/kde.yaml',
                            'tests/releases/test_publish.py', 'tests/ci/test_changes.py',
                            'tests/performance/observe_cold_family.py', 'tools/mcp/share_mcp/server.py'],
                           False, False)

    def test_compiled_or_ctest_inputs_build(self):
        for path in ('src/core/Settings.cpp', 'tests/unit/test_settings.cpp', 'tests/desktop/x11/test_x11.cpp',
                     'tests/desktop/test_gnome_ownership.mjs', 'tests/performance/bench_capture.cpp',
                     'schemas/config.schema.json', 'tools/diagnostics/check_socket_paths.py',
                     'some/new/directory/file.txt'):
            with self.subTest(path=path):
                self.assertSelects([path], True, False)

    def test_packaging_inputs_build_and_package(self):
        for path in ('packaging/linux/CMakeLists.txt', 'tools/releases/collect.py', 'tools/proton/runtime-lock.json',
                     'cmake/Proton.cmake', 'CMakeLists.txt', 'CMakePresets.json', 'LICENSE',
                     'THIRD_PARTY_NOTICES.md', 'integrations/gnome/extension.js', '.github/workflows/ci.yml',
                     'tools/ci/sdk_cache.py', 'tools/verify_dependencies.py', 'docs/build.md',
                     'docs/proton-drive.md', '.gitignore'):
            with self.subTest(path=path):
                self.assertSelects([path], True, True)

    def test_mixed_change_takes_the_union(self):
        self.assertSelects(['docs/ci.md', 'src/main.cpp'], True, False)
        self.assertSelects(['docs/ci.md', 'packaging/appimage/AppRun'], True, True)
        self.assertSelects([], False, False)


class GitDiff(unittest.TestCase):
    def test_deleted_and_renamed_paths_are_both_reported(self):
        with tempfile.TemporaryDirectory(prefix='pgchanges-') as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.check_output(['git', '-C', str(root), *args], text=True,
                    env=dict(os.environ, GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
                             GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid'))
            git('init', '-q')
            (root / 'docs').mkdir()
            (root / 'docs/a.md').write_text('same content\n' * 20)
            git('add', '.')
            git('commit', '-qm', 'base')
            (root / 'src').mkdir()
            git('mv', 'docs/a.md', 'src/a.md')
            git('commit', '-qm', 'move')
            paths = changes.changed_paths(root, 'HEAD^1', 'HEAD')
            self.assertEqual(sorted(paths), ['docs/a.md', 'src/a.md'])
            self.assertTrue(changes.select(paths)['build'])


if __name__ == '__main__':
    unittest.main()
