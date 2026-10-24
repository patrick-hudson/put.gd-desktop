#!/usr/bin/env python3
"""#379 safety/measurement contracts. No product, desktop or cloud access."""

import json
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'performance'))
import physical_campaign as campaign


def fixture(root, count=10000, independent=True):
    (root / 'config/putgd').mkdir(parents=True)
    (root / 'data/putgd').mkdir(parents=True)
    (root / 'archive').mkdir()
    (root / 'archive/image.png').write_bytes(b'synthetic-not-an-image')
    (root / 'config/putgd/config.json').write_text(json.dumps({
        'profiles': [], 'archive': {'root': str(root / 'archive')}}))
    db = root / 'data/putgd/history.sqlite'
    with sqlite3.connect(db) as connection:
        connection.executescript('CREATE TABLE history(id TEXT PRIMARY KEY,operation_id TEXT,item_id TEXT,category TEXT,data BLOB);'
                                 'CREATE TABLE operations(id TEXT PRIMARY KEY,data BLOB);'
                                 'CREATE TABLE item_state(id TEXT PRIMARY KEY,operation_id TEXT,data BLOB);')
        for i in range(count):
            op = 'op-' + str(i if independent else 0)
            value = json.dumps({'path': str(root / 'archive/image.png')}).encode()
            connection.execute('INSERT INTO history VALUES(?,?,?,?,?)', (str(i), op, 'item-' + str(i),
                                                                        'image' if i % 2 else 'video', value))
            connection.execute('INSERT OR IGNORE INTO operations VALUES(?,?)', (op, value))
            connection.execute('INSERT INTO item_state VALUES(?,?,?)', (str(i), op, value))
    db.chmod(0o600)
    root.chmod(0o700)
    return db


class FixtureSafetyTests(unittest.TestCase):
    def test_rejects_10000_rows_sharing_one_operation(self):
        with tempfile.TemporaryDirectory() as directory:
            db = fixture(Path(directory), independent=False)
            with self.assertRaisesRegex(campaign.EvidenceError, 'not_independent'):
                campaign.fixture_summary(db)

    def test_relocated_blob_journals_remain_blobs_and_source_is_immutable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, destination = root / 'source', root / 'copy'
            source.mkdir(); destination.mkdir()
            db = fixture(source)
            original = campaign.sha256(db)
            result = campaign.copy_fixture(source, destination)
            self.assertEqual(result['independentOperations'], 10000)
            self.assertEqual(campaign.sha256(db), original)
            with sqlite3.connect(destination / 'data/putgd/history.sqlite') as connection:
                for table in ('history', 'operations', 'item_state'):
                    value, kind = connection.execute(f'SELECT data,typeof(data) FROM {table} LIMIT 1').fetchone()
                    self.assertEqual(kind, 'blob')
                    self.assertEqual(json.loads(value)['path'], str(destination / 'archive/image.png'))

    def test_missing_item_state_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            db = fixture(Path(directory), count=2)
            with sqlite3.connect(db) as connection:
                connection.execute('DELETE FROM item_state WHERE id="1"')
            with self.assertRaisesRegex(campaign.EvidenceError, 'missing_journal_or_item_state'):
                campaign.fixture_summary(db, expected=2)

    def test_symlink_payload_cannot_escape_private_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, destination = root / 'source', root / 'copy'
            source.mkdir(); destination.mkdir()
            fixture(source)
            (source / 'archive/link').symlink_to('/etc/passwd')
            with self.assertRaisesRegex(campaign.EvidenceError, 'link_or_special'):
                campaign.copy_fixture(source, destination)

    def test_live_wal_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            db = fixture(source, count=2)
            Path(str(db) + '-wal').touch()
            with self.assertRaisesRegex(campaign.EvidenceError, 'wal_checkpoint'):
                campaign.copy_fixture(source, source / 'new')


class EndpointAndBudgetTests(unittest.TestCase):
    def test_only_literal_loopback_http_port_is_allowed(self):
        self.assertTrue(campaign.loopback_endpoint('http://127.0.0.1:19237'))
        for endpoint in ('https://s3.amazonaws.com', 'http://localhost:12', 'http://127.1:12',
                         'http://127.0.0.1.evil:12', 'http://127.0.0.1:12@evil:23',
                         'http://user:secret@127.0.0.1:12', 'http://127.0.0.1:12/path',
                         'http://127.0.0.1:12/?url=evil', 'http://127.0.0.1', 'http://127.0.0.1:0'):
            with self.subTest(endpoint=endpoint):
                self.assertFalse(campaign.loopback_endpoint(endpoint))

    def test_rejects_real_destination_before_app_launch(self):
        with self.assertRaisesRegex(campaign.EvidenceError, 'unsafe_fixture_endpoint'):
            campaign.safe_document({'profiles': [{'provider': 's3', 'settings': {'endpoint': 'https://example.com'}}]})

    def test_memory_enforces_40_not_legacy_100_and_300_seconds(self):
        metrics = {'seconds': 300, 'residentCpuPercentOneCore': .49,
                   'peakFamilyRssBytes': 41 * campaign.MIB, 'peakFamilyPssBytes': 28 * campaign.MIB}
        result = campaign.memory_gates(metrics)
        self.assertEqual(result['cpu']['status'], 'pass')
        self.assertEqual(result['rss']['status'], 'fail')
        self.assertEqual(result['pss']['status'], 'pass')
        for cpu, seconds in ((.5, 300), (.01, 299.99)):
            self.assertEqual(campaign.memory_gates({**metrics, 'seconds': seconds,
                                                   'residentCpuPercentOneCore': cpu})['cpu']['status'], 'fail')

    def test_output_collision_is_never_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            args = campaign.parser().parse_args(['memory', '--output', directory, '--package', 'x',
                '--package-sha256', 'a' * 64, '--version', '26.1024.1185-nightly',
                '--source-revision', 'b' * 40, '--desktop-session', '2', '--lock-session', 'owner'])
            with self.assertRaisesRegex(campaign.EvidenceError, 'output_must_be_new'):
                campaign.Campaign(args)

    def test_overlong_complete_socket_rejected_before_physical_or_product_access(self):
        with tempfile.TemporaryDirectory() as directory:
            output = str(Path(directory) / ('a' * 80))
            args = campaign.parser().parse_args(['memory', '--output', output, '--package', 'x',
                '--package-sha256', 'a' * 64, '--version', '26.1024.1185-nightly',
                '--source-revision', 'b' * 40, '--desktop-session', '2', '--lock-session', 'owner'])
            observer = campaign.Campaign(args)
            with mock.patch.object(campaign, 'physical_identity') as physical, \
                 mock.patch.object(campaign, 'verify_package') as package:
                with self.assertRaisesRegex(campaign.EvidenceError, 'socket_path_too_long'):
                    observer.initialize()
                physical.assert_not_called()
                package.assert_not_called()
            result = json.loads((Path(output) / 'results.json').read_text())
            self.assertFalse(result['socketPreflight']['passed'])
            self.assertEqual(result['socketPreflight']['paths'][0]['path'], output + '/runtime/putgd/session.sock')


if __name__ == '__main__':
    unittest.main()
