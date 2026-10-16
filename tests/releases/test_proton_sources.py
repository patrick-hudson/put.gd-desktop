"""Corresponding-source transport verifies archive bytes without extracting them."""
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import proton_sources


class Sources(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgps-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bundle = self.root / 'transport.tar.gz'
        self.catalog = self.root / 'catalog.json'
        self.output = self.root / 'output'
        self.members = {'helper.tar.gz': b'preferred helper source', 'bun.tar': b'preferred Bun source'}
        self.write_fixture()

    def write_fixture(self, tar_members=None):
        with tarfile.open(self.bundle, 'w:gz') as archive:
            for name, contents in (self.members if tar_members is None else tar_members).items():
                info = tarfile.TarInfo(name)
                info.size = len(contents)
                archive.addfile(info, io.BytesIO(contents))
        self.value = {'schemaVersion': 1, 'materialsReady': True, 'published': True,
            'helperSha256': 'a' * 64,
            'artifacts': [{'filename': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, data in self.members.items()],
            'delivery': {'file': proton_sources.BUNDLE_NAME, 'bytes': self.bundle.stat().st_size,
                'sha256': hashlib.sha256(self.bundle.read_bytes()).hexdigest(),
                'parts': []}}
        self.update_parts()
        self.catalog.write_text(json.dumps(self.value))

    def update_parts(self):
        data = self.bundle.read_bytes()
        split = len(data) // 2
        self.value['delivery']['parts'] = [{'file': name, 'bytes': len(part),
            'sha256': hashlib.sha256(part).hexdigest(), 'url': 'https://example.invalid/' + name}
            for name, part in zip(proton_sources.PART_NAMES, (data[:split], data[split:]))]

    def test_prepares_exact_bundle_catalog_and_receipt(self):
        result = proton_sources.prepare(self.catalog, self.output, self.bundle)
        self.assertEqual(b''.join((self.output / name).read_bytes() for name in proton_sources.PART_NAMES), self.bundle.read_bytes())
        self.assertEqual((self.output / proton_sources.CATALOG_NAME).read_bytes(), self.catalog.read_bytes())
        self.assertEqual(result['helperSha256'], 'a' * 64)
        self.assertEqual(result['sourceCatalogSha256'], hashlib.sha256(self.catalog.read_bytes()).hexdigest())
        self.assertEqual(result, json.loads((self.output / 'proton-source-delivery.json').read_text()))
        self.assertFalse((self.output / 'helper.tar.gz').exists())

    def test_downloads_pinned_transport_parts(self):
        data = self.bundle.read_bytes()
        split = len(data) // 2
        with patch.object(proton_sources, 'urlopen', side_effect=[io.BytesIO(data[:split]), io.BytesIO(data[split:])]) as download:
            proton_sources.prepare(self.catalog, self.output)
        self.assertEqual([call.args[0] for call in download.call_args_list],
                         [part['url'] for part in self.value['delivery']['parts']])
        self.assertEqual(b''.join((self.output / name).read_bytes() for name in proton_sources.PART_NAMES), data)

    def test_corrupt_outer_hash_rejected_before_output(self):
        self.bundle.write_bytes(self.bundle.read_bytes() + b'changed')
        with self.assertRaisesRegex(ValueError, 'bundle bytes'):
            proton_sources.prepare(self.catalog, self.output, self.bundle)
        self.assertFalse(self.output.exists())

    def test_inner_hash_and_missing_or_extra_members_rejected_before_output(self):
        for members in ({'helper.tar.gz': b'wrong source', 'bun.tar': self.members['bun.tar']},
                        {'helper.tar.gz': self.members['helper.tar.gz']},
                        {**self.members, 'unlisted.tar': b'extra'},
                        {'../helper.tar.gz': b'bad', 'bun.tar': self.members['bun.tar']}):
            with self.subTest(members=list(members)):
                self.write_fixture(members)
                with self.assertRaises(ValueError):
                    proton_sources.prepare(self.catalog, self.output, self.bundle)
                self.assertFalse(self.output.exists())

    def test_same_length_inner_hash_rejected(self):
        changed = dict(self.members)
        changed['helper.tar.gz'] = b'X' * len(self.members['helper.tar.gz'])
        self.write_fixture(changed)
        with self.assertRaisesRegex(ValueError, 'member bytes'):
            proton_sources.prepare(self.catalog, self.output, self.bundle)
        self.assertFalse(self.output.exists())

    def test_link_archive_member_rejected(self):
        with tarfile.open(self.bundle, 'w:gz') as archive:
            info = tarfile.TarInfo('helper.tar.gz')
            info.type = tarfile.SYMTYPE
            info.linkname = '/etc/passwd'
            archive.addfile(info)
        self.value['delivery'].update(bytes=self.bundle.stat().st_size,
            sha256=hashlib.sha256(self.bundle.read_bytes()).hexdigest())
        self.update_parts()
        self.catalog.write_text(json.dumps(self.value))
        with self.assertRaisesRegex(ValueError, 'member'):
            proton_sources.prepare(self.catalog, self.output, self.bundle)
        self.assertFalse(self.output.exists())

    def test_duplicate_catalog_member_rejected(self):
        self.value['artifacts'].append(self.value['artifacts'][0])
        self.catalog.write_text(json.dumps(self.value))
        with self.assertRaisesRegex(ValueError, 'catalog'):
            proton_sources.prepare(self.catalog, self.output, self.bundle)
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
