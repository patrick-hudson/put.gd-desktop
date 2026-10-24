#!/usr/bin/env python3
"""Policy, proxy and external-observation contracts, without a product or GUI."""
import asyncio
import base64
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'performance'))
import campaign_proxy
import build_current_capture
import campaign_workloads as workloads
import campaign_fixture
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
            self.assertEqual(run.result['gates']['historySearchP95UpperBoundMs']['status'], 'unassessed')
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


class FileUploadSafetyTests(unittest.TestCase):
    def prepare(self, root):
        media = root / 'burst-media'
        media.mkdir(mode=0o700)
        path = media / 'queue-00-synthetic.png'
        campaign_fixture.png(path, 100, 320, 180)
        path.chmod(0o400)
        return path, workloads.upload_source(path, root)

    def backend(self, root, path, change=None):
        run = mock.Mock()
        run.root = root
        state = {'phase': 'editing', 'width': 320, 'height': 180, 'crop': [0, 0, 320, 180],
                 'shapes': [], 'modified': False}
        def call(method, params):
            if method == 'editor.open':
                self.assertEqual(params, {'source': 'file', 'path': str(path)})
                if change:
                    change()
                return {'operationId': 'edit-1'}
            if method == 'editor.inspect':
                return state
            if method == 'editor.preview':
                return {'mime': 'image/png', 'png': base64.b64encode(path.read_bytes()).decode(),
                        'width': 320, 'height': 180, 'sourceWidth': 320, 'sourceHeight': 180}
            if method == 'editor.submit':
                return {'submitted': True}
            if method == 'operation.inspect':
                return {'state': 'running', 'items': [{'artifact': {'sha256': 'b' * 64}}]}
            if method == 'editor.cancel':
                return {'cancelled': True}
            self.fail('Unsafe/unexpected production method: ' + method)
        run.call.side_effect = call
        return run

    def test_explicit_file_preview_is_attributed_before_upload_and_no_capture_is_invoked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, source = self.prepare(root)
            run = self.backend(root, path)
            self.assertEqual(workloads.submit_file_upload(run, source), 'edit-1')
            methods = [call.args[0] for call in run.call.call_args_list]
            self.assertEqual(methods, ['editor.open', 'editor.inspect', 'editor.preview',
                                      'editor.inspect', 'editor.submit', 'operation.inspect'])
            calls = run.mock_calls
            proof = next(i for i, call in enumerate(calls) if call[0] == 'event' and
                         call.args[0] == 'burst-file-source-attributed-before-submit')
            submit = next(i for i, call in enumerate(calls) if call[0] == 'call' and call.args[0] == 'editor.submit')
            self.assertLess(proof, submit)
            for call in run.event.call_args_list:
                self.assertNotIn('png', call.kwargs, 'Preview payload must never be logged')

    def test_changed_preview_pixels_cancel_without_upload_even_if_cover_would_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, source = self.prepare(root)
            def replace():
                path.chmod(0o600)
                campaign_fixture.png(path, 101, 320, 180)
                path.chmod(0o400)
            run = self.backend(root, path, replace)
            with self.assertRaisesRegex(EvidenceError, 'preview_content_mismatch'):
                workloads.submit_file_upload(run, source)
            methods = [call.args[0] for call in run.call.call_args_list]
            self.assertNotIn('editor.submit', methods)
            self.assertEqual(methods[-1], 'editor.cancel')

    def test_same_pixels_replaced_inode_is_rejected_before_submit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, source = self.prepare(root)
            def replace():
                replacement = path.with_suffix('.replacement')
                replacement.write_bytes(path.read_bytes())
                replacement.chmod(0o400)
                replacement.replace(path)
            run = self.backend(root, path, replace)
            with self.assertRaisesRegex(EvidenceError, 'identity_changed_before_submit'):
                workloads.submit_file_upload(run, source)
            self.assertNotIn('editor.submit', [call.args[0] for call in run.call.call_args_list])

    def test_symlink_and_writable_source_refused_before_any_product_call(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, source = self.prepare(root)
            run = self.backend(root, path)
            path.chmod(0o600)
            with self.assertRaisesRegex(EvidenceError, 'not_owned_immutable_png'):
                workloads.submit_file_upload(run, source)
            run.call.assert_not_called()
            path.chmod(0o400)
            link = path.with_name('linked.png')
            link.symlink_to(path)
            with self.assertRaisesRegex(EvidenceError, 'outside_private_media'):
                workloads.submit_file_upload(run, {**source, 'path': str(link)})
            run.call.assert_not_called()

    def test_independent_upload_with_correct_hash_but_wrong_known_pixels_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, source = self.prepare(root)
            other = root / 'different.png'
            campaign_fixture.png(other, 200, 320, 180)
            data = other.read_bytes()
            link = 'http://127.0.0.1:12345/fixture/upload.png'
            operation = {'id': 'op', 'state': 'completed', 'items': [{'uploadStatus': 'uploaded',
                'artifact': {'id': 'item', 'sha256': hashlib.sha256(data).hexdigest()},
                'receipt': {'directUrl': link}}]}
            run = mock.Mock()
            run.args.endpoint = 'http://127.0.0.1:12345'
            run.call.return_value = {'operationId': 'op', 'uploadStatus': 'uploaded'}
            response = mock.MagicMock()
            response.geturl.return_value = link
            response.read.side_effect = [data, b'']
            response.__enter__.return_value = response
            with mock.patch('urllib.request.build_opener') as opener:
                opener.return_value.open.return_value = response
                with self.assertRaisesRegex(EvidenceError, 'uploaded_pixels_differ'):
                    workloads.verify_uploaded(run, operation, source)

    def test_full_queue_protocol_uses_50_distinct_files_and_leaves_capture_gate_unassessed(self):
        # Control-flow rehearsal only: no product, GUI, fixture or upload runs.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = mock.Mock()
            run.root = root
            run.result = {'metrics': {}, 'gates': {}, 'limitations': []}
            run.sample.return_value = {'totals': {}}
            sources, operations = {}, {}
            current = None
            state = {'phase': 'editing', 'width': 320, 'height': 180, 'crop': [0, 0, 320, 180],
                     'shapes': [], 'modified': False}
            def call(method, params):
                nonlocal current
                if method == 'editor.open':
                    self.assertEqual(params['source'], 'file')
                    current = 'operation-' + str(len(sources))
                    sources[current] = Path(params['path'])
                    operations[current] = {'id': current, 'state': 'running', 'phase': 'queued',
                        'items': [{'uploadStatus': 'uploading', 'artifact': {'id': current, 'sha256': 'b' * 64},
                                   'receipt': {'directUrl': 'http://127.0.0.1:12345/fixture/' + current}}]}
                    return {'operationId': current}
                if method == 'editor.inspect':
                    return state
                if method == 'editor.preview':
                    return {'mime': 'image/png', 'png': base64.b64encode(sources[current].read_bytes()).decode(),
                            'width': 320, 'height': 180, 'sourceWidth': 320, 'sourceHeight': 180}
                if method == 'editor.submit':
                    return {'submitted': True}
                if method == 'operation.inspect':
                    return operations[params['id']]
                self.fail('Unexpected method: ' + method)
            run.call.side_effect = call
            run.wait_operation.side_effect = lambda key: {**operations[key], 'state': 'completed'}
            tray_states = iter([{'IconPixmap': 'idle', 'ToolTip': 'idle'}] +
                [{'IconPixmap': 'working', 'ToolTip': 'working'}] * 10 +
                [{'Status': 'Active', 'workingTitleObserved': False}])
            with mock.patch.object(workloads, 'tray', side_effect=lambda *args: next(tray_states)), \
                 mock.patch.object(workloads, 'UI'), \
                 mock.patch.object(workloads, 'verify_uploaded') as verify, \
                 mock.patch.object(workloads, 'settle') as settle, \
                 mock.patch.object(workloads.subprocess, 'run'), \
                 mock.patch.object(workloads.time, 'sleep'):
                workloads.burst(run)
            self.assertEqual(len(sources), 50)
            self.assertEqual(len({hashlib.sha256(path.read_bytes()).hexdigest() for path in sources.values()}), 50)
            self.assertEqual([call.args[0] for call in run.call.call_args_list].count('editor.submit'), 50)
            self.assertEqual(verify.call_count, 50)
            settle.assert_called_once_with(run, {})
            self.assertEqual(run.result['metrics']['burst']['acceptedFileEditorUploads'], 50)
            self.assertEqual(run.result['metrics']['burst']['nativeCapturesExercised'], 0)
            self.assertEqual(run.result['gates']['captureBurst']['status'], 'unassessed')


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


class CurrentWitnessTests(unittest.TestCase):
    def test_all_headless_socket_paths_are_precomputed_including_abstract_x11(self):
        paths = build_current_capture.test_socket_paths(Path('/tmp/p379-test/r'), 379)
        self.assertEqual(len(paths), 4)
        self.assertIn('/tmp/.X11-unix/X379', [row.get('path') for row in paths])
        self.assertEqual(paths[-1]['utf8Bytes'], len(b'\0/tmp/.X11-unix/X379'))
        with self.assertRaisesRegex(ValueError, 'socket_path_too_long'):
            build_current_capture.test_socket_paths(Path('/tmp/' + 'x' * 100), 379)

    def test_derivative_retains_frozen_ownership_timer_inputs_samples_and_budget(self):
        source = (build_current_capture.REPO / 'tests/performance/bench_capture.cpp').read_bytes()
        current, difference = build_current_capture.generate(source)
        original = source.decode()
        before = original[:original.index('bool outlinePainted(')]
        self.assertEqual(current[:current.index('bool outlinePainted(')], before)
        tail = original[original.index('bool visibleCanvas('):]
        tail = tail.replace('result["schemaVersion"]=3;', 'result["schemaVersion"]=4;result["frozenSourceSha256"]="' + build_current_capture.FROZEN_SHA256 + '";')
        tail = tail.replace('result["witnessMode"]="live-outline";', 'result["witnessMode"]="current-contrast";result["paintCriterion"]="scale1 black outer edge plus white inset edge at the original top-edge position";')
        self.assertEqual(current[current.index('bool visibleCanvas('):], tail)
        self.assertIn('1,2,~0u)', current)
        self.assertIn('qRound(local.y())', difference)
        self.assertEqual((build_current_capture.REPO / 'tests/performance/bench_capture.cpp').read_bytes(), source)

    def test_unknown_or_changed_frozen_source_is_not_rewritten(self):
        source = (build_current_capture.REPO / 'tests/performance/bench_capture.cpp').read_bytes()
        with self.assertRaisesRegex(ValueError, 'frozen_source_hash_mismatch'):
            build_current_capture.generate(source + b'// changed\n')


if __name__ == '__main__':
    unittest.main()
