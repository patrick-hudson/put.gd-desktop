"""Fault-injection checks for Tier 1's independent result oracles."""
from contextlib import contextmanager, nullcontext
import io
from pathlib import Path
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, call, patch
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.lab.lab import FIXTURE_RGB
from tools.lab.surface_driver.recipes.outcomes import edited_pixels, pattern_visible, pixels_equal
from tools.lab.surface_driver.recipes import outcomes
from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.run import discover, validate_recipes


def original():
    image = Image.new('RGB', (640, 320))
    for i, color in enumerate(FIXTURE_RGB):
        x, y = i % 2 * 320, i // 2 * 160
        image.paste(color, (x, y, x + 320, y + 160))
    return image


def annotated():
    image = original()
    draw = ImageDraw.Draw(image)
    draw.line((30, 30, 250, 90), fill='white', width=4)
    draw.rectangle((350, 30, 590, 110), outline='white', width=4)
    draw.text((40, 220), 'Tier 1', fill='white')
    draw.rectangle((360, 190, 550, 280), fill='black')
    return image


class Outcomes(unittest.TestCase):
    def test_window_and_desktop_capture_work_without_clipboard_tools(self):
        expected = original()
        stream = io.BytesIO()
        expected.save(stream, format='PNG')
        for kind in ('window', 'desktop'):
            with self.subTest(kind=kind):
                ctx = Mock()
                ctx.expect = ClipboardContext().expect
                ctx.call.side_effect = [dict(operations=[]),
                    dict(document=dict(actions={kind:dict(shortcut='synthetic-hotkey')}))]
                ctx.wait.side_effect = [True, [dict(id='operation')]]
                detail = dict(id='row')
                with patch.object(outcomes, '_clipboard_reader', side_effect=RecipeBlocked('missing reader')) as reader, \
                        patch.object(outcomes, '_pattern', return_value=(SimpleNamespace(rect=(0, 0, 640, 320)), expected)), \
                        patch.object(outcomes.selectors, '_portal_scope', return_value=nullcontext()), \
                        patch.object(outcomes, '_complete', return_value=(detail, stream.getvalue())):
                    self.assertIs(outcomes._capture(ctx, object(), object(), kind), detail)
                reader.assert_not_called()

    def test_region_capture_checks_clipboard_tools_before_input(self):
        ctx = Mock()
        with patch.object(outcomes, '_clipboard_reader', side_effect=RecipeBlocked('missing reader')), \
                patch.object(outcomes, '_pattern') as pattern:
            with self.assertRaisesRegex(RecipeBlocked, 'missing reader'):
                outcomes._capture(ctx, object(), object(), 'region')
        pattern.assert_not_called()
        ctx.drag.assert_not_called()

    def test_pattern_oracle_rejects_foreign_ui_and_absent_video(self):
        cover = Image.new('RGB', (1280, 800), (255, 0, 255))
        rect = (320, 240, 640, 320)
        self.assertFalse(pattern_visible(cover, rect))
        video = annotated()
        cover.paste(video, rect[:2])
        self.assertTrue(pattern_visible(cover, rect))
        cover.putpixel((1100, 600), (0, 0, 0))
        self.assertFalse(pattern_visible(cover, rect))

    def test_capture_rejects_shift_dimensions_black_and_overlay(self):
        expected = original()
        self.assertTrue(pixels_equal(expected.copy(), expected))
        shifted = expected.copy()
        shifted.paste(expected.crop((1, 0, 640, 320)), (0, 0))
        overlay = expected.copy()
        overlay.putpixel((100, 100), (0, 0, 0))
        transparent = expected.convert('RGBA')
        transparent.putalpha(0)
        for defect in (shifted, overlay, transparent, Image.new('RGB', expected.size), expected.resize((320, 160))):
            self.assertFalse(pixels_equal(defect, expected))

    def test_editor_rejects_missing_marks_non_solid_redaction_and_wrong_location(self):
        self.assertTrue(edited_pixels(annotated(), original()))
        self.assertFalse(edited_pixels(original(), original()))
        transparent = annotated().convert('RGBA')
        transparent.putalpha(0)
        self.assertFalse(edited_pixels(transparent, original()))
        for box in ((20, 20, 265, 105), (340, 20, 605, 125), (30, 195, 290, 265)):
            missing = annotated()
            missing.paste(original().crop(box), box)
            self.assertFalse(edited_pixels(missing, original()))
        for location in ((400, 230), (610, 300)):
            defect = annotated()
            defect.putpixel(location, (1, 2, 3))
            self.assertFalse(edited_pixels(defect, original()))

    def test_editor_rejects_shifted_annotations_inside_their_broad_regions(self):
        for box, shift in (((20, 20, 265, 105), 10), ((340, 20, 605, 125), 10),
                           ((30, 195, 290, 265), 10), ((350, 180, 560, 290), 5)):
            image = annotated()
            mark = image.crop(box)
            image.paste(original().crop(box), box)
            image.paste(mark, (box[0] + shift, box[1]))
            self.assertFalse(edited_pixels(image, original()))

    def test_eleven_recipes_reference_existing_interactions(self):
        recipes = discover('outcomes')
        self.assertEqual(len(recipes), 11)
        validate_recipes(recipes)
        self.assertTrue(all(r.destinations == ('local',) for r in recipes))


