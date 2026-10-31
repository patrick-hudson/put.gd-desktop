"""Exact download checks fail closed on missing, extra or changed release bytes."""
import hashlib
import json
from pathlib import Path
import sys
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import io

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import audit_assets
import quiet_git  # noqa: F401


class DownloadChecks(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / 'package.deb').write_bytes(b'exact downloaded bytes')
        self.digest = hashlib.sha256(b'exact downloaded bytes').hexdigest()
        (self.root / 'SHA256SUMS').write_text(self.digest + '  package.deb\n')
        self.expected = {'SHA256SUMS', 'package.deb'}

    def test_complete_checksums(self):
        result = audit_assets.checksums(self.root, self.expected)
        self.assertEqual(result['package.deb']['sha256'], self.digest)

    def test_missing_extra_duplicate_or_changed(self):
        for text in ('', self.digest + '  ../package.deb\n',
                     (self.digest + '  package.deb\n') * 2, '0' * 64 + '  package.deb\n'):
            with self.subTest(text=text):
                (self.root / 'SHA256SUMS').write_text(text)
                with self.assertRaises(ValueError):
                    audit_assets.checksums(self.root, self.expected)
        (self.root / 'extra').touch()
        with self.assertRaises(ValueError):
            audit_assets.checksums(self.root, self.expected)

    def test_symlink_is_rejected(self):
        package = self.root / 'package.deb'
        package.unlink()
        package.symlink_to('/dev/null')
        with self.assertRaises(ValueError):
            audit_assets.checksums(self.root, self.expected)

    def test_authenticated_list_resolves_draft_before_asset_snapshot(self):
        metadata = {'id': 123, 'tag_name': 'v26.1031.3384', 'draft': True,
                    'assets': [{'id': 456, 'name': 'package.deb', 'state': 'uploaded',
                        'size': (self.root / 'package.deb').stat().st_size,
                        'digest': 'sha256:' + self.digest}]}
        with patch.object(audit_assets, 'run', side_effect=[json.dumps([metadata]), json.dumps(metadata)]) as api:
            snapshot = audit_assets.remote_snapshot('owner/repo', 'v26.1031.3384', self.root, {'package.deb'})
        self.assertTrue(snapshot['draft'])
        self.assertIn('releases?per_page=100', api.call_args_list[0].args[-1])
        self.assertEqual(api.call_args_list[1].args[-1], 'repos/owner/repo/releases/123')

    def test_duplicate_draft_on_later_page_is_rejected(self):
        first = [{'id': 1, 'tag_name': 'v26.1031.3384'}] + [
            {'id': n + 2, 'tag_name': 'unrelated-' + str(n)} for n in range(99)]
        second = [{'id': 200, 'tag_name': 'v26.1031.3384'}]
        with patch.object(audit_assets, 'run', side_effect=[json.dumps(first), json.dumps(second)]) as api:
            with self.assertRaisesRegex(ValueError, 'Ambiguous release tag'):
                audit_assets.remote_snapshot('owner/repo', 'v26.1031.3384', self.root, {'package.deb'})
        self.assertEqual(api.call_count, 2)

    def test_rebuild_must_reproduce_both_runtime_copies(self):
        catalog = {'helperSha256': self.digest}
        audit_assets.verify_rebuilt_proton({'rebuiltProton': {'sha256': self.digest, 'bundledSha256': self.digest}}, catalog)
        for built in ({}, {'rebuiltProton': {'sha256': self.digest}},
                      {'rebuiltProton': {'sha256': self.digest, 'bundledSha256': '0' * 64}}):
            with self.assertRaises(ValueError):
                audit_assets.verify_rebuilt_proton(built, catalog)


