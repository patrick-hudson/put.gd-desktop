#!/usr/bin/env python3
"""Policy, proxy and external-observation contracts, without a product or GUI."""
import asyncio
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'performance'))
import campaign_proxy
import campaign_workloads as workloads
from bench_ui_process import EvidenceError


class ThresholdTests(unittest.TestCase):
    def test_no_policy_does_not_pass_a_measured_response(self):
        run = mock.Mock()
        run.args.threshold_policy = None
        run.result = {'metrics': {}, 'gates': {}}
        workloads.assess(run, 'historySearchP95UpperBoundMs', 10)
        self.assertEqual(run.result['metrics']['historySearchP95UpperBoundMs'], 10)
        self.assertEqual(run.result['gates']['historySearchP95UpperBoundMs']['status'], 'unassessed')

    def test_explicit_policy_requires_attribution_and_does_not_pass_missing_measurements(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'policy.json'
            value = {'schemaVersion': 1, 'approvedBy': 'test-only attribution', 'approvalReference': 'synthetic-unit-test',
                     'maximums': {'historySearchP95UpperBoundMs': 250}}
            path.write_text(json.dumps(value)); path.chmod(0o600)
            run = mock.Mock()
            run.args.threshold_policy = str(path)
            run.result = {'metrics': {}, 'gates': {}}
            workloads.assess(run, 'historySearchP95UpperBoundMs', None)
            self.assertEqual(run.result['gates']['historySearchP95UpperBoundMs']['status'], 'fail')
            value['approvedBy'] = ''
            path.write_text(json.dumps(value))
            with self.assertRaisesRegex(EvidenceError, 'attribution_required'):
                workloads.policy(path)

    def test_nan_or_unknown_numerical_budget_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'policy.json'
            for maximums in ({'historySearchP95UpperBoundMs': float('nan')}, {'newInventedGate': 1}):
                path.write_text(json.dumps({'schemaVersion': 1, 'approvedBy': 'unit-test',
                                           'approvalReference': 'test-only', 'maximums': maximums}))
                path.chmod(0o600)
                with self.assertRaisesRegex(EvidenceError, 'invalid_threshold_policy_maximums'):
                    workloads.policy(path)

    def test_uploaded_link_is_rejected_before_external_http_request(self):
        run = mock.Mock()
        run.args.endpoint = 'http://127.0.0.1:12345'
        run.call.return_value = {'operationId': 'operation', 'uploadStatus': 'uploaded'}
        operation = {'id': 'operation', 'state': 'completed', 'items': [{'uploadStatus': 'uploaded',
            'artifact': {'id': 'item', 'sha256': 'a' * 64}, 'receipt': {'directUrl': 'https://example.com/payload'}}]}
        with mock.patch('urllib.request.build_opener') as opener:
            with self.assertRaisesRegex(EvidenceError, 'non_loopback_result_link'):
                workloads.verify_uploaded(run, operation)
            opener.return_value.open.assert_not_called()


class ProxyTests(unittest.IsolatedAsyncioTestCase):
    async def test_original_16k_chunk_deadlocks_at_256k_but_wrapper_makes_progress(self):
        source = Path(__file__).resolve().parents[2] / 'docs/evidence/2026-10-20-round9-storage/scripts/throttle_proxy.py'
        before = source.read_bytes()
        original, digest = campaign_proxy.load_original(source, 0, 12345, 262144)
        self.assertEqual(source.read_bytes(), before)
        self.assertEqual(len(digest), 64)
        with self.assertRaises(asyncio.TimeoutError):
            await asyncio.wait_for(original['bucket'].take(16384), .1)
        bucket = campaign_proxy.BoundedBucket(262144)
        await asyncio.wait_for(bucket.take(16384), 2)
        self.assertLess(bucket.tokens, 1)

    async def test_shared_rate_is_accounted_across_concurrent_connections(self):
        bucket = campaign_proxy.BoundedBucket(262144)
        await asyncio.wait_for(asyncio.gather(*(bucket.take(8192) for _ in range(4))), 2)
        self.assertGreaterEqual(bucket.tokens, 0)
        with self.assertRaises(ValueError):
            await bucket.take(16385)

    async def test_changed_frozen_entrypoint_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'proxy.py'
            path.write_text('raise RuntimeError("must never execute")\n')
            with self.assertRaisesRegex(ValueError, 'unrecognized_frozen_proxy_startup'):
                campaign_proxy.load_original(path, 0, 12345, 262144)


if __name__ == '__main__':
    unittest.main()