class FileManagerUpload(unittest.TestCase):
    TSV = ('level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n'
           '5\t1\t1\t1\t{line}\t1\t40\t200\t100\t20\t96\tUpload\n'
           '5\t1\t1\t1\t2\t2\t150\t200\t60\t20\t96\twith\n'
           '5\t1\t1\t1\t2\t3\t220\t200\t90\t20\t96\t{last}\n'
           '5\t1\t1\t1\t3\t1\t40\t260\t100\t20\t96\tProperties\n')

    def context(self, session='x11'):
        ctx = ClipboardContext(session)
        ctx.geometry = Mock(return_value=dict(x=0, y=0, width=1280, height=800, head=0, pid=7))
        ctx.focus = Mock()
        ctx.click_at = Mock()
        ctx.frame = Mock()
        return ctx

    def test_right_click_lands_on_the_name_not_the_empty_end_of_its_row(self):
        ctx = self.context()
        # Nemo's row: [448,193,256,20]. Its centre is empty space, which opens the background menu.
        ctx.accessible = Mock(return_value=[dict(rect=[448, 193, 256, 20], role='table cell')])
        outcomes._right_click_file(ctx, 'role', 'fm.txt', {'process': 'nemo'})
        ctx.click_at.assert_called_once_with(488, 203, button='right', head=0)
        self.assertEqual(ctx.accessible.call_args.args[0]['name'], 'fm.txt*')

    def test_the_smallest_node_naming_the_file_gets_the_click(self):
        ctx = self.context()
        ctx.accessible = Mock(return_value=[dict(rect=[0, 0, 500, 100], role='list item'), dict(rect=[10, 10, 100, 20], role='label')])
        outcomes._right_click_file(ctx, 'role', 'fm.txt', {'process': 'dolphin'})
        ctx.click_at.assert_called_once_with(50, 20, button='right', head=0)

    def test_a_file_the_view_does_not_place_fails(self):
        ctx = self.context()
        # Nemo's icon view: the file has a name but no bounds.
        ctx.accessible = Mock(return_value=[dict(rect=None, role='icon')])
        with self.assertRaisesRegex(RecipeFailed, 'the file is visible'):
            outcomes._right_click_file(ctx, 'role', 'fm.txt', {'process': 'nemo'})
        ctx.click_at.assert_not_called()

    def test_named_menu_needs_exactly_one_entry_and_clicks_it_as_a_popup(self):
        ctx = self.context()
        popups = []
        ctx.click_at = Mock(side_effect=lambda *a, **k: popups.append(ctx._popup))
        entry = dict(rect=[489, 270, 185, 26], popup=dict(role='menu'))
        ctx.accessible = Mock(return_value=[])
        with self.assertRaisesRegex(RecipeFailed, 'is in the right-click menu'):
            outcomes._click_menu_entry(ctx, Mock(), 'role', {})
        ctx.accessible = Mock(return_value=[entry, entry])
        with self.assertRaisesRegex(RecipeFailed, 'one Upload with put.gd entry'):
            outcomes._click_menu_entry(ctx, Mock(), 'role', {})
        ctx.click_at.assert_not_called()
        ctx.accessible = Mock(return_value=[entry])
        outcomes._click_menu_entry(ctx, Mock(), 'role', {})
        ctx.click_at.assert_called_once_with(541, 283, head=0)
        self.assertEqual(popups, [entry['popup']])
        self.assertIsNone(ctx._popup)

    def test_drawn_menu_is_read_only_for_the_whole_entry_on_one_line(self):
        self.assertEqual(outcomes._menu_line(self.TSV.format(line=2, last='put.gd')), (90, 210))
        self.assertEqual(outcomes._menu_line(self.TSV.format(line=2, last='put.gd,')), (90, 210))
        self.assertIsNone(outcomes._menu_line(self.TSV.format(line=2, last='nothing')))
        self.assertIsNone(outcomes._menu_line(self.TSV.format(line=1, last='put.gd')))
        self.assertIsNone(outcomes._menu_line('level\ttext\n'))

    def test_drawn_menu_clicks_the_read_line_in_head_pixels(self):
        ctx = self.context('wayland')
        ctx.profile['compositor'] = dict(kind='gnome-lab-witness')
        ctx._probe = Mock(return_value=dict(windows=[dict(pid=7, type=0, frame=[0, 0, 900, 500]),
                                                     dict(pid=7, type=9, frame=[474, 240, 243, 450])]))
        ctx.target = SimpleNamespace(capture_image=lambda head: Image.new('RGB', (1280, 800)))
        ctx.shell = Mock(return_value=dict(returncode=0, stdout=self.TSV.format(line=2, last='put.gd')))
        scope = SimpleNamespace(root='/synthetic', write=Mock())
        outcomes._click_menu_entry(ctx, scope, 'role', {'menu': 'drawn'})
        # The crop is read at twice its size.
        ctx.click_at.assert_called_once_with(474 + 45, 240 + 105, head=0)
        ctx.shell = Mock(return_value=dict(returncode=0, stdout='level\ttext\n'))
        with self.assertRaisesRegex(RecipeFailed, 'is in the right-click menu'):
            outcomes._click_menu_entry(ctx, scope, 'role', {'menu': 'drawn'})

    def upload_case(self, *, notices=1, uploaded='digest', name='fm.txt'):
        ctx = self.context()
        ctx.profile.update(file_manager={'process': 'nemo'}, guest='cinnamon-x11')
        ctx.call = Mock(return_value=dict(operations=[]))
        ctx.notifications = Mock(return_value=[dict(summary='Upload complete')] * notices)
        detail = dict(id='row', name=name, receipt=dict(directUrl='http://127.0.0.1:1/fm.txt'))
        scope = Mock()
        @contextmanager
        def lane(ctx):
            yield scope, object()
        with patch.object(outcomes, '_lane', lane), patch.object(outcomes, '_clipboard_reader'), \
             patch.object(outcomes, '_file_manager_window', return_value=('role', 'fm.txt', 'digest', 'window')), \
             patch.object(outcomes, '_right_click_file'), patch.object(outcomes, '_click_menu_entry'), \
             patch.object(outcomes.history, '_new_operation', return_value=dict(id='operation')), \
             patch.object(outcomes.tray, '_notice', return_value=dict(summary='Upload complete')), \
             patch.object(outcomes.tray, '_drawn_notice'), \
             patch.object(outcomes, '_complete', return_value=(detail, uploaded)), \
             patch.object(outcomes, '_auto_copy'), patch.object(outcomes, '_history') as history, \
             patch.object(outcomes, '_frame'):
            order = Mock()
            order.attach_mock(scope.stop, 'stop')
            order.attach_mock(history, 'history')
            outcomes.file_manager_upload(ctx)
        # Closed first, so the History frame shows the row.
        self.assertEqual(order.mock_calls, [call.stop('window'), call.history(ctx, detail)])

    def test_upload_oracles(self):
        self.upload_case()
        with self.assertRaisesRegex(RecipeFailed, 'SHA-256'):
            self.upload_case(uploaded='other')
        with self.assertRaisesRegex(RecipeFailed, 'one upload banner'):
            self.upload_case(notices=2)
        with self.assertRaisesRegex(RecipeFailed, 'names the right-clicked file'):
            self.upload_case(name='other.txt')

    def test_a_desktop_without_a_file_manager_is_blocked_not_green(self):
        ctx = self.context()
        with self.assertRaises(RecipeBlocked):
            outcomes.file_manager_upload(ctx)

    def test_a_running_file_manager_goes_first_and_only_the_owned_window_is_used(self):
        ctx = self.context()
        ctx.shell = Mock(side_effect=[dict(returncode=1), dict(returncode=0), dict(returncode=1)])
        ctx.wait = lambda predicate, what, **kwargs: next(v for v in iter(predicate, None) if v)
        ctx.keys = Mock()
        scope = SimpleNamespace(root='/synthetic', token='a'*32, write=Mock(), launch=Mock(), defer=Mock())
        with patch.object(outcomes.history, '_wait_window'):
            role, name, _, _ = outcomes._file_manager_window(ctx, scope, dict(process='nemo', command=['nemo'], quit=['nemo', '--quit'], view_keys=['Ctrl+2']))
        self.assertEqual(ctx.shell.call_args_list[0].args[0], 'nemo --quit')
        self.assertEqual(ctx.shell.call_count, 3)
        scope.launch.assert_called_once_with(['nemo', '/synthetic/fm-aaaaaaaa'], name='file-manager')
        self.assertEqual(name, 'fm-aaaaaaaa.txt')
        self.assertEqual(ctx.profile['windows'][role]['title'], 'fm\\-aaaaaaaa')
        ctx.keys.assert_called_once_with('Ctrl+2')

    def test_gnome_banners_turn_on_for_the_flow_and_back_off(self):
        ctx = self.context('wayland')
        ctx.shell = Mock(side_effect=[dict(returncode=0, stdout='false\n'), dict(returncode=0, stdout='')])
        scope = SimpleNamespace(defer=Mock())
        outcomes._show_banners(ctx, scope)
        self.assertTrue(ctx.shell.call_args_list[1].args[0].endswith('set org.gnome.desktop.notifications show-banners true'))
        ctx.shell = Mock(return_value=dict(returncode=0, stdout=''))
        scope.defer.call_args.args[1]()
        self.assertEqual(ctx.shell.call_args.kwargs, dict(cleanup=True))
        self.assertTrue(ctx.shell.call_args.args[0].endswith('show-banners false'))
        scope = SimpleNamespace(defer=Mock())
        ctx.shell = Mock(return_value=dict(returncode=0, stdout='true\n'))
        outcomes._show_banners(ctx, scope)
        scope.defer.assert_not_called()


