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
import quiet_git  # noqa: F401  (disables git background maintenance, #99)
import verify_tag
from release_identity import calendar_base, parse_version

# HEAD is committed 2026-10-12 02:00 +0530: October 11 in UTC, the second commit.
BASE = '26.1011.2'
UTC = '20261011153045'


class ReleaseIdentity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pgidentity-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.source = self.directory / 'source'
        self.source.mkdir()
        (self.source / 'CMakeLists.txt').write_text('project(putgd LANGUAGES CXX)\n')
        self.git('init', '-q')
        self.git('config', 'user.name', 'Synthetic identity fixture')
        self.git('config', 'user.email', 'identity@example.invalid')
        self.git('add', '.')
        self.commit('First', '2026-10-10T21:00:45+0530')
        self.commit('Second', '2026-10-12T02:00:45+0530')
        self.head = self.git('rev-parse', 'HEAD')
        self.short = self.head[:12]
        self.tree = self.git('rev-parse', 'HEAD^{tree}')
        self.time = int(self.git('show', '-s', '--format=%ct', 'HEAD'))
        self.nightly = f'v{BASE}-nightly'

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.source), *args], text=True,
                                       stderr=subprocess.PIPE).strip()

    def commit(self, message, date):
        subprocess.run(['git', '-C', str(self.source), 'commit', '--allow-empty', '-qm', message],
                       env=dict(os.environ, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date), check=True)

    def cmake(self, tag='', source=None, supplied=None):
        command = ['cmake', f'-DSOURCE_DIR={source or self.source}',
                   f'-DOUTPUT_DIR={self.directory / "identity"}', f'-DRELEASE_TAG={tag}']
        if supplied:
            command.extend(f'-DSUPPLIED_{key}={value}' for key, value in supplied.items())
        command.extend(['-P', str(REPO / 'cmake/GenerateBuildInfo.cmake')])
        # SOURCE_DATE_EPOCH must not substitute the workflow clock for the commit time.
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

    def test_qt_installer_logging_keeps_tagged_source_clean(self):
        self.git('tag', self.nightly)
        self.both(self.nightly, True, f'{BASE}~nightly')
        runner = self.directory / 'runner'
        runner.mkdir()
        config = REPO / 'tools/ci/aqt-logging.ini'
        subprocess.run([sys.executable, '-c',
            "import logging, logging.config, sys; "
            "logging.config.fileConfig(sys.argv[1]); "
            "logging.getLogger('aqt').debug('synthetic installer diagnostic')",
            str(config)], cwd=self.source, check=True, capture_output=True,
            env=dict(os.environ, RUNNER_TEMP=str(runner)))
        self.assertIn('synthetic installer diagnostic', (runner / 'aqtinstall.log').read_text())
        self.assertFalse((self.source / 'aqtinstall.log').exists())
        self.assertEqual(self.git('status', '--porcelain', '--untracked-files=normal'), '')
        self.both(self.nightly, True, f'{BASE}~nightly')
        # The logging override must not hide actual source changes.
        (self.source / 'CMakeLists.txt').write_text('project(putgd)\n# mutation\n')
        self.both(self.nightly, False)

    def test_stable_characterization(self):
        self.git('tag', '-a', f'v{BASE}', '-m', 'Synthetic tag')
        receipt = self.both(f'v{BASE}', True, BASE)
        self.assertFalse(receipt['prerelease'])
        self.assertEqual(receipt['channel'], 'stable')
        # Candidates ended with #373; pre-#373 tags and other dates or counts are not this commit's.
        for tag in ['v0.1.1', 'v0.1.1-rc.2', f'v{BASE}-rc', f'v{BASE}-rc.1', 'v26.1012.2', 'v26.1011.3',
                    'v26.01011.2', 'v26.1011.02', 'v026.1011.2', f'v{BASE}+build', f'v{BASE}-beta']:
            with self.subTest(tag=tag):
                self.git('tag', '-f', tag)
                self.both(tag, False)

    def test_ordinary_dev_and_archive_characterization(self):
        self.git('tag', self.nightly)
        result, identity = self.cmake()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(identity['version'], f'{BASE}-dev+g{self.short}')
        self.assertEqual(identity['packageVersion'], f'{BASE}~dev+g{self.short}')
        self.assertEqual(identity['baseVersion'], BASE)
        self.assertEqual(identity['sourceProvenance'], 'git')
        (self.source / 'CMakeLists.txt').write_text('project(putgd)\n# synthetic edit\n')
        result, identity = self.cmake()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(identity['dirty'])
        self.assertEqual(identity['version'], f'{BASE}-dev+g{self.short}.dirty')
        archive = self.directory / 'archive'
        archive.mkdir()
        (archive / 'CMakeLists.txt').write_text('project(putgd)\n')
        supplied = {'COMMIT': self.head, 'TREE': self.tree, 'COUNT': 2, 'TIME': self.time}
        result, identity = self.cmake(source=archive, supplied=supplied)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(identity['version'], f'{BASE}-dev+g{self.short}')
        self.assertEqual(identity['version'], calendar_base(self.time, 2) + f'-dev+g{self.short}')
        # An archive must supply the commit time with the rest of its provenance.
        result, _ = self.cmake(source=archive, supplied={'COMMIT': self.head, 'TREE': self.tree, 'COUNT': 2})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(identity['sourceProvenance'], 'supplied')
        self.assertIsNone(identity['dirty'])
        for tag in [f'v{BASE}', self.nightly]:
            with self.subTest(archive_tag=tag):
                result, _ = self.cmake(tag, archive, supplied)
                self.assertNotEqual(result.returncode, 0)
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    verify_tag.verify(archive, tag)

    def test_nightly_cross_language_case_table(self):
        cases = [
            ('exact source date and count', self.nightly, 'head', True),
            ('local date instead of UTC', 'v26.1012.2-nightly', 'head', False),
            ('wrong count', 'v26.1011.3-nightly', 'head', False),
            ('leading zero count', 'v26.1011.02-nightly', 'head', False),
            ('leading zero date', 'v26.01011.2-nightly', 'head', False),
            ('zero count', 'v26.1011.0-nightly', 'head', False),
            ('pre-#373 numbered nightly', 'v0.1.1-nightly.2', 'head', False),
            ('numbered nightly', f'v{BASE}-nightly.2', 'head', False),
            ('uppercase label', f'v{BASE}-Nightly', 'head', False),
            ('build metadata', f'v{BASE}-nightly+g{self.short}', 'head', False),
            ('previous dev format', f'v0.1.1-dev.2.{UTC}+g{self.short}', 'head', False),
            ('missing tag', self.nightly, 'missing', False),
            ('wrong tag HEAD', self.nightly, 'previous', False),
            ('dirty source', self.nightly, 'dirty', False),
        ]
        for name, tag, state, accepted in cases:
            with self.subTest(case=name):
                self.git('tag', '-f', tag, 'HEAD~1' if state == 'previous' else 'HEAD')
                if state == 'missing': self.git('tag', '-d', tag)
                if state == 'dirty': (self.source / 'untracked').write_text('synthetic dirty file')
                receipt = self.both(tag, accepted, f'{BASE}~nightly' if accepted else None)
                if accepted and receipt is not None:
                    self.assertEqual(receipt['channel'], 'nightly')
                    self.assertTrue(receipt['prerelease'])
                if state == 'dirty': (self.source / 'untracked').unlink()

    def test_shallow_source_cannot_assert_full_history_count(self):
        shallow = self.directory / 'shallow'
        subprocess.run(['git', 'clone', '-q', '--depth=1', self.source.as_uri(), str(shallow)], check=True)
        tag = 'v' + calendar_base(self.time, 1) + '-nightly'
        subprocess.run(['git', '-C', str(shallow), 'tag', tag], check=True)
        result, _ = self.cmake(tag, shallow)
        self.assertNotEqual(result.returncode, 0)
        with self.assertRaises((ValueError, subprocess.CalledProcessError)):
            verify_tag.verify(shallow, tag)

    def test_grammar_and_utc_dates(self):
        self.assertEqual(calendar_base(self.time, 2), BASE)
        self.assertEqual(calendar_base(1767571200, 990), '26.105.990')  # 2026-01-05T00:00:00Z
        self.assertEqual(calendar_base(1767571199, 990), '26.104.990')  # one second earlier
        self.assertEqual(parse_version('28.229.5').channel, 'stable')  # 2028 is a leap year
        for version in ['26.229.5', '26.1301.5', '26.132.5', '25.1231.5', '26.0105.5', '26.105.05', '26.105.0',
                        '26.1101.960-rc', '26.1101.960-rc.1', '26.1022.942-nightly.942', '1.10.0', '026.105.5']:
            with self.subTest(version=version), self.assertRaises(ValueError):
                parse_version(version)
        ordered = ['0.1.1-nightly.2', '0.1.1-nightly.925', '0.1.1-rc.1', '0.1.1', '26.105.990-nightly', '26.105.990',
                   '26.1011.2-nightly', '26.1011.3-nightly', '26.1011.10-nightly', '26.1011.10', '26.1012.5-nightly',
                   '27.101.5-nightly']
        keys = [parse_version(version).key for version in ordered]
        self.assertEqual(keys, sorted(keys))
        self.assertEqual(len(set(keys)), len(keys))
        self.assertEqual(parse_version('26.1022.942-nightly').package_version, '26.1022.942~nightly')
        self.assertEqual(parse_version('0.1.1-nightly.925').package_version, '0.1.1~nightly.925')

    # Untagged builds, then the pre-#373 nightlies, candidates and stable, then
    # calendar versions (#373): upgrades follow this order, with no epoch.
    PACKAGE_VERSIONS = ['0.1.1~dev.2+g0123456789ab',
                        '0.1.1~dev.309.20261018030128+gcb26dc713228',
                        '0.1.1~nightly.2', '0.1.1~nightly.3', '0.1.1~nightly.925',
                        '0.1.1~rc.2', '0.1.1~rc.10', '0.1.1',
                        '26.105.990~dev+g0123456789ab', '26.105.990~nightly', '26.105.990',
                        '26.1011.2~nightly', '26.1011.3~nightly', '26.1011.10~dev+g0123456789ab',
                        '26.1011.10~nightly', '26.1011.10', '26.1012.5~nightly', '27.101.5~nightly']

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
