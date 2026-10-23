"""refresh_pins.py re-derives URLs, digests and commits from versions; renovate_summary.py reports a run (#420)."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/maintenance'))
import refresh_pins
import renovate_summary


class FakeUpstream:
    """Tags, downloads and submodule trees from fixtures instead of the network."""

    def __init__(self, tags, files=None, checksums=None, modules=None, links=None):
        self.tags, self.files, self.checksums = tags, files or {}, checksums or {}
        self.modules, self.links = modules or {}, links or {}
        self.downloads = []

    def release_commit(self, repository, release):
        for tag in (release, 'v' + release, 'n' + release, 'release-' + release.replace('.', '-')):
            if (repository, tag) in self.tags:
                return self.tags[(repository, tag)]
        raise ValueError(f'No tag for {release} in {repository}')

    def sha256(self, url):
        self.downloads.append(url)
        return hashlib.sha256(self.files[url]).hexdigest()

    def published_sha256(self, url, asset):
        return self.checksums.get(url)

    def gitmodules(self, repository, commit):
        return self.modules.get((repository, commit), '')

    def gitlink(self, repository, commit, path):
        return self.links[(repository, commit, path)]


def tool(name, repository, release, url, sha='0' * 64, **extra):
    return {'name': name, 'repository': repository, 'release': release, 'url': url, 'sha256': sha, **extra}


class Tools(unittest.TestCase):
    def test_new_release_gets_its_asset_url_digest_and_commit(self):
        old = 'https://github.com/anchore/syft/releases/download/v1.55.0/syft_1.55.0_linux_amd64.tar.gz'
        new = 'https://github.com/anchore/syft/releases/download/v1.56.0/syft_1.56.0_linux_amd64.tar.gz'
        runtime = 'https://github.com/AppImage/type2-runtime/releases/download/20260101/runtime-x86_64'
        license_old = 'https://raw.githubusercontent.com/AppImage/type2-runtime/' + 'a' * 40 + '/LICENSE'
        license_new = 'https://raw.githubusercontent.com/AppImage/type2-runtime/' + 'b' * 40 + '/LICENSE'
        lock = {'tools': [tool('syft', 'anchore/syft', 'v1.56.0', old),
                          tool('runtime-x86_64', 'AppImage/type2-runtime', '20260101',
                               'https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64', commit='a' * 40),
                          tool('runtime-LICENSE', 'AppImage/type2-runtime', '20260101', license_old, commit='a' * 40),
                          tool('appimagetool', 'AppImage/appimagetool', '1.9.1',
                               'https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage', sha='1' * 64)]}
        files = {new: b'syft', runtime: b'runtime', license_new: b'MIT'}
        upstream = FakeUpstream({('AppImage/type2-runtime', '20260101'): 'b' * 40}, files,
                                {new: hashlib.sha256(b'syft').hexdigest()})
        changes = refresh_pins.refresh_tools(lock, upstream)
        syft, image, notice, appimagetool = lock['tools']
        self.assertEqual((syft['url'], syft['sha256']), (new, hashlib.sha256(b'syft').hexdigest()))
        self.assertEqual((image['url'], image['commit'], image['sha256']), (runtime, 'b' * 40, hashlib.sha256(b'runtime').hexdigest()))
        self.assertEqual((notice['url'], notice['sha256']), (license_new, hashlib.sha256(b'MIT').hexdigest()))
        # Unchanged versions are neither downloaded nor rewritten.
        self.assertEqual(appimagetool['sha256'], '1' * 64)
        self.assertNotIn(appimagetool['url'], upstream.downloads)
        self.assertEqual(len(changes), 5)
        self.assertEqual(refresh_pins.refresh_tools(lock, upstream), [])

    def test_published_checksum_must_agree(self):
        url = 'https://github.com/anchore/grype/releases/download/v0.122.0/grype_0.122.0_linux_amd64.tar.gz'
        lock = {'tools': [tool('grype', 'anchore/grype', 'v0.122.0',
                               'https://github.com/anchore/grype/releases/download/v0.121.0/grype_0.121.0_linux_amd64.tar.gz')]}
        upstream = FakeUpstream({}, {url: b'tampered'}, {url: hashlib.sha256(b'grype').hexdigest()})
        with self.assertRaisesRegex(ValueError, 'differs from its published checksum'):
            refresh_pins.refresh_tools(lock, upstream)

    def test_unexpected_url_is_rejected(self):
        lock = {'tools': [tool('x', 'owner/x', '2', 'https://example.invalid/x-1')]}
        with self.assertRaisesRegex(ValueError, 'Unexpected tool URL'):
            refresh_pins.refresh_tools(lock, FakeUpstream({}))


class BuildInputs(unittest.TestCase):
    def lock(self, qt='6.12.1', sources='6.12.1'):
        return {'buildInputs': {'qt': qt, 'qtLicenseSources': [
                    {'repository': 'https://github.com/qt/qtbase.git', 'release': sources, 'commit': '0' * 40}],
                'ffmpeg': {'repository': 'https://github.com/FFmpeg/FFmpeg.git', 'release': '7.1.6', 'commit': '0' * 40},
                'qtKeychain': {'repository': 'https://github.com/frankosterfeld/qtkeychain.git', 'release': '0.14.2', 'commit': '2' * 40},
                'distribution': 'Ubuntu 24.04'}}

    def test_each_source_commit_is_its_release_tag(self):
        lock = self.lock()
        upstream = FakeUpstream({('https://github.com/qt/qtbase.git', 'v6.12.1'): 'a' * 40,
                                 ('https://github.com/FFmpeg/FFmpeg.git', 'n7.1.6'): 'b' * 40,
                                 ('https://github.com/frankosterfeld/qtkeychain.git', '0.14.2'): '2' * 40})
        changes = refresh_pins.refresh_build_inputs(lock, upstream)
        self.assertEqual(lock['buildInputs']['qtLicenseSources'][0]['commit'], 'a' * 40)
        self.assertEqual(lock['buildInputs']['ffmpeg']['commit'], 'b' * 40)
        self.assertEqual(len(changes), 2)

    def test_qt_and_its_licence_sources_move_together(self):
        with self.assertRaisesRegex(ValueError, 'update them together'):
            refresh_pins.refresh_build_inputs(self.lock(sources='6.12.0'), FakeUpstream({}))


class AwsSdk(unittest.TestCase):
    def test_submodules_are_read_recursively_at_the_new_release(self):
        sdk, crt = 'https://github.com/aws/aws-sdk-cpp.git', 'https://github.com/awslabs/aws-crt-cpp.git'
        upstream = FakeUpstream(
            {(sdk, '1.11.916'): 'n' * 40},
            modules={(sdk, 'n' * 40): '[submodule "crt/aws-crt-cpp"]\n\tpath = crt/aws-crt-cpp\n\turl = ' + crt + '\n',
                     (crt, 'c' * 40): '[submodule "crt/s2n"]\n\tpath = crt/s2n\n\turl = ../../aws/s2n-tls.git\n'
                                      '[submodule "crt/aws-c-common"]\n\tpath = crt/aws-c-common\n\turl = ../aws-c-common.git\n'},
            links={(sdk, 'n' * 40, 'crt/aws-crt-cpp'): 'c' * 40, (crt, 'c' * 40, 'crt/s2n'): 's' * 40,
                   (crt, 'c' * 40, 'crt/aws-c-common'): 'o' * 40})
        lock = {'awsSdk': {'repository': sdk, 'release': '1.11.916', 'commit': 'a' * 40,
                           'submodules': [{'path': 'crt/aws-crt-cpp', 'commit': 'b' * 40},
                                          {'path': 'crt/aws-crt-cpp/crt/aws-c-io', 'commit': 'd' * 40}]}}
        changes = refresh_pins.refresh_aws(lock, upstream)
        self.assertEqual(lock['awsSdk']['commit'], 'n' * 40)
        self.assertEqual(lock['awsSdk']['submodules'], [{'path': 'crt/aws-crt-cpp', 'commit': 'c' * 40},
                                                        {'path': 'crt/aws-crt-cpp/crt/aws-c-common', 'commit': 'o' * 40},
                                                        {'path': 'crt/aws-crt-cpp/crt/s2n', 'commit': 's' * 40}])
        self.assertIn('  crt/aws-crt-cpp/crt/aws-c-io: removed', changes)
        self.assertEqual(refresh_pins.refresh_aws(lock, upstream), [])

    def test_refresh_writes_only_changed_locks(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            for name, body in (('packaging/appimage/tools.lock.json', {'tools': [], 'buildInputs': {'qt': '6.12.0', 'qtLicenseSources': [
                                   {'repository': 'q', 'release': '6.12.0', 'commit': 'q' * 40}]}}),
                               ('packaging/sbom/tools.lock.json', {'tools': []}),
                               ('cmake/dependency-lock.json', {'awsSdk': {'repository': 'r', 'release': '1', 'commit': 'a' * 40, 'submodules': []}})):
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(json.dumps(body, indent=2) + '\n')
            before = {path: path.read_bytes() for path in root.rglob('*.json')}
            self.assertEqual(refresh_pins.refresh(root, FakeUpstream({('r', '1'): 'a' * 40, ('q', 'v6.12.0'): 'q' * 40})), {})
            self.assertEqual(before, {path: path.read_bytes() for path in root.rglob('*.json')})


class Summary(unittest.TestCase):
    def test_lists_dependencies_updates_and_problems(self):
        files = {'regex': [{'packageFile': 'packaging/appimage/tools.lock.json', 'deps': [
                    {'depName': 'qt/qtbase', 'datasource': 'github-tags', 'currentValue': '6.8.3',
                     'updates': [{'newValue': '6.12.0', 'updateType': 'minor', 'dependencyDashboardApproval': True}]},
                    {'depName': 'unicode-org/icu', 'datasource': 'github-tags', 'currentValue': '73.2', 'skipReason': 'disabled'}]}],
                 'npm': [{'packageFile': 'tools/proton/package.json', 'deps': [
                    {'depName': 'ky', 'datasource': 'npm', 'currentValue': '1.14.3', 'updates': []}]}]}
        entries = [{'level': 20, 'msg': 'packageFiles with updates', 'config': files},
                   {'level': 40, 'msg': 'Found renovate config errors'}]
        text = renovate_summary.markdown(entries)
        self.assertIn('3 dependencies detected; 1 with an update.', text)
        self.assertIn('| regex | `packaging/appimage/tools.lock.json` | qt/qtbase | github-tags | 6.8.3 | 6.12.0 (minor, needs approval) |', text)
        self.assertIn('| unicode-org/icu | github-tags | 73.2 | skipped: disabled |', text)
        self.assertIn('| ky | npm | 1.14.3 | up to date |', text)
        self.assertIn('- warning: Found renovate config errors', text)


if __name__ == '__main__':
    unittest.main()
