"""Contracts of the S3 differential harness (tools/lab/s3_differential.py): what it compares and what its cleanup promises.

No network, no resident: an in-memory target stands in for a server."""
import importlib.util
from pathlib import Path
import sys
import unittest

LAB = Path(__file__).resolve().parents[2] / 'tools/lab'
sys.path.insert(0, str(LAB))
spec = importlib.util.spec_from_file_location('s3_differential', LAB / 's3_differential.py')
diff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diff)

UUID_A = '1080d312-9246-46e6-9889-63809dcda31c'
UUID_B = 'f983db5e-502d-486c-9ec1-ef1d8b21568a'


def result(prefix, uuid, status='removed', head=200, key_uuid=None, lost_phase='done'):
    """A case record as one server produced it; only the run prefix, the generated UUID and the stated outcomes vary."""
    key = prefix + 'dup-rename-%s.txt' % uuid
    deletion = {'state': 'completed', 'cancelled': False, 'id': 'id-' + uuid, 'created': 'time-' + uuid, 'acknowledgement': 'ack-' + uuid,
                'items': [{'status': status, 'historyId': 'h-' + uuid, 'error': {'code': '', 'details': {}}, 'progress': {'phase': 'done'}}]}
    lost = {'state': 'completed', 'id': 'l-' + uuid, 'items': [{'status': 'unknown', 'error': {'code': 's3_transport_unknown', 'details': {'requestStage': 'delete_object', 'curlCode': 52}},
                                                                 'progress': {'phase': 'object_unknown'}}]}
    retried = {'state': 'completed', 'id': 'l-' + uuid, 'items': [{'status': 'removed', 'error': {'code': ''}, 'progress': {'phase': lost_phase}}]}
    return {'prefix': prefix, 'case': 'x',
            'ops': [{'state': 'completed', 'error': None,
                     'items': [{'name': 'dup-rename.txt', 'uploadStatus': 'uploaded', 'errorCode': '', 'requestStage': None, 'key': key_uuid or key, 'directUrl': 'http://x/' + key}]}],
            'stored': [{'key': key, 'size': 5, 'sha256': 'ab' * 32}],
            'uploads': [], 'violations': [],
            'deletions': {'normal.txt': {'prepare': 'ok', 'final': deletion}},
            'lost_delete': lost, 'lost_delete_retry': {'ok': True, 'error': {'code': ''}, 'final': retried},
            'receipt_key_head': {'dir//': {'receiptKey': 'dir//p2.txt', 'head': head}}}


class Signature(unittest.TestCase):
    def test_generated_suffixes_and_run_identities_do_not_count_as_differences(self):
        first = diff.signature(result('d/run1/x/', UUID_A))
        second = diff.signature(result('acceptance/s3diff-run2/run2/x/', UUID_B))
        self.assertEqual(first, second)
        flat = repr(first)
        self.assertNotIn(UUID_A, flat)
        self.assertIn('dup-rename-<uuid>.txt', flat)

    def test_a_receipt_that_names_another_object_than_the_stored_one_differs(self):
        good = diff.signature(result('d/r/x/', UUID_A))
        wrong = diff.signature(result('d/r/x/', UUID_A, key_uuid='d/r/x/dup-rename-%s.txt' % UUID_B))
        self.assertNotEqual(good, wrong)

    def test_deletion_outcomes_are_compared(self):
        self.assertNotEqual(diff.signature(result('d/r/x/', UUID_A, status='removed')), diff.signature(result('d/r/x/', UUID_A, status='unknown')))

    def test_lost_delete_retry_is_compared(self):
        self.assertNotEqual(diff.signature(result('d/r/x/', UUID_A, lost_phase='done')), diff.signature(result('d/r/x/', UUID_A, lost_phase='object_unknown')))

    def test_receipt_key_head_results_are_compared(self):
        self.assertNotEqual(diff.signature(result('d/r/x/', UUID_A, head=200)), diff.signature(result('d/r/x/', UUID_A, head=404)))

    def test_the_leading_slash_shape_does_not_carry_the_run_prefix(self):
        def shaped(prefix):
            record = result(prefix, UUID_A)
            record['receipt_key_head'] = {'^/' + prefix + 'ls/': {'receiptKey': '/ls/p13.txt', 'head': 404}}
            return record
        self.assertEqual(diff.signature(shaped('d/r1/x/')), diff.signature(shaped('acceptance/s3diff-r2/x/')))

    def test_the_leading_slash_namespace_stays_distinct_from_the_case_prefix(self):
        self.assertEqual(diff.relative('d/r/x/', 'd/r/x/ls/p.txt'), 'ls/p.txt')
        self.assertEqual(diff.relative('d/r/x/', '/d/r/x/ls/p.txt'), '/ls/p.txt')


