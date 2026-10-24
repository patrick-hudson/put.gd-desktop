"""SBOM components come from the packages' own inventories; the policy flags or blocks by channel (#420)."""
import datetime
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import sbom

LOCK = {'awsSdk': {'repository': 'https://github.com/aws/aws-sdk-cpp.git', 'release': '1.11.907', 'commit': 'a' * 40,
                   'submodules': [{'path': 'crt/aws-crt-cpp', 'commit': 'b' * 40},
                                  {'path': 'crt/aws-crt-cpp/crt/s2n', 'commit': 'c' * 40},
                                  {'path': 'crt/aws-crt-cpp/crt/aws-lc', 'commit': 'd' * 40},
                                  {'path': 'crt/aws-crt-cpp/crt/s2n/tests/cbmc/aws-verification-model-for-libcrypto', 'commit': 'e' * 40}]}}
NOTICES = """put.gd Proton runtime: installed dependency license inventory.

Package: @oven/bun-linux-x64 1.4.2
Classification: embedded-runtime
Declared license: MIT
Integrity: sha512-x

Package: @protontech/openpgp 6.3.1
Classification: bundled-js
Declared license: LGPL-3.0+
Integrity: sha512-y

Package: typescript 5.9.3
Classification: build-only
Declared license: Apache-2.0
Integrity: sha512-z

Package: ky 1.14.3
Classification: bundled-js
Declared license: not declared
Integrity: sha512-w
"""
INVENTORY = {'components': [
    {'id': 'putgd', 'name': 'put.gd', 'version': '26.1023.1058-nightly'},
    {'id': 'ffmpeg', 'name': 'FFmpeg', 'version': '7.1.5', 'license': 'GPL-2.0-or-later',
     'source': {'repository': 'https://github.com/FFmpeg/FFmpeg.git', 'commit': 'f' * 40}},
    {'id': 'qtbase', 'name': 'Qt qtbase', 'version': '6.12.0', 'license': 'LGPL-3.0-only',
     'source': {'repository': 'https://github.com/qt/qtbase.git', 'commit': '1' * 40},
     'embedded': [{'name': 'LibPNG', 'version': '1.6.47', 'license': 'libpng-2.0'},
                  {'name': 'PCRE2', 'version': '10.45', 'license': 'BSD-3-Clause'},
                  {'name': 'PCRE2 - Stack-less Just-In-Time Compiler', 'version': '10.45'},
                  {'name': 'Unicode Common Locale Data Repository (CLDR)', 'version': 'v46.1'},
                  {'name': 'Anti-aliasing rasterizer from FreeType 2', 'version': 'unknown'}]},
    {'id': 'qtkeychain', 'name': 'QtKeychain', 'version': '8eac9889775c',
     'source': {'repository': 'https://github.com/frankosterfeld/qtkeychain.git', 'commit': '8eac9889775ca45c106d077bb0d3744754a1570a'}},
    {'id': 'aws-sdk-cpp', 'name': 'AWS SDK', 'version': '1.11.907'},
    {'id': 'proton-helper', 'name': 'Proton helper', 'version': 'x'},
    {'id': 'ubuntu:libssl3t64', 'name': 'libssl3t64', 'version': '3.0.13-0ubuntu3.6', 'license': 'Apache-2.0',
     'source': {'distribution': 'Ubuntu 24.04', 'sourcePackage': 'openssl', 'sourceVersion': '3.0.13-0ubuntu3.6'}},
    {'id': 'ubuntu:libx264-164', 'name': 'libx264-164', 'version': '2:0.164.3108+git31e19f9-1',
     'source': {'distribution': 'Ubuntu 24.04', 'sourcePackage': 'x264', 'sourceVersion': '2:0.164.3108+git31e19f9-1'}},
    {'id': 'ubuntu:zlib1g', 'name': 'zlib1g', 'version': '1:1.3.dfsg-3.1ubuntu2.1',
     'source': {'distribution': 'Ubuntu 24.04', 'sourcePackage': 'zlib', 'sourceVersion': '1:1.3.dfsg-3.1ubuntu2.1'}},
]}


def properties(item):
    return {prop['name']: prop['value'] for prop in item['properties']}