class SourceChecks(unittest.TestCase):
    def make_archive(self, directory, path, body=b'source', recorded=None):
        artifact = directory / 'source.tar.gz'
        identity = {'gitCommit': 'a' * 40}
        content = {'source/source-info.json': json.dumps({'identity': identity, 'files': [
            {'path': path, 'mode': '0644', 'bytes': len(body), 'sha256': hashlib.sha256(recorded or body).hexdigest()}]}).encode(),
            'source/' + path: body}
        with tarfile.open(artifact, 'w:gz') as archive:
            directories = set()
            for name in content:
                parent = Path(name).parent
                while str(parent) != '.':
                    directories.add(parent.as_posix())
                    parent = parent.parent
            for name in sorted(directories):
                member = tarfile.TarInfo(name)
                member.type = tarfile.DIRTYPE
                member.mode = 0o755
                archive.addfile(member)
            for name, value in content.items():
                member = tarfile.TarInfo(name)
                member.size = len(value)
                archive.addfile(member, io.BytesIO(value))
        return artifact, {'archiveRoot': 'source', 'identity': identity, 'members': [
            {'path': path, 'mode': '0644', 'bytes': len(body), 'sha256': hashlib.sha256(recorded or body).hexdigest()},
            {'path': 'source-info.json', 'mode': '0644', 'bytes': len(content['source/source-info.json']),
             'sha256': hashlib.sha256(content['source/source-info.json']).hexdigest()}]}

    def test_source_hashes_and_private_path_exclusion(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact, receipt = self.make_archive(root, 'src/main.cpp')
            self.assertEqual(audit_assets.source_export(artifact, receipt)['files'], 1)
            for path, recorded in (('work/local.txt', None), ('src/credentials.txt', None),
                                   ('src/main.cpp', b'other')):
                artifact, receipt = self.make_archive(root, path, recorded=recorded)
                with self.subTest(path=path), self.assertRaises(ValueError):
                    audit_assets.source_export(artifact, receipt)

    def test_source_requires_independent_member_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            artifact, receipt = self.make_archive(Path(temporary), 'src/main.cpp')
            receipt.pop('members')
            with self.assertRaisesRegex(ValueError, 'Source export identity mismatch'):
                audit_assets.source_export(artifact, receipt)
            receipt['members'] = []
            with self.assertRaisesRegex(ValueError, 'Source export identity mismatch'):
                audit_assets.source_export(artifact, receipt)

    def test_delivered_catalog_must_equal_the_curated_source_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact, receipt = self.make_archive(root, 'tools/proton/notices/SOURCES.json', b'{"source":"original"}')
            export = audit_assets.source_export(artifact, receipt)
            catalog = root / 'proton-source-catalog.json'
            catalog.write_bytes(b'{"source":"original"}')
            audit_assets.verify_source_catalog(export, catalog)
            catalog.write_bytes(b'{"source":"substituted"}')
            with self.assertRaisesRegex(ValueError, 'differs from curated source'):
                audit_assets.verify_source_catalog(export, catalog)

    def test_forged_archive_and_receipt_cannot_substitute_committed_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'git'
            (root / 'src').mkdir(parents=True)
            (root / 'src/main.cpp').write_text('// committed source\n')
            def git(*args):
                return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
            git('init', '-q')
            git('add', 'src/main.cpp')
            git('-c', 'user.name=Audit fixture', '-c', 'user.email=audit@example.invalid',
                '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'fixture')
            identity = {'version': '26.1031.1-nightly', 'baseVersion': '26.1031.1',
                        'packageVersion': '26.1031.1~nightly', 'gitCommit': git('rev-parse', 'HEAD'),
                        'gitTree': git('rev-parse', 'HEAD^{tree}'), 'commitCount': 1,
                        'dirty': False, 'sourceProvenance': 'git'}
            artifact = Path(temporary) / 'source.tar.gz'
            receipt = audit_assets.source_archive.create_archive(root, artifact, identity)
            audit_assets.source_export(artifact, receipt, root)
            prefix = receipt['archiveRoot'] + '/'
            with tarfile.open(artifact) as tar:
                contents = {m.name: tar.extractfile(m).read() for m in tar if m.isfile()}
            changed = b'// substituted source\n'
            contents[prefix + 'src/main.cpp'] = changed
            info = json.loads(contents[prefix + 'source-info.json'])
            for item in info['files']:
                if item['path'] == 'src/main.cpp':
                    item.update(bytes=len(changed), sha256=hashlib.sha256(changed).hexdigest())
            contents[prefix + 'source-info.json'] = json.dumps(info).encode()
            receipt['members'] = info['files'] + [{'path': 'source-info.json', 'mode': '0644',
                'bytes': len(contents[prefix + 'source-info.json']),
                'sha256': hashlib.sha256(contents[prefix + 'source-info.json']).hexdigest()}]
            with tarfile.open(artifact, 'w:gz') as tar:
                for name, body in contents.items():
                    member = tarfile.TarInfo(name)
                    member.size = len(body)
                    tar.addfile(member, io.BytesIO(body))
            with self.assertRaisesRegex(ValueError, 'differs from committed Git source'):
                audit_assets.source_export(artifact, receipt, root)

    def test_tar_header_file_and_directory_modes_are_verified(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for target, mode in (('source/src/main.cpp', 0o755), ('source/src', 0o777)):
                artifact, receipt = self.make_archive(root, 'src/main.cpp')
                with tarfile.open(artifact) as tar:
                    entries = [(m, tar.extractfile(m).read() if m.isfile() else None) for m in tar]
                with tarfile.open(artifact, 'w:gz') as tar:
                    for member, body in entries:
                        if member.name == target:
                            member.mode = mode
                        tar.addfile(member, io.BytesIO(body) if body is not None else None)
                with self.subTest(target=target), self.assertRaises(ValueError):
                    audit_assets.source_export(artifact, receipt)

    def test_rebuild_archive_binding_and_recorded_legacy_parameters(self):
        receipt = {'artifact': {'file': 'source.tar.gz', 'sha256': 'a' * 64, 'bytes': 123},
                   'identity': {'gitCommit': 'b' * 40}}
        params = {'archive': receipt['artifact'], 'identity': receipt['identity'], 'bundleProton': True}
        bound = audit_assets.bind_rebuild_archive({'passed': True}, receipt, params)
        self.assertEqual(bound['archive'], receipt['artifact'])
        with self.assertRaises(ValueError):
            audit_assets.bind_rebuild_archive({'passed': True}, receipt)
        with self.assertRaises(ValueError):
            audit_assets.bind_rebuild_archive({'archive': {**receipt['artifact'], 'sha256': 'c' * 64}}, receipt)


class AdvisoryRanges(unittest.TestCase):
    def test_official_bun_advisory_catches_scanner_miss_and_fixed_boundary(self):
        advisory = {'ghsa_id': 'GHSA-test', 'severity': 'high', 'withdrawn_at': None,
                    'vulnerabilities': [{'package': {'name': 'bun'},
                        'vulnerable_version_range': '>= 1.0.21, < 1.4.4', 'patched_versions': '1.4.4'}]}
        with patch.object(audit_assets, 'run', return_value=json.dumps([advisory]) + '\n[]'):
            findings = audit_assets.bun_advisories({'1.4.2': ['appimage', 'debian13'], '1.4.4': ['fixed']})
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0]['packages'], ['appimage', 'debian13'])
        self.assertEqual(findings[0]['fixedIn'], '1.4.4')

    def test_fixed_boundary_and_union(self):
        self.assertTrue(audit_assets.version_in_range('1.4.2', '>= 1.0.21, < 1.4.4'))
        self.assertTrue(audit_assets.version_in_range('1.4.2', '>= 1.3.14, < 1.4.4'))
        self.assertFalse(audit_assets.version_in_range('1.4.4', '>=1.0.21, <1.4.4'))
        self.assertTrue(audit_assets.version_in_range('1.4.2', '<1.0.0 || >=1.3.14 <1.4.4'))
        with self.assertRaises(ValueError):
            audit_assets.version_in_range('1.4.2', '^1.0')



class ProtonCiAudit(unittest.TestCase):
    def test_ci_installer_runtime_identity_and_advisories_fail_closed(self):
        for scenario in ('fixed', 'vulnerable', 'wrong_actual_version', 'wrong_catalog'):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                directory = root / 'artifacts'
                directory.mkdir()
                (directory / 'putgd-test.deb').write_bytes(b'exact CI installer')
                source = root / 'tools/proton'
                (source / 'notices').mkdir(parents=True)
                lock = {'bunVersion': '1.4.4', 'bunSourceReference': 'b' * 40, 'bunSha256': 'c' * 64}
                (source / 'runtime-lock.json').write_text(json.dumps(lock))
                catalog = source / 'notices/SOURCES.json'
                catalog.write_text(json.dumps({'helperSha256': 'a' * 64}))
                proton = {'manifest': lock, 'runtimeSha256': 'a' * 64,
                          'sourceCatalogSha256': audit_assets.sha256(catalog),
                          'helper': 'helper'}
                if scenario == 'wrong_catalog':
                    proton['sourceCatalogSha256'] = 'd' * 64
                identity = {'bunVersion': '1.4.2' if scenario == 'wrong_actual_version' else '1.4.4'}
                def run(*args, **kwargs):
                    return json.dumps(identity) if args[-1] == '--version' else 'test-builder'
                findings = [{'ghsa_id': 'GHSA-test'}] if scenario == 'vulnerable' else []
                with patch.object(audit_assets, 'ROOT', root), \
                        patch.object(audit_assets, 'run', side_effect=run), \
                        patch.object(audit_assets.sbom, 'extract', return_value=root), \
                        patch.object(audit_assets, 'audit_proton', return_value=proton), \
                        patch.object(audit_assets, 'bun_advisories', return_value=findings):
                    result = audit_assets.audit_proton_ci(directory, root / 'audit')
                self.assertEqual(result['passed'], scenario == 'fixed')
                self.assertEqual(json.loads((root / 'audit/audit.json').read_text()), result)

    def test_no_installers_is_not_a_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(audit_assets, 'run', return_value='test-builder'):
                result = audit_assets.audit_proton_ci(root, root / 'audit')
            self.assertFalse(result['passed'])
            self.assertIn('No CI installers supplied', result['errors'])


if __name__ == '__main__':
    unittest.main()
