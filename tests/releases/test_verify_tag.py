"""Cross-language release identity contracts using actual temporary Git sources."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools/releases'))
import verify_tag

BASE = '0.1.1'
UTC = '20261011153045'


class ReleaseIdentity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pgidentity-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.source = self.directory / 'source'
        self.source.mkdir()
        (self.source / 'CMakeLists.txt').write_text('project(putgd VERSION 0.1.1 LANGUAGES CXX)\n')
        self.git('init', '-q')
        self.git('config', 'user.name', 'Synthetic identity fixture')
        self.git('config', 'user.email', 'identity@example.invalid')
        self.git('add', '.')
        self.commit('First', '2026-10-10T21:00:45+0530')
        self.commit('Second', '2026-10-11T21:00:45+0530')
        self.head = self.git('rev-parse', 'HEAD')
        self.short = self.head[:12]
        self.tree = self.git('rev-parse', 'HEAD^{tree}')
        self.nightly = f'v{BASE}-dev.2.{UTC}+g{self.short}'

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.source), *args], text=True,
                                       stderr=subprocess.PIPE).strip()

    def commit(self, message, date):
        subprocess.run(['git', '-C', str(self.source), 'commit', '--allow-empty', '-qm', message],
                       env=dict(os.environ, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date), check=True)

    def cmake(self, tag='', source=None, supplied=None):
        command = ['cmake', f'-DSOURCE_DIR={source or self.source}',
                   f'-DOUTPUT_DIR={self.directory / "identity"}', f'-DBASE_VERSION={BASE}',
                   f'-DRELEASE_TAG={tag}']
        if supplied:
            command.extend(f'-DSUPPLIED_{key}={value}' for key, value in supplied.items())
        command.extend(['-P', str(REPO / 'cmake/GenerateBuildInfo.cmake')])
        # SOURCE_DATE_EPOCH must not substitute the workflow clock for source time.
        result = subprocess.run(command, text=True, capture_output=True,
                                env=dict(os.environ, SOURCE_DATE_EPOCH='0'))
        return result, (json.loads((self.directory / 'identity/build-info.json').read_text())
                        if result.returncode == 0 else None)

    def both(self, tag, accepted, package=None):
        result, compiled = self.cmake(tag)
        try:
            receipt = verify_tag.verify(self.source, tag)
        except (ValueError, subprocess.CalledProcessError):
            receipt = None
        with self.subTest(implementation='CMake'):
            self.assertEqual(result.returncode == 0, accepted, result.stderr)
        with self.subTest(implementation='Python'):
            self.assertEqual(receipt is not None, accepted, tag)
        if accepted and compiled is not None and receipt is not None:
            for key in ('version', 'baseVersion', 'packageVersion', 'gitCommit', 'gitTree', 'commitCount'):
                self.assertEqual(compiled[key], receipt[key], key)
            self.assertEqual(compiled['version'], tag[1:])
            self.assertEqual(compiled['packageVersion'], package)
            self.assertEqual(compiled['gitCommit'], self.head)
            self.assertEqual(compiled['gitTree'], self.tree)
            self.assertEqual(compiled['commitCount'], 2)
            self.assertEqual(compiled['schemaVersion'], 1)
            self.assertEqual(set(compiled), {'schemaVersion', 'version', 'baseVersion', 'packageVersion',
                             'gitCommit', 'gitTree', 'commitCount', 'dirty', 'sourceProvenance'})
        return receipt

    def test_stable_rc_characterization(self):
        for tag, package, prerelease in [('v0.1.1', '0.1.1', False),
                                        ('v0.1.1-rc.2', '0.1.1~rc.2', True),
                                        ('v0.1.1-rc.10', '0.1.1~rc.10', True)]:
            with self.subTest(tag=tag):
                self.git('tag', '-a', tag, '-m', 'Synthetic tag')
                receipt = self.both(tag, True, package)
                self.assertEqual(receipt['prerelease'], prerelease)
                self.assertEqual(receipt['channel'], 'candidate' if prerelease else 'stable')
        for tag in ['v0.1.1-rc.0', 'v0.1.1-rc.02', 'v00.1.1', 'v0.1.1-beta.1', 'v0.1.1+build']:
            with self.subTest(tag=tag):
                self.both(tag, False)

    def test_ordinary_dev_and_archive_characterization(self):
        self.git('tag', self.nightly)
        result, identity = self.cmake()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(identity['version'], f'{BASE}-dev.2+g{self.short}')
        self.assertEqual(identity['sourceProvenance'], 'git')
        (self.source / 'CMakeLists.txt').write_text('project(putgd VERSION 0.1.1)\n# synthetic edit\n')
        result, identity = self.cmake()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(identity['dirty'])
        self.assertEqual(identity['version'], f'{BASE}-dev.2+g{self.short}.dirty')
        archive = self.directory / 'archive'
        archive.mkdir()
        (archive / 'CMakeLists.txt').write_text('project(putgd VERSION 0.1.1)\n')
        supplied = {'COMMIT': self.head, 'TREE': self.tree, 'COUNT': 2}
        result, identity = self.cmake(source=archive, supplied=supplied)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(identity['version'], f'{BASE}-dev.2+g{self.short}')
        self.assertEqual(identity['sourceProvenance'], 'supplied')
        self.assertIsNone(identity['dirty'])
        for tag in ['v0.1.1', 'v0.1.1-rc.2', self.nightly]:
            with self.subTest(archive_tag=tag):
                result, _ = self.cmake(tag, archive, supplied)
                self.assertNotEqual(result.returncode, 0)
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    verify_tag.verify(archive, tag)

    def test_nightly_cross_language_case_table(self):
        wrong_hash = ('0' if self.short[0] != '0' else '1') + self.short[1:]
        cases = [
            ('exact source UTC', self.nightly, 'head', True),
            ('wrong base', f'v0.1.2-dev.2.{UTC}+g{self.short}', 'head', False),
            ('wrong count', f'v{BASE}-dev.3.{UTC}+g{self.short}', 'head', False),
            ('wrong short hash', f'v{BASE}-dev.2.{UTC}+g{wrong_hash}', 'head', False),
            ('wrong source date', f'v{BASE}-dev.2.20261010153045+g{self.short}', 'head', False),
            ('invalid UTC calendar', f'v{BASE}-dev.2.20260230153045+g{self.short}', 'head', False),
            ('invalid UTC clock', f'v{BASE}-dev.2.20261011243045+g{self.short}', 'head', False),
            ('leading zero count', f'v{BASE}-dev.02.{UTC}+g{self.short}', 'head', False),
            ('zero count', f'v{BASE}-dev.0.{UTC}+g{self.short}', 'head', False),
            ('leading zero base', f'v00.1.1-dev.2.{UTC}+g{self.short}', 'head', False),
            ('uppercase hash', f'v{BASE}-dev.2.{UTC}+g{self.short.upper()}', 'head', False),
            ('short hash length', f'v{BASE}-dev.2.{UTC}+g{self.short[:-1]}', 'head', False),
            ('timestamp length', f'v{BASE}-dev.2.{UTC[:-1]}+g{self.short}', 'head', False),
            ('missing tag', self.nightly, 'missing', False),
            ('wrong tag HEAD', self.nightly, 'previous', False),
            ('dirty source', self.nightly, 'dirty', False),
        ]
        for name, tag, state, accepted in cases:
            with self.subTest(case=name):
                self.git('tag', '-f', tag, 'HEAD~1' if state == 'previous' else 'HEAD')
                if state == 'missing': self.git('tag', '-d', tag)
                if state == 'dirty': (self.source / 'untracked').write_text('synthetic dirty file')
                receipt = self.both(tag, accepted, f'{BASE}~dev.2.{UTC}+g{self.short}' if accepted else None)
                if accepted and receipt is not None:
                    self.assertEqual(receipt['channel'], 'nightly')
                    self.assertTrue(receipt['prerelease'])
                if state == 'dirty': (self.source / 'untracked').unlink()

    def test_shallow_source_cannot_assert_full_history_count(self):
        shallow = self.directory / 'shallow'
        subprocess.run(['git', 'clone', '-q', '--depth=1', self.source.as_uri(), str(shallow)], check=True)
        short = self.head[:12]
        tag = f'v{BASE}-dev.1.{UTC}+g{short}'
        subprocess.run(['git', '-C', str(shallow), 'tag', tag], check=True)
        result, _ = self.cmake(tag, shallow)
        self.assertNotEqual(result.returncode, 0)
        with self.assertRaises((ValueError, subprocess.CalledProcessError)):
            verify_tag.verify(shallow, tag)

    PACKAGE_VERSIONS = ['0.1.1~dev.2+g0123456789ab',
                        '0.1.1~dev.2.20261010153045+g0123456789ab',
                        '0.1.1~dev.3.20261011153045+g0123456789ab',
                        '0.1.1~dev.10.20261012153045+g0123456789ab',
                        '0.1.1~rc.2', '0.1.1~rc.10', '0.1.1']

    @unittest.skipUnless(shutil.which('dpkg'), 'requires actual Debian dpkg comparison tool')
    def test_debian_package_ordering(self):
        versions = self.PACKAGE_VERSIONS
        for left, right in zip(versions, versions[1:]):
            subprocess.run(['dpkg', '--compare-versions', left, 'lt', right], check=True)
        subprocess.run(['dpkg', '--compare-versions', versions[-1], 'gt', versions[2]], check=True)

    @unittest.skipUnless(shutil.which('rpm'), 'requires actual Fedora RPM comparison tool')
    def test_rpm_package_ordering(self):
        versions = self.PACKAGE_VERSIONS
        for left, right in zip(versions, versions[1:]):
            expression = '%{lua:print(rpm.vercmp("' + left + '", "' + right + '"))}'
            self.assertEqual(subprocess.check_output(['rpm', '--eval', expression], text=True).strip(), '-1')
        expression = '%{lua:print(rpm.vercmp("' + versions[-1] + '", "' + versions[2] + '"))}'
        self.assertEqual(subprocess.check_output(['rpm', '--eval', expression], text=True).strip(), '1')


if __name__ == '__main__': unittest.main()
