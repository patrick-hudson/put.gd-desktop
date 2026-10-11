#!/usr/bin/env python3
"""Standard-library unit checks; synthetic Python children, no app or GUI launch."""

import json
import os
from pathlib import Path
import subprocess
import socket
import tempfile
import sys
import unittest
from unittest import mock

import bench_ui_process as bench


PARENT = r'''
import json, subprocess, sys
payload = bytearray(4 * 1024 * 1024)
children = []
def spawn():
    child = subprocess.Popen([sys.executable, '-c',
        'import sys; payload=bytearray(12*1024*1024); print("ready",flush=True); sys.stdin.readline()',
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert child.stdout.readline().strip() == 'ready'
    children.append(child)
    print(json.dumps({'child': child.pid}), flush=True)
try:
    spawn()
    for command in sys.stdin:
        if command.strip() == 'stop':
            child = children.pop()
            child.stdin.write('\n'); child.stdin.flush(); child.wait(timeout=5)
            print('stopped', flush=True)
        elif command.strip() == 'spawn':
            spawn()
        else:
            break
finally:
    for child in children:
        child.stdin.write('\n'); child.stdin.flush(); child.wait(timeout=5)
'''


class ProcessFamilyTests(unittest.TestCase):
    def setUp(self):
        self.parent = subprocess.Popen([sys.executable, '-u', '-c', PARENT],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
        self.first_child = json.loads(self.parent.stdout.readline())['child']
        self.app = Path(sys.executable).resolve()

    def tearDown(self):
        if self.parent.poll() is None:
            self.parent.stdin.write('quit\n')
            self.parent.stdin.flush()
        self.parent.communicate(timeout=10)
        self.assertEqual(self.parent.returncode, 0)

    def family(self):
        return bench.ProcessFamily(self.parent.pid, self.app, self.app,
                                   bench.identity(self.parent.pid)['startTicks'])

    def test_totals_include_resident_and_live_child(self):
        family = self.family()
        row = family.sample()
        self.assertEqual(row['processCount'], 2)
        self.assertEqual(row['uiProcessCount'], 1)
        self.assertEqual({p['pid'] for p in row['processes']}, {self.parent.pid, self.first_child})
        for metric in bench.METRICS:
            self.assertEqual(row['totals'][metric], sum(p[metric] for p in row['processes']))
        self.assertGreater(row['totals']['rssBytes'],
                           next(p['rssBytes'] for p in row['processes'] if p['role'] == 'resident'))
        self.assertTrue(all(p['sha256'] == bench.sha256(self.app) for p in row['processes']))
        self.assertTrue(all(p['inResidentTree'] for p in row['processes']))

    def test_normal_exit_and_reopen_keep_distinct_process_identities(self):
        family = self.family()
        initial = family.sample()
        self.parent.stdin.write('stop\n'); self.parent.stdin.flush()
        self.assertEqual(self.parent.stdout.readline().strip(), 'stopped')
        closed = family.sample()
        self.assertEqual(closed['processCount'], 1)
        self.assertEqual(closed['uiProcessCount'], 0)
        self.assertEqual([p['pid'] for p in family.exited], [self.first_child])
        self.parent.stdin.write('spawn\n'); self.parent.stdin.flush()
        second_child = json.loads(self.parent.stdout.readline())['child']
        reopened = family.sample()
        self.assertEqual(reopened['processCount'], 2)
        old_identity = next(bench.identity_key(p) for p in initial['processes'] if p['role'] == 'ui')
        new_identity = next(bench.identity_key(p) for p in reopened['processes'] if p['role'] == 'ui')
        self.assertNotEqual(old_identity, new_identity)
        self.assertIn(second_child, {p['pid'] for p in reopened['processes']})

    def test_existing_process_mode_writes_complete_totals_without_interaction(self):
        with tempfile.TemporaryDirectory() as directory:
            args = bench.parser().parse_args([
                '--mode', 'split', '--pid', str(self.parent.pid),
                '--start-ticks', str(bench.identity(self.parent.pid)['startTicks']),
                '--app', str(self.app), '--helper', str(self.app),
                '--source-revision', 'a' * 40, '--output', str(Path(directory) / 'run')])
            recorder = bench.Recorder(args)
            with mock.patch.object(bench, 'system_metadata', return_value={}), \
                 mock.patch.object(bench, 'version', return_value='0.1.1-dev.0+gaaaaaaaaaaaa'):
                recorder.initialize(args)
            self.assertEqual(bench.observe_existing(args, recorder), 0)
            result = json.loads((recorder.root / 'results.json').read_text())
            samples = [json.loads(line) for line in (recorder.root / 'samples.jsonl').read_text().splitlines()]
            self.assertTrue(result['passed'])
            self.assertEqual(len(samples), 1)
            self.assertEqual(samples[0]['processCount'], 2)
            self.assertEqual(result['summary']['finalTotals'], samples[0]['totals'])
            self.assertFalse(result['summary']['cold40MiBTargetEstablished'])
            self.assertFalse(result['summary']['renderedTrayAcceptanceEstablished'])
            self.assertIsNone(self.parent.poll())

    def test_wrong_start_time_or_executable_fails(self):
        with self.assertRaisesRegex(bench.EvidenceError, 'start_time_mismatch'):
            bench.ProcessFamily(self.parent.pid, self.app, start_ticks=0)
        with self.assertRaisesRegex(bench.EvidenceError, 'executable_mismatch'):
            bench.ProcessFamily(self.parent.pid, '/bin/true')

    def test_live_identity_change_is_not_counted_as_memory_saving(self):
        family = self.family()
        original = bench.identity
        def changed(pid):
            value = original(pid)
            if pid == self.parent.pid:
                value['startTicks'] += 1
            return value
        with mock.patch.object(bench, 'identity', side_effect=changed):
            with self.assertRaisesRegex(bench.EvidenceError, 'resident_identity_changed'):
                family.sample()

    def test_tracked_child_outside_tree_remains_in_total(self):
        family = self.family()
        family.sample()
        # Simulate reparenting after observation without creating a real orphan.
        with mock.patch.object(bench, 'children', return_value=set()):
            row = family.sample()
        self.assertEqual(row['processCount'], 2)
        self.assertEqual(row['escapedProcessCount'], 1)
        self.assertTrue(any(p['pid'] == self.first_child and not p['inResidentTree']
                            for p in row['processes']))


class ReparentedDiscoveryTests(unittest.TestCase):
    """Model process topology changes without orphaning or recycling real PIDs."""

    def setUp(self):
        self.identities = {
            pid: {'pid': pid, 'startTicks': pid * 100, 'uid': os.getuid(),
                  'executable': f'/synthetic/process-{pid}', 'device': 1, 'inode': pid}
            for pid in range(1, 5)
        }
        self.family = bench.ProcessFamily.__new__(bench.ProcessFamily)
        self.family.root = dict(self.identities[1])
        self.family.helper = None
        self.family.expected_hashes = {}
        self.family.known = {
            bench.identity_key(self.identities[pid]): {
                **self.identities[pid], 'sha256': 'digest',
                'role': 'resident' if pid == 1 else 'auxiliary'}
            for pid in (1, 2)
        }
        self.family.exited = []
        # Child 2 was observed earlier but now belongs to another process tree.
        # Child 4 still belongs to the resident; child 3 was born after 2 escaped.
        self.links = {1: {4}, 2: {3}, 3: set(), 4: set()}
        self.children = mock.Mock(side_effect=lambda pid: self.links[pid])
        for patcher in (
            mock.patch.object(bench, 'identity', side_effect=lambda pid: dict(self.identities[pid])),
            mock.patch.object(bench, 'children', self.children),
            mock.patch.object(bench, 'sha256', return_value='digest'),
            mock.patch.object(bench, 'memory', side_effect=lambda pid: {
                'rssBytes': pid * 100, 'pssBytes': pid * 50, 'privateBytes': pid * 25}),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_escaped_childs_new_worker_is_counted_but_not_in_resident_tree(self):
        row = self.family.sample()
        self.assertEqual(row['processCount'], 4)
        self.assertEqual(row['escapedProcessCount'], 2)
        self.assertEqual(row['totals'], {'rssBytes': 1000, 'pssBytes': 500, 'privateBytes': 250})
        self.assertEqual({p['pid']: p['inResidentTree'] for p in row['processes']},
                         {1: True, 2: False, 3: False, 4: True})
        self.assertIn(mock.call(2), self.children.call_args_list)
        self.assertIn(mock.call(3), self.children.call_args_list)

    def test_reused_escaped_pid_is_not_a_discovery_root(self):
        old_identity = dict(self.family.known[(2, 200)])
        self.identities[2]['startTicks'] += 1
        row = self.family.sample()
        self.assertEqual({p['pid'] for p in row['processes']}, {1, 4})
        self.assertEqual(row['escapedProcessCount'], 0)
        self.assertNotIn(mock.call(2), self.children.call_args_list)
        self.assertNotIn(mock.call(3), self.children.call_args_list)
        self.assertEqual(self.family.exited, [old_identity])

    def test_pid_reuse_during_child_lookup_does_not_adopt_unrelated_workers(self):
        def changed_during_lookup(pid):
            if pid == 2:
                self.identities[2]['startTicks'] += 1
            return self.links[pid]
        self.children.side_effect = changed_during_lookup
        row = self.family.sample()
        self.assertEqual({p['pid'] for p in row['processes']}, {1, 4})
        self.assertEqual(row['escapedProcessCount'], 0)
        self.assertNotIn(mock.call(3), self.children.call_args_list)
        self.assertEqual([p['pid'] for p in self.family.exited], [2])


class WindowAndContractTests(unittest.TestCase):
    def test_only_verified_pids_are_selected_and_duplicates_removed(self):
        lookup = mock.Mock(side_effect=lambda pid: {11: ['0x1', '0x2'], 22: ['0x3']}[pid])
        self.assertEqual(bench.selected_windows([11, 22, 11], lookup), ['0x1', '0x2', '0x3'])
        self.assertEqual(lookup.call_args_list, [mock.call(11), mock.call(22)])

    def test_socket_peer_matches_the_explicit_resident_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / 'session.sock')
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(path)
                listener.listen(2)
                family = mock.Mock(root=bench.identity(os.getpid()))
                bench.verify_socket_owner(path, family)
                family.root = {**family.root, 'pid': family.root['pid'] + 1}
                with self.assertRaisesRegex(bench.EvidenceError, 'session_socket_owner_mismatch'):
                    bench.verify_socket_owner(path, family)

    def test_initialization_failure_retains_a_structured_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'new-run'
            arguments = ['bench_ui_process.py', '--mode', 'retained', '--pid', str(os.getpid()),
                         '--start-ticks', str(bench.identity(os.getpid())['startTicks']),
                         '--app', '/bin/true', '--source-revision', 'a' * 40, '--output', str(output)]
            with mock.patch.object(sys, 'argv', arguments), \
                 mock.patch.object(bench, 'system_metadata', return_value={}), \
                 mock.patch.object(bench, 'version', side_effect=bench.EvidenceError('test_version_mismatch')):
                self.assertEqual(bench.main(), 2)
            result = json.loads((output / 'results.json').read_text())
            self.assertFalse(result['passed'])
            self.assertEqual(result['status'], 'failed-or-interrupted')
            self.assertEqual(result['sampleCount'], 0)
            self.assertEqual(result['errorCode'], 'test_version_mismatch')
            self.assertEqual(output.stat().st_mode & 0o777, 0o700)
            self.assertEqual((output / 'results.json').stat().st_mode & 0o777, 0o600)

    def test_stat_parser_accepts_parentheses_and_spaces_in_comm(self):
        fields = ['S', '7'] + ['0'] * 22
        fields[11], fields[12], fields[19] = '13', '17', '12345'
        value = bench.stat_fields('9 (worker ) name) ' + ' '.join(fields))
        self.assertEqual(value, {'state': 'S', 'ppid': 7, 'cpuTicks': 30, 'startTicks': 12345})

    def test_closed_split_rejects_live_helper_even_if_diagnostics_claim_zero(self):
        args = type('Args', (), {'mode': 'split'})()
        family = mock.Mock()
        family.sample.return_value = {'escapedProcessCount': 0, 'processes': [{'role': 'ui', 'pid': 8}]}
        with self.assertRaisesRegex(bench.EvidenceError, 'closed_ui_process_still_present'):
            bench.checked_helper(args, family, {'ui': {'pid': 0}}, closed=True)

    def test_open_split_requires_actual_helper_and_matching_ready_ack(self):
        args = type('Args', (), {'mode': 'split'})()
        family = mock.Mock()
        family.sample.return_value = {'escapedProcessCount': 0, 'processes': [{'role': 'ui', 'pid': 8}]}
        self.assertEqual(bench.checked_helper(args, family, {'ui': {'pid': 8, 'state': 'ready'}}),
                         family.sample.return_value)
        with self.assertRaisesRegex(bench.EvidenceError, 'ui_diagnostics_process_mismatch'):
            bench.checked_helper(args, family, {'ui': {'pid': 9, 'state': 'ready'}})


if __name__ == '__main__':
    unittest.main()
