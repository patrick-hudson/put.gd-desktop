"""Fault-injection checks for Tier 1's independent result oracles."""
from contextlib import contextmanager, nullcontext
import io
from pathlib import Path
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch
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

    def test_ten_recipes_reference_existing_interactions(self):
        recipes = discover('outcomes')
        self.assertEqual(len(recipes), 10)
        validate_recipes(recipes)
        self.assertTrue(all(r.destinations == ('local',) for r in recipes))


class RecordingExports(unittest.TestCase):
    def clip(self, terminal):
        ctx = Mock()
        ctx.expect = ClipboardContext().expect
        scene = SimpleNamespace(rect=[320, 240, 640, 320], output_dir='/synthetic',
                                record_helper='/synthetic/helper', command=Mock())
        requested = []
        statuses = iter(('exporting', 'exporting', terminal))
        observed = []

        def call(method, params):
            self.assertEqual(method, 'recording.saveCopy')
            requested.append(params)

        def inspect(context, rid):
            if not requested:
                return {'target': {'rect': scene.rect}}
            status = next(statuses)
            observed.append(status)
            return {'lastExport': {'path': requested[-1]['path'], 'status': status}}

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
            if terminal == 'saved':
                self.assertEqual(outcomes._clip(ctx, scene, False), ('recording', 2.0))
                measure.assert_called_once()
                cancel.assert_not_called()
            else:
                with self.assertRaisesRegex(RecipeFailed, 'clip export succeeded'):
                    outcomes._clip(ctx, scene, False)
                measure.assert_not_called()
                cancel.assert_called_once_with(ctx, 'recording')
        self.assertEqual(observed, ['exporting', 'exporting', terminal])

    def test_waits_for_async_export_before_decoding_saved_copy(self):
        self.clip('saved')

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