class RecordingExports(unittest.TestCase):
    def clip(self, terminal, export_path=None):
        ctx = Mock()
        ctx.expect = ClipboardContext().expect
        scene = SimpleNamespace(rect=[320, 240, 640, 320], output_dir='/synthetic',
                                record_helper='/synthetic/helper', command=Mock())
        requested = []
        statuses = iter(('exporting', 'exporting', terminal, terminal))
        observed = []

        def call(method, params):
            self.assertEqual(method, 'recording.saveCopy')
            requested.append(params)

        def inspect(context, rid):
            if not requested:
                return {'target': {'rect': scene.rect}}
            status = next(statuses)
            observed.append(status)
            path = requested[-1]['path'] if export_path is None else export_path
            return {'lastExport': {'path': path, 'status': status}}

        def wait(predicate, what, **kwargs):
            if what == 'four seconds of actual capture':
                return True
            for _ in range(4):
                value = predicate()
                if value:
                    return value
            raise RecipeBlocked('synthetic export did not complete')

        ctx.call.side_effect = call
        ctx.wait.side_effect = wait
        with patch.object(outcomes.selectors, '_open_record', return_value='recording'), \
             patch.object(outcomes.selectors, '_inspect', side_effect=inspect), \
             patch.object(outcomes.selectors, '_start'), \
             patch.object(outcomes.selectors, '_bar_click'), \
             patch.object(outcomes.selectors, '_state', return_value={'durationUs': 5_000_000}), \
             patch.object(outcomes.selectors, '_cancel_record') as cancel, \
             patch.object(outcomes.selectors, '_retain'), \
             patch.object(outcomes, '_measure', return_value=(2.0, 5000)) as measure:
            if export_path is not None:
                with self.assertRaisesRegex(RecipeBlocked, 'synthetic export did not complete'):
                    outcomes._clip(ctx, scene, False)
                measure.assert_not_called()
                cancel.assert_called_once_with(ctx, 'recording')
            elif terminal == 'saved':
                self.assertEqual(outcomes._clip(ctx, scene, False), ('recording', 2.0))
                measure.assert_called_once()
                cancel.assert_not_called()
            else:
                with self.assertRaisesRegex(RecipeFailed, 'clip export succeeded'):
                    outcomes._clip(ctx, scene, False)
                measure.assert_not_called()
                cancel.assert_called_once_with(ctx, 'recording')
        self.assertEqual(observed, ['exporting', 'exporting', terminal] +
                         ([terminal] if export_path is not None else []))

    def test_waits_for_async_export_before_decoding_saved_copy(self):
        self.clip('saved')

    def test_saved_export_for_a_different_path_stays_blocked(self):
        self.clip('saved', '/synthetic/stale.mp4')

    def test_failed_async_export_still_fails_and_cancels_recording(self):
        self.clip('failed')