class Memory(diff.Target):
    """A server in memory. Keys can be made undeletable and aborts can be made to fail."""
    name, cloud = 'memory', False

    def __init__(self, keys=(), uploads=(), delete_status=204, abort_status=204):
        self.keys = {k: 1 for k in keys}
        self.open = {(k, u) for k, u in uploads}
        self.delete_status, self.abort_status = delete_status, abort_status

    def raw_keys_in(self, prefix):
        return [(k, 1, 'e') for k in sorted(self.keys) if k.startswith(prefix)]

    def stored_in(self, prefix):
        return [{'key': k, 'size': 1} for k in sorted(self.keys) if k.startswith(prefix)]

    def uploads_in(self, prefix):
        return sorted(item for item in self.open if item[0].startswith(prefix))

    def remove(self, key):
        if self.delete_status in (200, 204):
            self.keys.pop(key, None)
        return self.delete_status

    def abort(self, key, upload):
        if self.abort_status in (200, 204, 404):
            self.open.discard((key, upload))
        return self.abort_status


class Cleanup(unittest.TestCase):
    def test_it_covers_the_leading_slash_namespace(self):
        target = Memory(keys=['d/r/x/a.txt', '/d/r/x/ls/b.txt', 'd/r/other/c.txt'], uploads=[('/d/r/x/ls/big', 'u1')])
        self.assertEqual(len(target.stored('d/r/x/')), 2)
        target.cleanup('d/r/x/')
        self.assertEqual(sorted(target.keys), ['d/r/other/c.txt'])
        self.assertEqual(target.open, set())

    def test_a_refused_abort_fails_the_cleanup(self):
        target = Memory(uploads=[('d/r/x/big', 'u1')], abort_status=403)
        with self.assertRaisesRegex(RuntimeError, 'abort'):
            target.cleanup('d/r/x/')

    def test_a_refused_delete_fails_the_cleanup(self):
        target = Memory(keys=['d/r/x/a.txt'], delete_status=403)
        with self.assertRaisesRegex(RuntimeError, 'delete'):
            target.cleanup('d/r/x/')

    def test_an_upload_that_is_still_listed_after_a_quiet_abort_fails_the_cleanup(self):
        class Liar(Memory):
            def abort(self, key, upload):
                return 204  # answers success, removes nothing
        target = Liar(uploads=[('d/r/x/big', 'u1')])
        with self.assertRaisesRegex(RuntimeError, 'still-incomplete'):
            target.cleanup('d/r/x/')

    def test_an_upload_listing_the_server_refuses_fails_the_cleanup(self):
        class Unlisted(Memory):
            def uploads_in(self, prefix):
                return None
        with self.assertRaisesRegex(RuntimeError, 'uploads-unlisted'):
            Unlisted(keys=['d/r/x/a.txt']).cleanup('d/r/x/')

    def test_an_unlisted_upload_listing_is_never_a_clean_final_check(self):
        import tempfile
        class Unlisted(Memory):
            root_prefix = 'd'
            def uploads_in(self, prefix):
                return None
        with tempfile.TemporaryDirectory() as root:
            lab = type('Lab', (), {'root': Path(root)})()
            self.assertFalse(diff.final_leftover_check(Unlisted(), lab, 'memory', 'r'))

    def test_a_refused_bucket_loses_nothing_to_the_final_cleanup(self):
        import tempfile
        from unittest import mock
        class Versioned(Memory):
            root_prefix = 'd'
            def require_unversioned(self):
                raise SystemExit('versioned')
        target = Versioned(keys=['d/r/old.txt'])
        with tempfile.TemporaryDirectory() as work:
            options = type('Options', (), {'work': work, 'cases': ''})()
            lab = type('Lab', (), {'root': Path(work), 'stop': lambda self: None})()
            with mock.patch.object(diff, 'build_targets', return_value={'memory': target}), mock.patch.object(diff, 'Lab', return_value=lab):
                with self.assertRaises(SystemExit):
                    diff.run_server(options, 'memory', 'r')
        self.assertEqual(sorted(target.keys), ['d/r/old.txt'])


class Exit(unittest.TestCase):
    def run_main(self, clean):
        import tempfile
        from unittest import mock
        with tempfile.TemporaryDirectory() as out, mock.patch.object(diff, 'run_server', return_value=([], clean)), mock.patch.object(sys, 'argv', ['x', '--out', out, 'run', '--servers', 'oracle,minio']):
            return diff.main()

    def test_a_failed_cleanup_makes_the_command_fail(self):
        self.assertEqual(self.run_main(False), 1)

    def test_a_clean_run_succeeds(self):
        self.assertEqual(self.run_main(True), 0)


class Versioning(unittest.TestCase):
    def cloud(self, answer):
        target = diff.RawTarget('cloudy', 'https://s3.example.invalid', 'us-east-2', 'bucket', 'a', 's', cloud=True)
        target.raw = type('Raw', (), {'versioning': lambda self: answer})()
        return target

    def test_a_bucket_that_keeps_versions_is_refused(self):
        for state in ('Enabled', 'Suspended', None):
            with self.assertRaises(SystemExit, msg=repr(state)):
                self.cloud(state).require_unversioned()

    def test_a_bucket_that_never_had_versioning_is_accepted(self):
        self.cloud('').require_unversioned()


if __name__ == '__main__':
    unittest.main()