class Components(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='pgsbom-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for path, body in (('usr/share/doc/putgd/bundled-components.json', json.dumps(INVENTORY)),
                           ('usr/share/putgd/dependency-lock.json', json.dumps(LOCK)),
                           ('usr/libexec/putgd/proton/THIRD-PARTY-NOTICES.txt', NOTICES)):
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_text(body)

    def by_name(self, components):
        return {item['name']: item for item in components}

    def test_appimage_inventory_gives_purls_cpes_and_origins(self):
        found = self.by_name(sbom.appimage_components(self.root))
        self.assertNotIn('putgd', found)
        self.assertEqual(found['ffmpeg']['purl'], 'pkg:generic/ffmpeg@7.1.5')
        self.assertEqual(found['ffmpeg']['cpe'], 'cpe:2.3:a:ffmpeg:ffmpeg:7.1.5:*:*:*:*:*:*:*')
        self.assertEqual(found['qtbase']['cpe'], 'cpe:2.3:a:qt:qt:6.12.0:*:*:*:*:*:*:*')
        self.assertNotIn('cpe', found['qtkeychain'])
        self.assertEqual(properties(found['qtkeychain'])['putgd:commit'], '8eac9889775ca45c106d077bb0d3744754a1570a')
        # Qt's own third-party code, once per library, only with a usable version.
        self.assertEqual(found['libpng']['cpe'], 'cpe:2.3:a:libpng:libpng:1.6.47:*:*:*:*:*:*:*')
        self.assertEqual(properties(found['libpng'])['putgd:embeddedIn'], 'qtbase')
        self.assertEqual(sum(1 for item in sbom.appimage_components(self.root) if item['name'] == 'pcre2'), 1)
        self.assertNotIn('freetype', found)
        self.assertEqual(found['libssl3t64']['purl'], 'pkg:deb/ubuntu/libssl3t64@3.0.13-0ubuntu3.6?arch=amd64'
                         '&distro=ubuntu-24.04&upstream=openssl')
        self.assertEqual(found['libx264-164']['purl'], 'pkg:deb/ubuntu/libx264-164@2%3A0.164.3108%2Bgit31e19f9-1'
                         '?arch=amd64&distro=ubuntu-24.04&upstream=x264')
        self.assertEqual(properties(found['libssl3t64'])['putgd:origin'], 'bundled-distro')
        self.assertEqual(properties(found['ffmpeg'])['putgd:origin'], 'bundled')
        # The AWS SDK and Proton helper come from their own locks and notices.
        self.assertNotIn('AWS SDK', found)
        self.assertIn('aws-sdk-cpp', found)
        self.assertIn('bun', found)

    def test_aws_lock_lists_shipped_crt_libraries_only(self):
        found = self.by_name(sbom.aws_components(LOCK))
        self.assertEqual(set(found), {'aws-sdk-cpp', 'aws-crt-cpp', 's2n'})
        self.assertEqual(found['s2n']['purl'], 'pkg:github/aws/s2n-tls@' + 'c' * 40)
        self.assertEqual(found['aws-crt-cpp']['purl'], 'pkg:github/awslabs/aws-crt-cpp@' + 'b' * 40)
        self.assertEqual(found['aws-sdk-cpp']['version'], '1.11.907')

    def test_proton_notices_give_bundled_npm_packages_and_the_bun_runtime(self):
        found = self.by_name(sbom.proton_components(self.root / 'usr/libexec/putgd/proton/THIRD-PARTY-NOTICES.txt'))
        self.assertEqual(set(found), {'bun', '@protontech/openpgp', 'ky'})
        self.assertEqual(found['bun']['purl'], 'pkg:npm/bun@1.4.2')
        self.assertEqual(properties(found['bun'])['putgd:npmPackage'], '@oven/bun-linux-x64')
        self.assertEqual(found['@protontech/openpgp']['purl'], 'pkg:npm/%40protontech/openpgp@6.3.1')
        self.assertEqual(found['@protontech/openpgp']['cpe'], 'cpe:2.3:a:openpgpjs:openpgpjs:6.3.1:*:*:*:*:*:*:*')
        self.assertNotIn('licenses', found['ky'])
        empty = self.root / 'empty.txt'
        empty.write_text('no packages\n')
        with self.assertRaisesRegex(ValueError, 'list no packages'):
            sbom.proton_components(empty)

    def test_native_packages_list_their_bundled_sdk_and_helper(self):
        names = {item['name'] for item in sbom.native_components(self.root)}
        self.assertEqual(names, {'aws-sdk-cpp', 'aws-crt-cpp', 's2n', 'bun', '@protontech/openpgp', 'ky'})

    def test_syft_duplicates_of_declared_components_are_dropped(self):
        declared = sbom.appimage_components(self.root)
        syft = {'components': [{'type': 'library', 'name': 'ffmpeg', 'version': '61.19.101'},
                               {'type': 'library', 'name': 'qtbase', 'version': '6.12.0'},
                               {'type': 'library', 'name': 'openssl', 'version': '3.0.13-0ubuntu3.6'},
                               {'type': 'file', 'name': '/usr/lib/x'},
                               {'type': 'library', 'name': 'go-module', 'version': '1.0'}]}
        kept = sbom.merge_syft(syft, declared)
        self.assertEqual([item['name'] for item in kept], ['go-module'])
        self.assertEqual(kept[0]['properties'], [{'name': 'putgd:origin', 'value': 'detected'}])

    def test_disposition_fingerprint_uses_actual_bytes_and_rejects_added_tools(self):
        for identifier, library, tool in [('ffmpeg', 'libavcodec.so.61', 'ffmpeg'),
                                           ('icu', 'libicuuc.so.73', 'genrb')]:
            with self.subTest(component=identifier):
                name = 'usr/lib/' + library
                path = self.root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'synthetic original library')
                item = {'id': identifier, 'files': [name]}
                before = dict(sbom.appimage_scope_properties(self.root, item))
                self.assertEqual(before['putgd:toolsAbsent'], 'true')
                path.write_bytes(b'synthetic changed build/configuration')
                after = dict(sbom.appimage_scope_properties(self.root, item))
                self.assertNotEqual(before['putgd:filesSha256'], after['putgd:filesSha256'])
                executable = self.root / 'usr/bin' / tool
                executable.parent.mkdir(parents=True, exist_ok=True)
                executable.write_bytes(b'synthetic tool')
                self.assertEqual(dict(sbom.appimage_scope_properties(self.root, item))['putgd:toolsAbsent'], 'false')
                executable.unlink()
        (self.root / 'usr/lib/libicutu.so.73').write_bytes(b'synthetic tool utility library')
        self.assertEqual(dict(sbom.appimage_scope_properties(self.root, item))['putgd:toolsAbsent'], 'false')

    def test_disposition_fingerprint_rejects_missing_or_external_files(self):
        for name in ['usr/lib/missing.so', '../outside']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                sbom.appimage_scope_properties(self.root, {'id': 'icu', 'files': [name]})