class ClipboardContext:
    def __init__(self, session='wayland'):
        self.profile = {'session': session}
        self._fixture_manager = SimpleNamespace(scope=object())
        self.notes = []

    def expect(self, value, message):
        if not value:
            raise RecipeFailed(message)

    def wait(self, predicate, what, **kwargs):
        if what in ('two seconds after Quit', 'two seconds after restart'):
            return True
        value = predicate()
        self.expect(value, 'timeout: '+what)
        return value

    def note(self, value):
        self.notes.append(value)

    def blocked(self, message):
        raise RecipeBlocked(message)


class ClipboardOracles(unittest.TestCase):
    detail = {'receipt': {'directUrl': 'http://127.0.0.1:1234/exact-object'}}

    def test_text_completion_uses_independent_object_hash_without_media_export(self):
        ctx = SimpleNamespace(note=Mock(), expect=ClipboardContext().expect,
            call=Mock(return_value={'id':'row','uploadStatus':'uploaded','local':{'status':'saved'}}))
        records = [{'id':'row','operationId':'operation'}]
        ctx.wait = Mock(side_effect=[{'state':'completed'}, records])
        with patch.object(outcomes, '_frame'), patch.object(outcomes, '_object', return_value='source-sha') as obj, \
             patch.object(outcomes, '_payload') as payload:
            detail, digest = outcomes._complete(SimpleNamespace(ctx=ctx), object(), 'operation', None)
        self.assertEqual(digest, 'source-sha')
        obj.assert_called_once()
        payload.assert_not_called()

    def test_wayland_text_reader_accepts_gnome_utf8_qualified_offer(self):
        for session, mime, selected in (('wayland', 'text/plain', 'text'),
                                        ('x11', 'text/plain', 'UTF8_STRING'),
                                        ('wayland', 'image/png', 'image/png')):
            scope = SimpleNamespace(ctx=ClipboardContext(session), json=Mock(return_value={'available':False,'data':''}))
            with patch.object(outcomes, '_clipboard_reader', return_value=['reader', '--type']):
                self.assertIsNone(outcomes._clipboard_bytes(scope, mime))
            self.assertIn(repr(['reader', '--type', selected]), scope.json.call_args.args[0])

    def test_auto_copy_requires_exact_independent_text_without_copy_action(self):
        ctx = ClipboardContext()
        for data in (None, b'', b'old clipboard', b'http://127.0.0.1:1234/wrong-object'):
            with patch.object(outcomes, '_clipboard_bytes', return_value=data):
                with self.assertRaises(RecipeFailed):
                    outcomes._auto_copy(ctx, object(), self.detail)
        with patch.object(outcomes, '_clipboard_bytes', return_value=self.detail['receipt']['directUrl'].encode()):
            outcomes._auto_copy(ctx, object(), self.detail)
        self.assertIn('no copy click', ctx.notes[-1])

    def exit_case(self, session, manager, after, restarted, processes=()):
        ctx = ClipboardContext(session)
        resident = SimpleNamespace(quit=Mock(), stopped=lambda: True, launch=Mock())
        @contextmanager
        def lane(ctx):
            yield object(), object()
        with patch.object(outcomes, '_lane', lane), patch.object(outcomes, '_capture', return_value=self.detail), \
             patch.object(outcomes, '_frame'), patch.object(outcomes, '_clipboard_handlers', return_value={
                 'x11ManagerWindow': manager, 'processes': list(processes), 'klipperBusOwner': None}), \
             patch.object(outcomes, '_clipboard_bytes', side_effect=[after, restarted]), \
             patch('tools.lab.surface_driver.fixtures.resident.lifecycle', return_value=resident):
            outcomes.survives_exit(ctx)
        resident.quit.assert_called_once()
        resident.launch.assert_called_once()
        return ctx

    def test_survival_and_restart_accept_exact_link_on_both_protocols(self):
        url = self.detail['receipt']['directUrl'].encode()
        for session, manager in (('wayland', None), ('x11', 123)):
            ctx = self.exit_case(session, manager, url, url)
            self.assertIn('survives_exit_exact_link_count=1', ctx.notes[-1])

    def test_no_manager_x11_loss_is_explicit_expected_result_with_documentation(self):
        with self.assertRaisesRegex(RecipeBlocked, 'Expected X11.*docs/desktops/cinnamon.md'):
            self.exit_case('x11', 0, None, None)

    def test_manager_loss_and_wayland_loss_are_real_oracle_failures(self):
        for session, manager in (('wayland', None), ('x11', 123)):
            with self.assertRaisesRegex(RecipeFailed, 'two seconds after Quit'):
                self.exit_case(session, manager, None, None)

    def test_x11_watcher_without_icccm_registration_still_blocks_expected_loss(self):
        with self.assertRaises(RecipeFailed):
            self.exit_case('x11', 0, None, None, [{'name': 'copyq', 'pid': 123}])

    def test_foreign_replacement_is_never_expected_x11_loss(self):
        with self.assertRaises(RecipeFailed):
            self.exit_case('x11', 0, b'foreign link', b'foreign link')

    def test_restart_clobbering_is_red_including_no_manager_x11(self):
        url = self.detail['receipt']['directUrl'].encode()
        for session, manager, after in (('wayland', None, url), ('x11', 123, url), ('x11', 0, None)):
            with self.assertRaisesRegex(RecipeFailed, 'restart does not clobber'):
                self.exit_case(session, manager, after, b'clobbered')


if __name__ == '__main__':
    unittest.main()
