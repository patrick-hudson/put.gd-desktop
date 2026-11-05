#!/usr/bin/env python3
"""Lightweight review recipe contracts; no app, compositor or media process."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import shlex
import sys
import unittest
from unittest.mock import patch
import tempfile
import types
import contextlib
import io

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver.api import Context, RecipeBlocked, RecipeFailed, REGISTRY
from tools.lab.surface_driver.recipes import review


class FakeContext(Context):
    desktop = 'mint-cinnamon-x11'
    host = 'boat'
    placement = 'boat'
    package = 'deb'
    destination = 'local'
    profile = {}

    def __init__(self):
        self.trace = []
        self.frames = []
        self.notes = []
        self.inputs_work = True
        self.export_status = 'saved'
        self.shell_status = 0
        self.operation = {'state': 'reviewing', 'items': []}
        self.info = {'state': 'reviewing', 'durationUs': 6_000_000, 'retainedUs': 6_000_000,
                     'canvas': [640, 480], 'config': {'format': 'webm', 'codec': 'vp9'},
                     'edit': {'revision': 0, 'trimStart': 0, 'trimEnd': None, 'masks': []}}
        self.state = {'visible': True, 'playing': False, 'playheadUs': 0, 'speed': 1,
                      'size': [960, 600], 'video': {'rect': [10, 20, 800, 400]},
                      'timeline': {'rect': [10, 430, 900, 80], 'lanes': 0, 'firstLane': 0,
                                   'interactionRowHeight': 20},
                      'format': {'format': 'webm', 'codec': 'vp9'}, 'masks': [],
                      'armed': None, 'selectedMaskId': None, 'discardConfirmation': {'open': False},
                      'fileDialog': {'open': False}, 'savedCopy': {'path': '', 'current': False},
                      'maskOptions': {'open': False}, 'preview': {'error': None},
                      'adviceDialog': {'open': False}}
        self.state['statusFacts'] = []
        self.menu = None
        self.last_focus = None
        self.chooser_path = ''
        self.last_click = None
        self.menu_index = 0
        self.clipboard = ''
        self.fixture_data = {'recording': {'id': 'recording-id', 'output_dir': '/tmp/r/out',
                                           'working_path': '/tmp/r/work.mkv', 'frozen_path': '/tmp/r/frozen.mkv'},
                             'destination': {'id': 'local-test', 'label': 'Isolated local copies'},
                             'review-controls': {'controls': [{'name': 'format'}]}}

    def fixture(self, name):
        if name in ('recording-budget-stop','recording-preview-error'):
            return self.fixture_data.get(name, self.fixture_data['recording'])
        if name not in self.fixture_data:
            self.blocked('missing fixture: ' + name)
        return self.fixture_data[name]

    def view(self, method, params=None):
        self.trace.append(('view', method, params))
        return self.call(method, params)

    def call(self, method, params=None, *, cleanup=False):
        self.trace.append(('call', method, params))
        params = params or {}
        if method == 'recording.inspect':
            return copy.deepcopy(self.info)
        if method == 'recording.capabilities':
            return {'formats': ['mp4', 'webm'], 'codecs': {'mp4': ['h264', 'h265'], 'webm': ['vp9']}}
        if method == 'operation.inspect':
            return copy.deepcopy(self.operation)
        if method == 'recording.saveCopy':
            self.info['lastExport'] = {'path': params['path'], 'status': self.export_status, 'revision': self.info['edit']['revision']}
            self.state['savedCopy'] = {'path': params['path'], 'current': self.export_status == 'saved'}
            return copy.deepcopy(self.info)
        if method == 'ui.history':
            return {}
        if method == 'recording.raise':
            self.state['visible'] = True
            return {}
        if method == 'recording.review':
            if self.state['playing']:
                self.state['playheadUs'] += 200_000
            self.state['trim'] = {'start': self.info['edit']['trimStart'],
                                  'end': self.info['edit']['trimEnd'] or self.info['durationUs']}
            self.state['edit'] = self.info['edit']
            self.state['state'] = self.info['state']
            return copy.deepcopy(self.state)
        raise AssertionError('unexpected call: ' + method)

    def wait(self, predicate, what, timeout=10, interval=.2):
        for _ in range(3):
            result = predicate()
            if result:
                return result
        raise RecipeFailed(what)

    def geometry(self, window):
        return {'x': 100, 'y': 50, 'width': 960, 'height': 600, 'scale': 1}

    def frame(self, label, window=None, reference=None):
        self.frames.append({'label': label, 'window': window, 'reference': reference})

    def note(self, text):
        self.notes.append(text)

    def review(self, reason):
        self.trace.append(('review', reason))

    def click(self, control, window, button='left', offset=None):
        self.trace.append(('click', control, window))
        self.last_click = control
        if control == 'text:Output format':
            control = 'format'
        if not self.inputs_work:
            return
        if control == 'play':
            self.state['playing'] = not self.state['playing']
        elif control in ('speed', 'format', 'addMask', 'masks'):
            self.menu = control
        elif control in ('discard', 'windowClose'):
            self.state['discardConfirmation']['open'] = True
        elif control == 'keepReviewing':
            self.state['discardConfirmation']['open'] = False
        elif control == 'confirmDiscard':
            self.operation['state'] = 'cancelled'
        elif control == 'windowMinimize':
            self.state['visible'] = False
        elif control == 'maskOptions':
            self.state['maskOptions']['open'] = True
        elif control == 'useRedact':
            self.info['edit']['masks'][0]['kind'] = 'solid'
            self.info['edit']['revision'] += 1
        elif control == 'upload':
            self.operation = {'state': 'completed', 'destination': {'id': 'local-test'},
                              'items': [{'local': {'status': 'saved', 'path': '/tmp/r/archive.webm'}}]}
        elif control == 'text:Copy path':
            self.clipboard = self.state['savedCopy']['path']
        elif control == 'text:Dismiss':
            self.state['savedCopy']['path'] = ''
        elif control == 'text:Save a copy…':
            self.state['fileDialog']['open'] = True

    def focus(self, window, raise_window=True):
        self.trace.append(('focus', window))
        self.last_focus = window
        return {}

    def keys(self, *keys):
        self.trace.append(('keys', keys))
        if self.state['fileDialog']['open'] and self.last_focus != 'fileDialog':
            raise RecipeFailed('file chooser must own guarded keyboard input')
        if not self.inputs_work:
            return
        for key in keys:
            if key == 'Home' and self.menu:
                self.menu_index = 0
            elif key == 'Down' and self.menu:
                self.menu_index += 1
            elif key == 'Return' and self.menu:
                menu, self.menu = self.menu, None
                if menu == 'addMask':
                    self.state['armed'] = 'solid' if self.menu_index == 0 else 'blur'
                elif menu == 'speed':
                    self.state['speed'] = [.5, 1, 2, 4][self.menu_index]
                elif menu == 'format':
                    fmt, codec = [('mp4', 'h264'), ('mp4', 'h265'), ('webm', 'vp9')][self.menu_index]
                    self.state['format'] = {'format': fmt, 'codec': codec}
                    self.info['config'] = self.state['format'].copy()
            elif key == 'Return' and self.state['armed']:
                self.add_mask()
            elif key == 'Return' and self.state['fileDialog']['open']:
                self.state['fileDialog']['open'] = False
                self.call('recording.saveCopy', {'id':'recording-id', 'path':self.chooser_path})
            elif key == 'Esc':
                self.menu = None
                self.state['armed'] = None
                self.state['discardConfirmation']['open'] = False
            elif key == 'Space':
                self.state['playing'] = not self.state['playing']

    def add_mask(self):
        count = len(self.info['edit']['masks'])
        mask = {'id': f'm{count}', 'kind': self.state['armed'], 'rect': [120, 120, 180, 120],
                'start': 0, 'end': 6_000_000}
        self.info['edit']['masks'].append(mask)
        self.info['edit']['revision'] += 1
        self.state['masks'].append({'id': mask['id'], 'timeline': [120, 480+count*20, 720, 18]})
        self.state['selectedMaskId'] = mask['id']
        self.state['armed'] = None
        self.state['timeline']['lanes'] = count + 1
        self.state['timeline']['firstLane'] = max(0, count-1)

    def click_at(self, x, y, button='left'):
        self.trace.append(('click_at', x, y))
        self.state['playheadUs'] = max(0, min(6_000_000, (x-220)/720*6_000_000))

    def drag(self, start, end, button='left', steps=12):
        self.trace.append(('drag', start, end))
        if self.state['armed'] and self.inputs_work:
            self.add_mask()

    def scroll(self, x, y, clicks):
        self.trace.append(('scroll', clicks))
        if self.inputs_work:
            self.state['timeline']['firstLane'] = 0 if clicks > 0 else 3

    def shell(self, script, timeout=60):
        self.trace.append(('shell', script))
        return {'returncode': self.shell_status, 'stdout': '{"exportMotion": 8.1}', 'stderr': ''}

    def type_text(self, text):
        self.trace.append(('type_text', text))
        if self.state['fileDialog']['open']:
            self.chooser_path = text
        if self.last_click == 'class:QSpinBox':
            self.info['edit']['masks'][0]['strength'] = int(text)

    def clipboard_text(self):
        return self.clipboard

    def accessible(self, query):
        self.trace.append(('accessible', query))
        if query.get('window') == 'review':
            return []
        return [{'name': 'Isolated local copies', 'popup': {'role':'menu'}}]


class ReviewRecipes(unittest.TestCase):
    def test_exact_lane_ownership_and_unique_registration(self):
        owners = json.loads((ROOT / 'tools/lab/surface_driver/ownership.json').read_text())['owners']
        actual = [i for r in REGISTRY if r.function.__module__ == review.__name__ for i in r.interactions]
        self.assertEqual(set(actual), {i for i, lane in owners.items() if lane == 'review'})
        self.assertEqual(len(actual), len(set(actual)))

    def test_playback_real_input_and_references(self):
        ctx = FakeContext()
        review.playback(ctx)
        self.assertEqual(ctx.state['speed'], 2)
        self.assertFalse(ctx.state['playing'])
        self.assertTrue(all(f['reference'] for f in ctx.frames))
        self.assertEqual([f['label'] for f in ctx.frames], ['before', 'after'])

    def test_ignored_playback_input_fails(self):
        ctx = FakeContext()
        ctx.inputs_work = False
        with self.assertRaises(RecipeFailed):
            review.playback(ctx)

    def test_fresh_fixture_required(self):
        ctx = FakeContext()
        ctx.info['edit']['trimStart'] = 100_000
        with self.assertRaisesRegex(RecipeFailed, 'fresh fixture'):
            review.playback(ctx)

    def test_api_open_retains_full_desktop_evidence(self):
        ctx = FakeContext()
        review._open(ctx, frame_review=False)
        self.assertIsNone(ctx.frames[-1]['window'])
        self.assertIsNone(ctx.frames[-1]['reference'])
        self.assertTrue(any(t[0] == 'review' for t in ctx.trace))

    def test_core_namespace_fixture_is_explicitly_blocked(self):
        ctx = FakeContext()
        ctx.fixture_data['recording'] = types.SimpleNamespace(id='recording-id')
        with self.assertRaises(RecipeBlocked) as result:
            review.playback(ctx)
        self.assertEqual(result.exception.issue, 672)

    def test_missing_motion_proof_fails_before_input(self):
        ctx = FakeContext()
        ctx.shell_status = 1
        with self.assertRaisesRegex(RecipeFailed, 'pattern moves'):
            review.playback(ctx)
        self.assertFalse(any(t[0] in ('click','keys') for t in ctx.trace))

    def test_discard_keep_then_confirm(self):
        ctx = FakeContext()
        review.discard(ctx)
        self.assertEqual(ctx.operation['state'], 'cancelled')
        self.assertIn('41-review-discard-confirm', [f['reference'] for f in ctx.frames])
        self.assertIn(('click', 'keepReviewing', 'confirmAction'), ctx.trace)
        self.assertIn(('click', 'confirmDiscard', 'confirmAction'), ctx.trace)

    def test_window_chrome_and_cancel(self):
        ctx = FakeContext()
        review.window_close(ctx)
        self.assertFalse(ctx.state['discardConfirmation']['open'])
        self.assertEqual(ctx.info['state'], 'reviewing')
        self.assertIn(('focus', 'confirmAction'), ctx.trace)

    def test_save_copy_routes_keyboard_to_chooser(self):
        ctx = FakeContext()
        review.save_copy(ctx)
        self.assertIn(('focus','fileDialog'), ctx.trace)
        self.assertEqual(ctx.clipboard, '/tmp/r/out/user-copy.webm')
        self.assertTrue(any(t[0] == 'shell' for t in ctx.trace))

    def test_mask_options_converts_and_decodes(self):
        ctx = FakeContext()
        review.mask_options(ctx)
        self.assertEqual(ctx.info['edit']['masks'][0]['kind'], 'solid')
        self.assertTrue(any(t[0] == 'drag' for t in ctx.trace))
        self.assertTrue(any(t[0] == 'shell' for t in ctx.trace))

    def test_wheel_really_changes_lanes(self):
        ctx = FakeContext()
        review.wheel_lanes(ctx)
        self.assertEqual(ctx.state['timeline']['firstLane'], 3)
        self.assertEqual(len(ctx.info['edit']['masks']), 5)

    def test_catalog_named_format_and_fallback(self):
        for controls, selector in [([{'name': 'format'}], 'format'), ([], 'text:Output format')]:
            ctx = FakeContext()
            ctx.fixture_data['review-controls']['controls'] = controls
            review._choose_format(ctx, 'recording-id', 'mp4', 'h265')
            self.assertIn(('click', selector, 'review'), ctx.trace)

    def test_unavailable_codec_blocked_not_passed(self):
        ctx = FakeContext()
        with self.assertRaises(RecipeBlocked):
            review._choose_format(ctx, 'recording-id', 'webm', 'h265')

    def test_failed_export_fails_before_media(self):
        ctx = FakeContext()
        ctx.export_status = 'failed'
        with self.assertRaisesRegex(RecipeFailed, 'export succeeded'):
            review._export(ctx, 'recording-id')
        self.assertFalse(any(t[0] == 'shell' for t in ctx.trace))

    def test_stale_saved_export_cannot_satisfy_new_copy(self):
        ctx = FakeContext()
        ctx.info['lastExport'] = {'path':'/tmp/old.webm','revision':0,'status':'saved'}
        with self.assertRaisesRegex(RecipeFailed, 'requested path'):
            review._saved(ctx, 'recording-id', '/tmp/new.webm')
        self.assertFalse(any(t[0]=='shell' for t in ctx.trace))

    def test_saved_waits_for_new_path_before_decoding(self):
        ctx = FakeContext()
        ctx.info['lastExport'] = {'path':'/tmp/old.webm','revision':0,'status':'saved'}
        inspect = review._inspect
        polls = []
        def completing(context, rid):
            polls.append(rid)
            if len(polls) == 2:
                context.info['lastExport']['path'] = '/tmp/new.webm'
            return inspect(context, rid)
        with patch.object(review, '_inspect', side_effect=completing):
            review._saved(ctx, 'recording-id', '/tmp/new.webm')
        self.assertEqual(len(polls), 2)
        self.assertTrue(any(t[0] == 'shell' for t in ctx.trace))

    def test_saved_waits_while_first_export_is_not_yet_accepted(self):
        ctx = FakeContext()
        ctx.info['lastExport'] = None
        inspect = review._inspect
        polls = []
        def completing(context, rid):
            polls.append(rid)
            if len(polls) == 2:
                context.call('recording.saveCopy', {'id':rid, 'path':'/tmp/new.webm'})
            return inspect(context, rid)
        with patch.object(review, '_inspect', side_effect=completing):
            review._saved(ctx, 'recording-id', '/tmp/new.webm')
        self.assertEqual(len(polls), 2)

    def test_status_retry_uses_fault_fixture_and_decodes(self):
        ctx = FakeContext()
        path = '/tmp/r/out/retry.webm'
        ctx.fixture_data['recording-export-failure'] = dict(ctx.fixture_data['recording'], retry_path=path)
        ctx.info['lastExport'] = {'path':path, 'status':'failed', 'revision':0}
        click = ctx.click
        def retry(control, window, **kwargs):
            click(control, window, **kwargs)
            if control == 'text:Try again':
                ctx.call('recording.saveCopy', {'id':'recording-id', 'path':path})
        ctx.click = retry
        with self.assertRaisesRegex(RecipeBlocked, 'Show in folder'):
            review.status_actions(ctx)
        self.assertTrue(any(t[0] == 'shell' and 'REVIEW_MEDIA_PY' in t[1] for t in ctx.trace))
        self.assertEqual(ctx.clipboard, path)

    def test_budget_fixture_has_a_bounded_longer_deadline(self):
        spec = next(r for r in REGISTRY if 'review.notice' in r.interactions)
        self.assertEqual(spec.needs, ('recording-budget-stop',))
        self.assertGreaterEqual(spec.timeout, 180)

    def test_notice_opens_its_named_fixture(self):
        ctx = FakeContext()
        ctx.fixture_data['recording-budget-stop'] = dict(ctx.fixture_data['recording'], id='budget-recording')
        with self.assertRaises(RecipeBlocked):
            review.stop_notice(ctx)
        self.assertIn(('view','recording.review',{'id':'budget-recording','action':'inspect'}), ctx.trace)

    def test_media_assertions_are_mandatory(self):
        ctx = FakeContext()
        ctx.shell_status = 1
        with self.assertRaisesRegex(RecipeFailed, 'decoded output'):
            review._export(ctx, 'recording-id')

    def test_media_check_uses_quoted_paths_and_authoritative_trim(self):
        ctx = FakeContext()
        ctx.fixture_data['recording']['working_path'] = "/tmp/a ' $(false)\n.mkv"
        ctx.info['edit'].update(trimStart=1_000_000, trimEnd=4_000_000)
        review._verify(ctx, 'recording-id', '/tmp/x.webm')
        command = next(t[1] for t in ctx.trace if t[0] == 'shell')
        args = shlex.split(command.split(" <<'REVIEW_MEDIA_PY'", 1)[0])
        params = json.loads(args[2])
        self.assertEqual(params['duration'], 3)
        self.assertEqual(params['working'], ctx.fixture_data['recording']['working_path'])
        self.assertEqual(params['codec'], 'vp9')
        compile(review._MEDIA_CHECK, '<media check>', 'exec')
        self.assertIn("'-nostdin'", review._MEDIA_CHECK)
        self.assertIn('solid mask leaked source pixels', review._MEDIA_CHECK)
        self.assertIn('frozen export', review._MEDIA_CHECK)

    def test_cli_and_mcp_reply_envelopes(self):
        for reply in ({'ok': True, 'result': {'state':'reviewing'}},
                      {'structuredContent': {'ok':True,'result':{'state':'reviewing'}}},
                      {'content':[{'type':'text','text':'{"ok":true,"result":{"state":"reviewing"}}'}]},
                      {'returncode':0,'stdout':'{"ok":true,"result":{"state":"reviewing"}}'}):
            self.assertEqual(review._reply(FakeContext(), reply)['state'], 'reviewing')
        with self.assertRaises(RecipeFailed):
            review._reply(FakeContext(), {'isError':True})

    def test_api_exports_include_a_timed_solid_mask_inside_trim(self):
        trim, mask = review._api_edits({'canvas':[640,320]})
        self.assertEqual(trim, {'op':'setTrim','start':1_000_000,'end':3_000_000})
        self.assertEqual(mask['mask']['rect'], [160,80,160,80])
        self.assertEqual(mask['mask']['kind'], 'solid')
        self.assertLess(trim['start'], mask['mask']['start'])
        self.assertLess(mask['mask']['end'], trim['end'])

    def test_mcp_review_upload_uses_reviewed_revision_and_decodes(self):
        ctx = FakeContext()
        ctx.info['edit']['revision'] = 3
        calls = []
        def mcp(tool, params):
            calls.append((tool, params))
            return {'ok':True, 'result': {'operationId':'upload-id'} if tool == 'upload_recording' else
                    {'state':'completed', 'items':[{'local':{'status':'saved','path':'/tmp/result.webm'}}]}}
        ctx.mcp = mcp
        with patch.object(review, '_mcp_review', return_value=('recording-id', ctx.info)):
            review.mcp_recording_review(ctx)
        self.assertEqual(calls[0], ('upload_recording', {'id':'recording-id','revision':3}))
        self.assertEqual(calls[1], ('get_operation', {'id':'upload-id'}))
        self.assertTrue(any(t[0] == 'shell' for t in ctx.trace))
        self.assertEqual(ctx.frames[-1]['label'], 'uploaded')

    def test_screen_coordinates_follow_live_geometry(self):
        ctx = FakeContext()
        state = ctx.call('recording.review')
        self.assertEqual(review._point(ctx, state, [0, 0, 960, 600]), (580, 350))
        ctx.geometry = lambda _: {'x': -1920, 'y': 0, 'screen_x': -1920, 'width': 960, 'height': 600, 'scale': 2}
        self.assertEqual(review._point(ctx, state, [0, 0, 960, 600]), (960, 600))

    def test_upload_terminal_operation_and_stored_file(self):
        ctx = FakeContext()
        review.upload(ctx)
        self.assertTrue(any(t[0] == 'shell' for t in ctx.trace))
        self.assertEqual(ctx.frames[-1]['reference'], '01-history-list')
        self.assertIn(('accessible', {'owned':True,'visible':True,'showing':True}), ctx.trace)

    def test_upload_does_not_accept_an_unrelated_destination_label(self):
        ctx = FakeContext()
        ctx.accessible = lambda query: [{'name':'Isolated local copies','popup':None}]
        with self.assertRaisesRegex(RecipeBlocked, 'destination popup'):
            review.upload(ctx)
        self.assertEqual(ctx.operation['state'], 'reviewing')

    def test_live_destinations_never_run_on_boat(self):
        for destination in ('s3', 'proton'):
            ctx = FakeContext()
            ctx.destination = destination
            with self.assertRaisesRegex(RecipeBlocked, 'Boat forbids'):
                review.upload(ctx)
            self.assertFalse(ctx.trace)

    def test_budget_notice_needs_real_stop(self):
        ctx = FakeContext()
        with self.assertRaisesRegex(RecipeBlocked, 'genuine byte-budget'):
            review.stop_notice(ctx)

    def test_preview_fault_needs_reversible_fixture(self):
        ctx = FakeContext()
        with self.assertRaisesRegex(RecipeBlocked, 'reversible decoder failure'):
            review.preview_error(ctx)


class MediaOracle(unittest.TestCase):
    """Run the actual target verifier with a bounded fake decoder, including failures."""

    def check_media(self, *, duration=6, codec='vp9', export_frozen=False,
                    leak=False, dark_source=False):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / 'output.webm')
            Path(path).write_bytes(b'media')
            params = {'path': path, 'working': '/working.mkv', 'frozen': '/frozen.mkv',
                      'duration': 6, 'codec': 'vp9', 'container': 'webm', 'trimStart': 0,
                      'trimEnd': 6_000_000, 'canvas': [640,480],
                      'masks': [{'kind':'solid','rect':[100,100,40,40],'start':0,'end':6_000_000}]}
            def decoder(args, **kwargs):
                if args[0] == 'ffprobe':
                    data = json.dumps({'format': {'duration': str(duration), 'format_name':'matroska,webm'},
                                       'streams':[{'codec_type':'video','codec_name':codec,'width':640,'height':480}]}).encode()
                else:
                    self.assertIn('-nostdin', args)
                    source = args[args.index('-i')+1]
                    if '-frames:v' in args:
                        size = 34*34*3
                        value = (0 if dark_source else 100) if source == '/working.mkv' else (80 if leak else 0)
                        data = bytes([value])*size
                    else:
                        moving = source == '/working.mkv' or (source == path and not export_frozen)
                        data = bytes([10])*6144 + bytes([30 if moving else 10])*6144
                return types.SimpleNamespace(stdout=data, returncode=0)
            with patch.object(sys, 'argv', ['verifier', json.dumps(params)]), \
                 patch('subprocess.run', side_effect=decoder), contextlib.redirect_stdout(io.StringIO()):
                exec(compile(review._MEDIA_CHECK, '<target verifier>', 'exec'), {})

    def test_valid_decoded_mask_and_motion(self):
        self.check_media()

    def test_wrong_duration_rejected(self):
        with self.assertRaises(AssertionError):
            self.check_media(duration=9)

    def test_wrong_codec_rejected(self):
        with self.assertRaises(AssertionError):
            self.check_media(codec='h264')

    def test_frozen_export_rejected(self):
        with self.assertRaisesRegex(AssertionError, 'frozen export'):
            self.check_media(export_frozen=True)

    def test_leaking_mask_rejected(self):
        with self.assertRaisesRegex(AssertionError, 'leaked source pixels'):
            self.check_media(leak=True)

    def test_black_source_is_not_mask_evidence(self):
        with self.assertRaisesRegex(AssertionError, 'visible pattern'):
            self.check_media(dark_source=True)


if __name__ == '__main__':
    unittest.main()