class Policy(unittest.TestCase):
    def finding(self, **values):
        return {'id': 'CVE-2026-1', 'severity': 'High', 'aliases': [], 'component': 'libpng', 'version': '1.6.47',
                'origin': 'bundled', 'fixState': 'fixed', 'fixedIn': ['1.6.56'], 'package': 'appimage', **values}

    def policy(self, ignore=()):
        return {'schemaVersion': 1, 'severities': ['Critical', 'High'], 'requireFix': True,
                'block': {'development': False, 'nightly': False, 'stable': True}, 'ignore': list(ignore)}

    def test_channels(self):
        self.assertEqual(sbom.channel(''), 'development')
        self.assertEqual(sbom.channel('v26.1023.1058-nightly'), 'nightly')
        self.assertEqual(sbom.channel('v26.1101.1100'), 'stable')

    def test_only_fixable_serious_bundled_findings_flag_and_stable_blocks(self):
        today = datetime.date(2026, 10, 23)
        items = [self.finding(), self.finding(id='CVE-2026-2', fixState='not-fixed', fixedIn=[]),
                 self.finding(id='CVE-2026-3', severity='Medium'), self.finding(id='CVE-2026-4', origin='distro'),
                 self.finding(id='CVE-2026-5', origin='bundled-distro')]
        self.assertFalse(sbom.classify(items, self.policy(), 'nightly', today))
        self.assertEqual([item['status'] for item in items], ['flagged', 'reported', 'reported', 'reported', 'flagged'])
        self.assertTrue(sbom.classify(items, self.policy(), 'stable', today))
        self.assertEqual(items[0]['status'], 'blocking')

    def test_accepted_findings_until_expiry_by_id_or_alias(self):
        rule = {'id': 'CVE-2026-1', 'component': 'libpng', 'reason': 'decoder not built', 'expires': '2026-11-01'}
        items = [self.finding(), self.finding(id='GHSA-xxxx', aliases=['CVE-2026-1']), self.finding(component='sqlite')]
        sbom.classify(items, self.policy([rule]), 'stable', datetime.date(2026, 10, 23))
        self.assertEqual([item['status'] for item in items], ['accepted until 2026-11-01', 'accepted until 2026-11-01', 'blocking'])
        sbom.classify(items, self.policy([rule]), 'stable', datetime.date(2026, 11, 2))
        self.assertEqual(items[0]['status'], 'blocking (acceptance expired 2026-11-01)')

    def test_policy_requires_reason_and_expiry(self):
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / 'policy.json'
            for rule in ({'id': 'CVE-1', 'component': 'x', 'expires': '2026-11-01'},
                         {'id': 'CVE-1', 'component': 'x', 'reason': ' ', 'expires': '2026-11-01'},
                         {'id': 'CVE-1', 'component': 'x', 'reason': 'r', 'expires': 'soon'}):
                path.write_text(json.dumps(self.policy([rule])))
                with self.assertRaises(ValueError):
                    sbom.load_policy(path)

    def scoped_policy(self):
        policy = self.policy([{'id': 'CVE-2026-1', 'component': 'libpng', 'reason': 'synthetic audited build',
                               'expires': '2027-01-21', 'scope': 'audited'}])
        policy['schemaVersion'] = 2
        policy['scopes'] = {'audited': {'package': 'appimage', 'component': 'libpng', 'version': '1.6.47',
                                       'properties': {'putgd:commit': 'a' * 40, 'putgd:filesSha256': 'b' * 64,
                                                      'putgd:toolsAbsent': 'true'}}}
        return policy

    def test_scoped_acceptance_is_bound_to_package_version_commit_and_actual_build(self):
        policy = self.scoped_policy()
        props = policy['scopes']['audited']['properties']
        original = self.finding(properties=props.copy())
        cases = [self.finding(properties=props.copy(), package='debian13'),
                 self.finding(properties=props.copy(), version='1.6.48'), self.finding(),
                 self.finding(properties=props.copy(), id='CVE-2026-NEW')]
        for key, value in [('putgd:commit', 'c' * 40), ('putgd:filesSha256', 'd' * 64), ('putgd:toolsAbsent', 'false')]:
            cases.append(self.finding(properties={**props, key: value}))
        sbom.classify([original, *cases], policy, 'stable', datetime.date(2026, 10, 24))
        self.assertEqual(original['status'], 'accepted until 2027-01-21')
        self.assertEqual(original['acceptance']['scope'], 'audited')
        self.assertTrue(all(item['status'].startswith('blocking') for item in cases))
        sbom.classify([original], policy, 'stable', datetime.date(2027, 1, 22))
        self.assertEqual(original['status'], 'blocking (acceptance expired 2027-01-21)')
        self.assertNotIn('acceptance', original)

    def test_scoped_policy_rejects_incomplete_or_mismatched_guards(self):
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / 'policy.json'
            mutations = [lambda p: p['ignore'][0].update(scope='unknown'),
                         lambda p: p.update(schemaVersion=1),
                         lambda p: p['ignore'][0].pop('scope'),
                         lambda p: p['scopes']['audited'].pop('version'),
                         lambda p: p['scopes']['audited'].update(component='ffmpeg'),
                         lambda p: p['scopes']['audited']['properties'].pop('putgd:filesSha256'),
                         lambda p: p['scopes']['audited']['properties'].update({'putgd:commit': 'not-a-commit'})]
            for mutate in mutations:
                policy = copy.deepcopy(self.scoped_policy())
                mutate(policy)
                path.write_text(json.dumps(policy))
                with self.subTest(policy=policy), self.assertRaises(ValueError):
                    sbom.load_policy(path)

    def test_findings_take_scope_evidence_from_sbom_not_scanner_claims(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            bom, scan = root / 'bom.json', root / 'scan.json'
            props = self.scoped_policy()['scopes']['audited']['properties']
            bom.write_text(json.dumps({'metadata': {'component': {'properties': [{'name': 'putgd:label', 'value': 'appimage'}]}},
                                       'components': [sbom.component('libpng', '1.6.47', 'pkg:generic/libpng@1.6.47', 'bundled',
                                                                      properties=list(props.items()))]}))
            scan.write_text(json.dumps({'matches': [{'artifact': {'name': 'libpng', 'version': '1.6.47',
                                                                  'properties': {'putgd:filesSha256': 'forged'}},
                                                     'vulnerability': {'id': 'CVE-2026-1', 'severity': 'High'}}]}))
            self.assertEqual(sbom.findings(bom, scan)[0]['properties']['putgd:filesSha256'], props['putgd:filesSha256'])

    def test_repository_policy_and_tool_lock_are_valid(self):
        policy = sbom.load_policy()
        self.assertEqual(policy['schemaVersion'], 2)
        self.assertTrue(policy['block']['stable'])
        self.assertTrue(policy['requireFix'])
        self.assertEqual(policy['severities'], ['Critical', 'High'])
        self.assertFalse(policy['block']['nightly'])
        evidence = json.loads((sbom.ROOT / 'packaging/sbom/dispositions/511.json').read_text())
        self.assertEqual(evidence['scopeFingerprints'], policy['scopes'])
        inputs = json.loads((sbom.ROOT / 'packaging/appimage/tools.lock.json').read_text())['buildInputs']
        for scope in policy['scopes'].values():
            item = inputs[scope['component']]
            self.assertEqual(scope['version'], item['release'])
            self.assertEqual(scope['properties']['putgd:commit'], item['commit'])
        configure = json.loads((sbom.ROOT / 'packaging/appimage/media.json').read_text())['ffmpeg']['configure']
        self.assertEqual(evidence['ffmpeg']['configuredComponents'], configure,
                         'FFmpeg configure changes require disposition reassessment')
        for entry in policy['ignore']:
            self.assertIn(entry['scope'], policy['scopes'])
            self.assertEqual(entry['evidence'], 'packaging/sbom/dispositions/511.json')
        lock = json.loads(sbom.TOOLS_LOCK.read_text())
        self.assertEqual({tool['name'] for tool in lock['tools']}, {'syft', 'grype'})
        for tool in lock['tools']:
            self.assertRegex(tool['sha256'], r'^[0-9a-f]{64}$')
            self.assertTrue(tool['url'].startswith('https://github.com/' + tool['repository'] + '/releases/download/' + tool['release'] + '/'))

    def test_published_sboms_are_read_by_their_labels(self):
        with tempfile.TemporaryDirectory() as work:
            directory = Path(work)
            with self.assertRaisesRegex(ValueError, 'No SBOMs'):
                sbom.published_receipts(directory)
            for label in ('appimage', 'fedora44'):
                (directory / f'putgd-26.1101.1-{label}.cdx.json').write_text(json.dumps({'metadata': {'component': {
                    'properties': [{'name': 'putgd:label', 'value': label}]}}, 'components': [{}, {}]}))
            self.assertEqual(sbom.published_receipts(directory)['fedora44'],
                             {'label': 'fedora44', 'file': 'putgd-26.1101.1-fedora44.cdx.json', 'components': 2})

    def test_summary_lists_flagged_serious_findings_first(self):
        items = [self.finding(id='CVE-2026-9', fixState='not-fixed', fixedIn=[]), self.finding(), self.finding(package='debian13', component='s2n')]
        sbom.classify(items, self.policy(), 'nightly', datetime.date(2026, 10, 23))
        text = sbom.summary_markdown({'appimage': {'components': 168}, 'debian13': {'components': 25}}, items,
                                     {'schemaVersion': 'v6.1.10', 'built': '2026-10-23T06:30:15Z'}, 'nightly', False)
        self.assertIn('flagged, not blocking', text)
        self.assertIn('| appimage | 168 | 0 | 2 | 0 | 0 | 0 | 0 | 1 |', text)
        rows = [line for line in text.splitlines() if line.startswith('| CVE-')]
        self.assertTrue(rows[0].startswith('| CVE-2026-1 | High | libpng'))
        self.assertTrue(rows[-1].startswith('| CVE-2026-9 '))


if __name__ == '__main__':
    unittest.main()
