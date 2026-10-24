"""Safety/shape contracts only. Real compositor evidence is collected separately."""
import copy
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/lab'))
import native_acceptance as driver
import native_keychain


def fixture(db, count=5):
    with sqlite3.connect(db) as con:
        con.executescript('''
        PRAGMA user_version=7;
        CREATE TABLE history(id TEXT PRIMARY KEY,operation_id TEXT NOT NULL,item_id TEXT NOT NULL,
          created TEXT NOT NULL,name TEXT,mime TEXT,category TEXT,profile_id TEXT,upload_status TEXT,
          local_status TEXT,remote_status TEXT,data BLOB,publication_identity TEXT,UNIQUE(operation_id,item_id));
        CREATE TABLE operations(id TEXT PRIMARY KEY,data BLOB NOT NULL);
        CREATE TABLE item_state(id TEXT PRIMARY KEY,operation_id TEXT NOT NULL,remote_status TEXT,
          data BLOB,publication_identity TEXT,upload_status TEXT);
        ''')
        for i in range(count):
            item, op = f'item-{i}', f'op-{i}'
            created = f'2026-10-23T00:00:0{i}.000Z'
            artifact = {'id': item, 'name': f'capture-{i}.png', 'path': f'/fixture/artifacts/{op}/{item}',
                        'previewPath': f'/fixture/previews/{item}.png'}
            journal = {'id': op, 'state': 'failed' if i == 4 else 'completed', 'created': created,
                       'items': [{'artifact': artifact, 'uploadStatus': 'failed' if i == 4 else 'uploaded'}],
                       'settings': {'actions': {'desktop': {'destination': 'upload'}}}}
            record = dict(artifact, id=item, itemId=item, operationId=op, created=created)
            con.execute('INSERT INTO operations VALUES(?,?)', (op, json.dumps(journal).encode()))
            con.execute('INSERT INTO history VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)',
                        (item, op, item, created, artifact['name'], 'image/png', 'image', 'fixture',
                         'failed' if i == 4 else 'uploaded', 'kept', 'not_requested', json.dumps(record).encode(), ''))
            con.execute('INSERT INTO item_state VALUES(?,?,?,?,?,?)',
                        (item, op, 'not_requested', b'{"remoteStatus":"not_requested"}', '', 'uploaded'))
    db.chmod(0o600)


