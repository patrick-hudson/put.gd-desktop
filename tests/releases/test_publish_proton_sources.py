"""Proton source-only publication must not enter the stable download channel."""
import copy
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import publish_proton_sources as publisher


class Publication(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.commit = 'a' * 40
        self.tag = 'proton-sources-0.23.0-crypto2.2.0-bun1.4.4'
        self.parts = []
        for name in publisher.PART_NAMES:
            data = name.encode()
            (self.root / name).write_bytes(data)
            self.parts.append({'file': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        (self.root / 'SHA256SUMS').write_text(''.join(p['sha256'] + '  ' + p['file'] + '\n' for p in self.parts))
        self.catalog = {'publication': {'releaseUrl': 'https://github.com/owner/repo/releases/tag/' + self.tag,
                                        'sourceTagCommit': self.commit},
                        'delivery': {'parts': self.parts}}
        self.metadata = None
        self.repository = 'owner/repo'
        self.commands = []

    def json(self, path, **kwargs):
        if path.startswith('releases?'):
            return [[copy.deepcopy(self.metadata)] if self.metadata else []]
        if path == 'releases/42':
            return copy.deepcopy(self.metadata)
        self.fail('Unexpected API path: ' + path)

    def run_command(self, command, **kwargs):
        self.commands.append(command)
        if command[2] == 'create':
            self.metadata = {'id': 42, 'tag_name': self.tag, 'target_commitish': self.commit,
                             'draft': True, 'prerelease': True, 'assets': []}
        elif command[2] == 'upload':
            for path in map(Path, command[6:]):
                self.metadata['assets'].append({'id': len(self.metadata['assets']) + 1, 'name': path.name,
                    'size': path.stat().st_size, 'state': 'uploaded',
                    'digest': 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()})
        elif command[2] == 'edit':
            self.metadata['draft'] = False
        else:
            self.fail('Unexpected command: ' + repr(command))

    def publish(self, *, tag_present=False, tag_commit=None):
        with patch.object(publisher.subprocess, 'run', side_effect=self.run_command), \
                patch.object(publisher, 'tag_exists', return_value=tag_present), \
                patch.object(publisher, 'remote_tag_commit', return_value=tag_commit or self.commit):
            return publisher.publish(self, self.catalog, self.root, self.commit, self.root / 'notes.md')

    def test_create_and_publication_both_force_prerelease_and_never_latest(self):
        result = self.publish()
        self.assertTrue(result['published'])
        for operation in ('create', 'edit'):
            command = next(c for c in self.commands if c[2] == operation)
            self.assertIn('--prerelease', command)
            self.assertIn('--latest=false', command)
        self.assertFalse(result['latestRequested'])

    def test_existing_full_release_or_draft_is_rejected_before_mutation(self):
        for draft in (True, False):
            self.metadata = {'id': 42, 'tag_name': self.tag, 'draft': draft, 'prerelease': False, 'assets': []}
            with self.subTest(draft=draft), self.assertRaisesRegex(ValueError, 'prerelease'):
                self.publish()
            self.assertEqual(self.commands, [])

    def test_corrupt_local_part_cannot_be_published(self):
        (self.root / self.parts[0]['file']).write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'differs from catalog'):
            self.publish()
        self.assertEqual(self.commands, [])

    def test_wrong_valid_commit_is_rejected_before_mutation(self):
        self.catalog['publication']['sourceTagCommit'] = 'b' * 40
        with self.assertRaisesRegex(ValueError, 'commit differs'):
            self.publish()
        self.assertEqual(self.commands, [])

    def test_incorrect_checksum_index_is_rejected_before_mutation(self):
        path = self.root / 'SHA256SUMS'
        valid = path.read_text()
        for bad in ('unrelated text\n', valid.replace(self.parts[0]['sha256'], 'b' * 64),
                    valid.splitlines()[0] + '\n', valid + valid.splitlines()[0] + '\n'):
            path.write_text(bad)
            with self.subTest(index=bad), self.assertRaisesRegex(ValueError, 'checksum index'):
                self.publish()
            self.assertEqual(self.commands, [])

    def test_existing_wrong_tag_is_rejected_before_any_mutation(self):
        with self.assertRaisesRegex(ValueError, 'tag targets another commit'):
            self.publish(tag_present=True, tag_commit='b' * 40)
        self.assertEqual(self.commands, [])

    def test_tag_race_is_rejected_before_publication(self):
        with patch.object(publisher.subprocess, 'run', side_effect=self.run_command), \
                patch.object(publisher, 'tag_exists', side_effect=[False, True]), \
                patch.object(publisher, 'remote_tag_commit', return_value='b' * 40):
            with self.assertRaisesRegex(ValueError, 'tag targets another commit'):
                publisher.publish(self, self.catalog, self.root, self.commit, self.root / 'notes.md')
        self.assertTrue(self.metadata['draft'])
        self.assertFalse(any(command[2] == 'edit' for command in self.commands))


if __name__ == '__main__':
    unittest.main()
