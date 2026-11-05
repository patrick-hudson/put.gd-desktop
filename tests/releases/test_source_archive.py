"""Curated source export contracts, using synthetic temporary Git repositories."""
import ast
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import quiet_git  # noqa: F401  (disables git background maintenance, #99)
import source_archive


class SourceArchive(unittest.TestCase):
    def test_appimage_build_script_and_local_import_closure_are_exported(self):
        root = Path(__file__).resolve().parents[2]
        script = root / 'tools/releases/build-appimage.sh'
        pending = {'tools/releases/build-appimage.sh', 'tools/releases/provision-appimage.py'}
        pending.update(re.findall(r'tools/releases/[A-Za-z0-9_.-]+\.(?:py|sh)', script.read_text()))
        visited = set()
        while pending:
            name = pending.pop()
            if name in visited:
                continue
            visited.add(name)
            self.assertTrue(source_archive._included(name), name)
            path = root / name
            if path.suffix != '.py':
                continue
            for node in ast.walk(ast.parse(path.read_text())):
                modules = ([node.module] if isinstance(node, ast.ImportFrom) else
                           [alias.name for alias in node.names] if isinstance(node, ast.Import) else [])
                for module in modules:
                    dependency = path.parent / ((module or '').split('.')[0] + '.py')
                    if dependency.is_file():
                        pending.add(dependency.relative_to(root).as_posix())
        for path in (root / 'packaging/appimage').iterdir():
            if path.is_file():
                self.assertTrue(source_archive._included(path.relative_to(root).as_posix()), path.name)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pgsource-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'repo'
        self.root.mkdir()
        files = {
            'CMakeLists.txt': 'project(putgd VERSION 0.1.1 LANGUAGES C CXX)\n',
            'CMakePresets.json': '{}\n', 'LICENSE': 'synthetic license\n',
            'THIRD_PARTY_NOTICES.md': 'synthetic notices\n', '.gitignore': '/build/\n',
            'src/main.cpp': '// committed synthetic source\n',
            'cmake/BuildVersion.cmake': '# synthetic build input\n',
            'cmake/dependency-lock.json': '{}\n',
            'packaging/linux/CMakeLists.txt': '# synthetic package input\n',
            'integrations/gnome/extension.js': '// synthetic extension\n',
            'integrations/gnome/metadata.json': '{}\n',
            'docs/build.md': '# synthetic build guide\n',
            'tools/verify_dependencies.py': '# synthetic verifier\n',
            'tools/releases/provision-sdk.sh': '#!/bin/sh\nexit 0\n',
            'tools/proton/runtime.ts': '// committed Proton helper\n',
            'tools/proton/runtime.test.ts': '// required provisioning checks\n',
            'tools/proton/runtime-lock.json': '{}\n',
            'tools/proton/package-lock.json': '{}\n',
            'tools/proton/provision_runtime.py': '# pinned provisioner\n',
            'tools/proton/notices/SOURCES.json': '{}\n',
            'tools/proton/notices/REBUILD.md': '# rebuild instructions\n',
            'docs/proton-drive.md': '# Proton setup\n',
            'tools/proton/credentials.txt': 'synthetic excluded credential input\n',
            'tools/proton/work/session.json': '{}\n',
            'work/tracked.txt': 'synthetic excluded work\n',
            'docs/evidence/receipt.json': '{}\n', 'docs/plans/plan.md': 'excluded plan\n',
            'tests/fixtures/payload.txt': 'excluded synthetic payload\n',
            '.agents/context.md': 'excluded context\n', '.github/workflows/ci.yml': '{}\n',
            'AGENTS.md': 'excluded agent instructions\n',
            'credentials.txt': 'synthetic credential fixture only\n',
            'src/credentials.txt': 'synthetic credential fixture only\n',
            'src/.env': 'SYNTHETIC_ONLY=fixture\n',
            'src/context/private.md': 'excluded context\n',
            'src/.context/session.md': 'synthetic excluded agent context\n',
            'src/.claude/commands.md': 'synthetic excluded agent commands\n',
            'src/CLAUDE.md': 'synthetic excluded agent notes\n',
            'src/MEMORY.md': 'synthetic excluded agent memory\n',
        }
        for name, content in files.items():
            p = self.root / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
        (self.root / 'tools/releases/provision-sdk.sh').chmod(0o755)
        self.git('init', '-q')
        self.commit()
        self.identity = self.current_identity()
        self.output = Path(self.temp.name) / 'putgd-0.1.1-source.tar.gz'

    def git(self, *args, input=None):
        return subprocess.check_output(['git', '-C', str(self.root), *args], input=input).decode().strip()

    def commit(self):
        self.git('add', '.')
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'synthetic fixture')

    def current_identity(self):
        commit = self.git('rev-parse', 'HEAD')
        count = int(self.git('rev-list', '--count', commit))
        return {'version': '0.1.1', 'baseVersion': '0.1.1', 'packageVersion': '0.1.1',
                'gitCommit': commit, 'gitTree': self.git('rev-parse', 'HEAD^{tree}'),
                'commitCount': count, 'dirty': False, 'sourceProvenance': 'git'}

    def archive(self):
        return source_archive.create_archive(self.root, self.output, self.identity)

    def contents(self):
        with tarfile.open(self.output, 'r:gz') as archive:
            prefix = archive.getnames()[0].split('/')[0] + '/'
            return {m.name[len(prefix):]: archive.extractfile(m).read()
                    for m in archive if m.isfile()}

    def test_exports_core_inputs_and_generated_provenance_only(self):
        (self.root / 'work/untracked.txt').write_text('excluded untracked work\n')
        (self.root / 'src/untracked.cpp').write_text('// excluded untracked source\n')
        receipt = self.archive()
        contents = self.contents()
        for name in ['CMakeLists.txt', 'CMakePresets.json', 'LICENSE', 'THIRD_PARTY_NOTICES.md',
                     'src/main.cpp', 'cmake/dependency-lock.json', 'packaging/linux/CMakeLists.txt',
                     'integrations/gnome/extension.js', 'docs/build.md', 'tools/verify_dependencies.py',
                     'tools/releases/provision-sdk.sh', 'tools/proton/runtime.ts',
                     'tools/proton/runtime.test.ts', 'tools/proton/runtime-lock.json',
                     'tools/proton/package-lock.json', 'tools/proton/provision_runtime.py',
                     'tools/proton/notices/SOURCES.json', 'tools/proton/notices/REBUILD.md',
                     'docs/proton-drive.md', 'README_SOURCE.md', 'source-info.json']:
            self.assertIn(name, contents)
        for name in ['credentials.txt', 'src/credentials.txt', 'src/.env', 'src/context/private.md', 'src/.context/session.md',
                     'src/.claude/commands.md', 'src/CLAUDE.md', 'src/MEMORY.md',
                     'work/tracked.txt', 'work/untracked.txt', 'src/untracked.cpp', 'AGENTS.md',
                     '.agents/context.md', '.github/workflows/ci.yml', 'docs/evidence/receipt.json',
                     'docs/plans/plan.md', 'tests/fixtures/payload.txt',
                     'tools/proton/credentials.txt', 'tools/proton/work/session.json']:
            self.assertNotIn(name, contents)
        info = json.loads(contents['source-info.json'])
        self.assertEqual(info['identity'], self.identity)
        self.assertEqual(info['localBuild']['sourceProvenance'], 'supplied')
        self.assertFalse(info['localBuild']['releaseTagBuild'])
        self.assertIn(b'-DBUILD_TESTING=OFF', contents['README_SOURCE.md'])
        for flag in [b'PUTGD_SOURCE_COMMIT', b'PUTGD_SOURCE_TREE', b'PUTGD_SOURCE_COMMIT_COUNT',
                     b'-DPUTGD_LAB_FIXTURE=OFF', b'-DPUTGD_PHYSICAL_BENCHMARKS=OFF']:
            self.assertIn(flag, contents['README_SOURCE.md'])
        for text in [b'python3 tools/proton/provision_runtime.py', b'-DPUTGD_BUNDLE_PROTON=ON',
                     b'Linux x86-64', b'notices/REBUILD.md', b'Git/npm']:
            self.assertIn(text, contents['README_SOURCE.md'])
        self.assertEqual(receipt['identity'], self.identity)
        self.assertEqual(receipt['artifact']['sha256'], hashlib.sha256(self.output.read_bytes()).hexdigest())
        for member in receipt['members']:
            self.assertEqual(member['sha256'], hashlib.sha256(contents[member['path']]).hexdigest())

    def test_uses_requested_committed_bytes_despite_dirty_or_new_head(self):
        (self.root / 'src/main.cpp').write_text('// a later committed source\n')
        self.commit()
        (self.root / 'src/main.cpp').write_text('// dirty source must not be exported\n')
        self.archive()
        self.assertEqual(self.contents()['src/main.cpp'], b'// committed synthetic source\n')

    def test_deterministic_tar_gzip_metadata_and_executable_mode(self):
        first = self.archive()
        other = Path(self.temp.name) / 'repeat.tar.gz'
        second = source_archive.create_archive(self.root, other, dict(reversed(list(self.identity.items()))))
        self.assertEqual(self.output.read_bytes(), other.read_bytes())
        self.assertEqual(first['artifact']['sha256'], second['artifact']['sha256'])
        with tarfile.open(self.output, 'r:gz') as archive:
            for member in archive:
                self.assertEqual((member.uid, member.gid, member.uname, member.gname), (0, 0, 'root', 'root'))
                self.assertEqual(member.mtime, first['timestamp'])
                self.assertEqual(member.mode, 0o755 if member.isdir() or member.name.endswith('provision-sdk.sh') else 0o644)
        self.assertEqual(int.from_bytes(self.output.read_bytes()[4:8], 'little'), first['timestamp'])

    def test_rejects_included_symlink_before_writing_archive(self):
        (self.root / 'src/link.cpp').symlink_to('main.cpp')
        self.commit()
        self.identity = self.current_identity()
        with self.assertRaisesRegex(ValueError, 'unsupported'):
            self.archive()
        self.assertFalse(self.output.exists())

    def test_rejects_included_submodule(self):
        commit = self.identity['gitCommit']
        self.git('update-index', '--add', '--cacheinfo', '160000,' + commit + ',src/nested-module')
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'synthetic gitlink')
        self.identity = self.current_identity()
        with self.assertRaisesRegex(ValueError, 'unsupported'):
            self.archive()
        self.assertFalse(self.output.exists())

    def test_rejects_unsafe_included_path(self):
        (self.root / 'src/bad\\name.cpp').write_text('// unsafe archive path\n')
        self.commit()
        self.identity = self.current_identity()
        with self.assertRaisesRegex(ValueError, 'unsafe'):
            self.archive()
        self.assertFalse(self.output.exists())

    def test_rejects_identity_mismatch_and_existing_output(self):
        self.identity['gitTree'] = '0' * 40
        with self.assertRaisesRegex(ValueError, 'tree'):
            self.archive()
        self.assertFalse(self.output.exists())
        self.identity = self.current_identity()
        self.archive()
        with self.assertRaisesRegex(ValueError, 'exists'):
            self.archive()

    def test_cli_creates_new_output_and_receipt(self):
        identity_path = Path(self.temp.name) / 'identity.json'
        identity_path.write_text(json.dumps(self.identity))
        output_dir = Path(self.temp.name) / 'cli-output'
        subprocess.run([sys.executable, str(Path(source_archive.__file__)), '--root', str(self.root),
                        '--output-dir', str(output_dir), '--identity', str(identity_path)], check=True,
                       capture_output=True)
        self.assertTrue((output_dir / 'putgd-0.1.1-source.tar.gz').is_file())
        receipt = json.loads((output_dir / 'source-archive.json').read_text())
        self.assertEqual(receipt['identity'], self.identity)


if __name__ == '__main__':
    unittest.main()