class HistoryShape(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.db = Path(self.temp.name) / 'history.sqlite'
        fixture(self.db)

    def test_ten_thousand_independent_journals_with_consistent_state(self):
        result = driver.grow_database(self.db, 10000)
        self.assertEqual((result['historyRows'], result['independentOperations'], result['operationJournals'], result['itemStates']),
                         (10000, 10000, 10000, 10000))
        with sqlite3.connect(self.db) as con:
            count = con.execute('''SELECT count(*) FROM history h
                JOIN operations o ON o.id=h.operation_id JOIN item_state s ON s.id=h.id AND s.operation_id=h.operation_id
                WHERE json_extract(o.data,'$.id')=h.operation_id
                AND json_extract(o.data,'$.items[0].artifact.id')=h.item_id
                AND json_array_length(o.data,'$.items')=1
                AND json_extract(h.data,'$.operationId')=h.operation_id
                AND json_extract(h.data,'$.itemId')=h.item_id
                AND json_extract(h.data,'$.created')=h.created
                AND json_extract(o.data,'$.created')=h.created''').fetchone()[0]
            self.assertEqual(count, 10000)
            self.assertEqual(con.execute('SELECT count(DISTINCT created) FROM history').fetchone()[0], 10000)
            self.assertLess(con.execute("SELECT max(created) FROM history WHERE id NOT LIKE 'item-%'").fetchone()[0], '2026-10-23T00:00:00.000Z')
            journal = json.loads(con.execute("SELECT data FROM operations WHERE id NOT LIKE 'op-%' LIMIT 1").fetchone()[0])
            self.assertEqual(journal['items'][0]['artifact']['path'], '/fixture/artifacts/op-0/item-0')
            self.assertEqual(journal['items'][0]['artifact']['previewPath'], '/fixture/previews/item-0.png')
            self.assertEqual(journal['settings']['actions']['desktop']['destination'], 'upload')

    def test_multi_item_seed_clones_only_matching_item(self):
        with sqlite3.connect(self.db) as con:
            journal = json.loads(con.execute("SELECT data FROM operations WHERE id='op-0'").fetchone()[0])
            journal['items'].append({'artifact': {'id': 'unrelated'}})
            con.execute("UPDATE operations SET data=? WHERE id='op-0'", (json.dumps(journal),))
        driver.grow_database(self.db, 6)
        with sqlite3.connect(self.db) as con:
            journal = json.loads(con.execute("SELECT data FROM operations WHERE id NOT LIKE 'op-%'").fetchone()[0])
            self.assertEqual(len(journal['items']), 1)

    def test_missing_journal_rolls_back_without_partial_clones(self):
        with sqlite3.connect(self.db) as con:
            con.execute("DELETE FROM operations WHERE id='op-4'")
        with self.assertRaises(driver.Refused): driver.grow_database(self.db, 10000)
        with sqlite3.connect(self.db) as con:
            self.assertEqual(con.execute('SELECT count(*) FROM history').fetchone()[0], 5)

    def test_active_operation_is_refused(self):
        with sqlite3.connect(self.db) as con:
            con.execute("UPDATE operations SET data=json_set(data,'$.state','running') WHERE id='op-1'")
        with self.assertRaises(driver.Refused): driver.grow_database(self.db, 10)

    def test_missing_database_snapshot_does_not_create_database(self):
        absent = self.db.with_name('purged.sqlite')
        self.assertEqual(driver.database_snapshot(absent), {'available': False})
        self.assertFalse(absent.exists())


class Safety(unittest.TestCase):
    def test_socket_limit_counts_utf8_after_resolution(self):
        self.assertTrue(str(driver.socket_path('/tmp/pg-495')).endswith('/putgd/session.sock'))
        with self.assertRaises(driver.Refused): driver.socket_path('/tmp/' + 'é' * 45)

    def test_private_login_input_no_read_symlink_or_credentials_file(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'password'; path.write_text('protected-login-must-never-appear'); path.chmod(0o600)
            with mock.patch.object(Path, 'read_text', side_effect=AssertionError('must not read')):
                self.assertEqual(driver.login_input(path), {'available': True, 'valueRead': False})
            link = path.with_name('link'); link.symlink_to(path)
            self.assertFalse(driver.login_input(link)['available'])
            path.chmod(0o644)
            self.assertFalse(driver.login_input(path)['available'])
            with self.assertRaises(driver.Refused): driver.login_input(path.with_name('credentials.txt'))

    def test_removed_cli_returns_structured_missing(self):
        with mock.patch.object(driver, 'command', side_effect=AssertionError('must not launch')):
            self.assertEqual(driver.cli('/does/not/exist', '/tmp/pg495', 'ui.snapshot', {}),
                             {'ok': False, 'error': {'code': 'cli_missing'}})

    def test_tray_target_requires_exact_owner_and_generation(self):
        identity = {'pid': 17, 'exe': '/usr/bin/putgd', 'startTicks': 1}
        def fake_bus(env, *args):
            if args[-1] == 'RegisteredStatusNotifierItems': return ['other/StatusNotifierItem', 'ours/StatusNotifierItem']
            if args[-1] == 'Title': return 'put.gd'
            if args[-1] == 'Menu': return '/Menu'
            raise AssertionError(args)
        with mock.patch.object(driver, 'same_process', return_value=True), mock.patch.object(driver, 'bus', side_effect=fake_bus), \
             mock.patch.object(driver, 'owner', side_effect=lambda e, s: (':1.7', 17 if s == 'ours' else 55)):
            target = driver.tray_target({}, identity)
            self.assertEqual((target['service'], target['path'], target['resident']), (':1.7', '/StatusNotifierItem', identity))
        with mock.patch.object(driver, 'same_process', return_value=False):
            with self.assertRaises(driver.Refused): driver.tray_target({}, identity)

    def test_no_matching_sni_never_falls_back_to_coordinates(self):
        with mock.patch.object(driver, 'same_process', return_value=True), mock.patch.object(driver, 'bus', return_value=[]):
            with self.assertRaisesRegex(driver.Refused, 'no coordinate fallback'):
                driver.tray_target({}, {'pid': 17})

    def test_quit_id_is_dynamic_and_must_be_unique_visible_enabled(self):
        def node(i, label, **props): return [i, {'label': {'data': label}, **{k: {'data': v} for k, v in props.items()}}, []]
        self.assertEqual(driver.quit_action([0, {}, [node(987, '&Quit'), node(12, 'Settings')]]), 987)
        for children in ([node(1, 'Quit', enabled=False)], [node(1, 'Quit'), node(2, 'Quit')], [node(1, 'Add a panel')]):
            with self.assertRaises(driver.Refused): driver.quit_action([0, {}, children])

    def test_session_selection_excludes_ssh_inactive_and_other_users(self):
        good = {'Id': 'new', 'User': str(os.getuid()), 'Active': 'yes', 'Remote': 'no', 'Type': 'wayland', 'Seat': 'seat0'}
        self.assertEqual(driver.choose_session([dict(good, Id='old', Active='no'), dict(good, Id='ssh', Type='tty'), good]), good)
        with self.assertRaises(driver.Refused): driver.choose_session([good, dict(good, Id='second')])

    def test_login_check_rejects_prior_shell_and_survivor(self):
        old = {'Id': 'old', 'TimestampMonotonic': '1000000', 'bootId': 'boot', 'shell': {'pid': 1}}
        before = {'prepared': True, 'oldSession': old}
        new = dict(old, Id='new', TimestampMonotonic='2000000', shell={'pid': 2})
        hz = os.sysconf('SC_CLK_TCK')
        self.assertTrue(driver.check_login(before, new, [{'startTicks': 3 * hz}])['newSessionVerified'])
        with self.assertRaises(driver.Refused): driver.check_login(before, old, [{'startTicks': 3 * hz}])
        with self.assertRaises(driver.Refused): driver.check_login(before, new, [{'startTicks': hz}])
        with self.assertRaises(driver.Refused): driver.check_login(before, new, [])
        self.assertTrue(driver.check_login(before, new, [], expected='absent')['newSessionVerified'])
        with self.assertRaises(driver.Refused): driver.check_login(before, new, [{'startTicks': 3 * hz}], expected='absent')

    def test_keychain_cleanup_reference_is_scoped(self):
        self.assertTrue(native_keychain.valid_reference('putgd-acceptance-12345678-1234-1234-1234-123456789abc'))
        for ref in ('putgd/proton/lab-f44', 'login', 'putgd-acceptance-/../../default'):
            self.assertFalse(native_keychain.valid_reference(ref))


if __name__ == '__main__':
    unittest.main()
