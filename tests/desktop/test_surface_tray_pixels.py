#!/usr/bin/env python3
"""Finding put.gd's tray icon and its open menu by pixels (tools/lab/surface_driver/tray_pixels.py).

The glyph frames under fixtures/tray/glyphs are what a resident published as its StatusNotifier
IconPixmap (turkey, tree and mark; dark and light panel tints), dumped from a lab guest with
PUTGD_TRAY_CALENDAR_NOW. The matcher itself never reads stored images: in a run, its template is
read from the running resident. Fast host tests: no app, desktop or VM.
"""
from pathlib import Path
import sys
import unittest

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver import tray_pixels as tp

GLYPHS = ROOT/'tests/desktop/fixtures/tray/glyphs'
SEASONS = ('turkey', 'tree', 'mark')
PANELS = {'dark': (28, 28, 32), 'light': (232, 232, 228)}


def frames(glyph, tint):
    """A resident's published frames, as the recipe reads them (cut to the glyph)."""
    found = sorted(GLYPHS.glob(f'{glyph}-{tint}-*.png'), key=lambda p: int(p.stem.rsplit('-', 1)[1]))
    assert found, f'no {glyph}-{tint} frames'
    return [tp.glyph(Image.open(p).convert('RGBA')) for p in found]


def largest(glyph, tint):
    return max((Image.open(p).convert('RGBA') for p in GLYPHS.glob(f'{glyph}-{tint}-*.png')), key=lambda i: i.width)


def symbolic(frame, colour):
    """What a host that recolours tray pixmaps draws: every opaque pixel one colour."""
    flat = Image.new('RGBA', frame.size, colour + (255,))
    flat.putalpha(frame.getchannel('A'))
    return flat


def panel(tint, width=640, height=36, distractors=()):
    image = Image.new('RGB', (width, height), PANELS[tint])
    ink = (214, 214, 210) if tint == 'dark' else (40, 40, 44)
    draw = ImageDraw.Draw(image)
    # Other tray glyphs and the clock: a bell-like blob, a square outline and digits-like strokes.
    draw.ellipse((40, 9, 58, 27), fill=ink)
    draw.rectangle((90, 8, 110, 28), outline=ink, width=2)
    for x in range(560, 620, 12):
        draw.rectangle((x, 10, x + 7, 26), outline=ink, width=2)
    for x, picture in distractors:
        image.paste(picture, (x, (height - picture.height) // 2), picture)
    return image


def drawn(frame, size, recolour=None):
    picture = frame.resize((size, size), Image.LANCZOS)
    return symbolic(picture, recolour) if recolour else picture


class LocateTheIcon(unittest.TestCase):
    def test_every_season_on_both_panel_tints_at_host_sizes_and_recoloured(self):
        for glyph in SEASONS:
            for tint in PANELS:
                published = frames(glyph, tint)
                source = largest(glyph, tint)
                others = [largest(g, tint) for g in SEASONS if g != glyph]
                grey = (214, 214, 210) if tint == 'dark' else (50, 50, 50)
                for size in (16, 22, 24, 32):
                    for recolour in (None, grey):
                        with self.subTest(glyph=glyph, tint=tint, size=size, recolour=bool(recolour)):
                            target = drawn(source, size, recolour)
                            image = panel(tint, distractors=[(200, drawn(others[0], size, recolour)),
                                                             (300, target),
                                                             (400, drawn(others[1], size, recolour))])
                            box = (0, 800 - 36, 640, 800)
                            screen = Image.new('RGB', (640, 800), (90, 30, 60))
                            screen.paste(image, (0, box[1]))
                            found = tp.locate(screen, box, published)
                            # The glyph's own outline inside the frame, where the host drew it.
                            gx, gy, gx1, gy1 = target.getchannel('A').point(lambda a: 255 if a >= 64 else 0).getbbox()
                            top = box[1] + (36 - target.height) // 2
                            expected = [300 + gx, top + gy, gx1 - gx, gy1 - gy]
                            # The click goes to the centre; a dark outline on a dark panel draws no contrast,
                            # so the found box may be a little smaller than the glyph's alpha.
                            centre = lambda r: (r[0] + r[2] / 2, r[1] + r[3] / 2)
                            self.assertTrue(all(abs(a - b) <= 2 for a, b in zip(centre(found['rect']), centre(expected))),
                                            (found, expected))
                            self.assertGreaterEqual(found['score'], tp.MIN_SCORE)
                            self.assertLess(found['runner_up'], found['score'] - tp.MARGIN)

    def test_two_copies_are_ambiguous_and_none_is_not_found(self):
        published = frames('turkey', 'dark')
        source = largest('turkey', 'dark')
        twice = panel('dark', distractors=[(200, drawn(source, 24)), (300, drawn(source, 24))])
        with self.assertRaisesRegex(tp.NoMatch, 'ambiguous'):
            tp.locate(twice, (0, 0, 640, 36), published)
        # Another season's glyph is not put.gd's icon today.
        other = panel('dark', distractors=[(300, drawn(largest('tree', 'dark'), 24))])
        with self.assertRaisesRegex(tp.NoMatch, 'no drawn panel glyph'):
            tp.locate(other, (0, 0, 640, 36), published)
        with self.assertRaisesRegex(tp.NoMatch, 'no icon frames'):
            tp.locate(other, (0, 0, 640, 36), [None])

    def test_a_second_shape_above_the_cutoff_is_ambiguous_however_far_behind(self):
        from unittest.mock import patch
        image = panel('dark', distractors=[(200, drawn(largest('turkey', 'dark'), 24)),
                                           (300, drawn(largest('turkey', 'dark'), 24))])
        scores = iter([0.95, 0.81] + [0.1] * 20)
        with patch.object(tp, 'similarity', side_effect=lambda *a: next(scores)):
            with self.assertRaisesRegex(tp.NoMatch, 'ambiguous'):
                tp.locate(image, (0, 0, 640, 36), frames('turkey', 'dark'))

    def test_a_panel_border_line_does_not_swallow_the_icon(self):
        published = frames('mark', 'light')
        image = panel('light', distractors=[(300, drawn(largest('mark', 'light'), 22))])
        ImageDraw.Draw(image).line((0, 1, 639, 1), fill=(150, 150, 150))
        self.assertTrue(300 <= tp.locate(image, (0, 0, 640, 36), published)['rect'][0] <= 306)

    def test_still_drawn_fails_once_something_covers_or_moves_the_icon(self):
        published = frames('turkey', 'light')
        image = panel('light', distractors=[(300, drawn(largest('turkey', 'light'), 24))])
        found = tp.locate(image, (0, 0, 640, 36), published)
        self.assertGreaterEqual(tp.still_drawn(image, found['rect'], published), tp.MIN_SCORE)
        covered = image.copy()
        ImageDraw.Draw(covered).rectangle((295, 0, 340, 36), fill=(255, 255, 255))
        self.assertEqual(tp.still_drawn(covered, found['rect'], published), 0.0)
        moved = panel('light', distractors=[(310, drawn(largest('turkey', 'light'), 24))])
        self.assertEqual(tp.still_drawn(moved, found['rect'], published), 0.0)

    def test_frames_come_from_the_published_bytes(self):
        frame = Image.new('RGBA', (2, 1), (0, 0, 0, 0))
        frame.putpixel((1, 0), (10, 20, 30, 255))
        argb = bytes([0, 0, 0, 0, 255, 10, 20, 30])
        self.assertEqual([tp.pixmap_image(2, 1, argb).getpixel((x, 0)) for x in (0, 1)], [frame.getpixel((x, 0)) for x in (0, 1)])
        with self.assertRaises(ValueError):
            tp.pixmap_image(2, 2, argb)
        self.assertIsNone(tp.glyph(Image.new('RGBA', (4, 4))))


class TheOpenMenu(unittest.TestCase):
    def test_menu_box_is_the_popup_by_the_icon_not_a_banner_or_the_panel(self):
        before = Image.new('RGB', (1280, 800), (40, 40, 60))
        after = before.copy()
        draw = ImageDraw.Draw(after)
        draw.rectangle((1000, 300, 1250, 759), fill=(250, 250, 250))  # the menu, above a bottom panel
        draw.rectangle((900, 20, 1270, 90), fill=(200, 0, 0))        # a banner far from it
        draw.rectangle((0, 770, 1280, 800), fill=(0, 0, 0))          # the panel itself changed
        box = tp.menu_box(before, after, (0, 760, 1280, 800), (1100, 768, 23, 23))
        self.assertIsNotNone(box)
        self.assertLessEqual(abs(box[0] - 1000), 4)
        self.assertLessEqual(abs(box[3] - 760), 4)
        self.assertIsNone(tp.menu_box(before, before, (0, 760, 1280, 800), (1100, 768, 23, 23)))

    def test_rows_align_in_order_with_wildcards_shortcuts_and_lookalikes(self):
        tsv = ['level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext']
        rows = ['Capture region Ctrl+Shift+1', 'Repeat last region', 'Capture region & edit', 'Capture delay: 5 seconds',
                'Capture output: Local only', 'Quit put.gd']
        for n, row in enumerate(rows):
            x = 10
            for word in row.split():
                tsv.append(f'5\t1\t1\t1\t{n + 1}\t1\t{x}\t{20 + 30 * n}\t{8 * len(word)}\t12\t95\t{word}')
                x += 8 * len(word) + 6
        lines = tp.ocr_lines('\n'.join(tsv))
        self.assertEqual([l['text'] for l in lines], rows)
        labels = ['Capture region', 'Repeat last region', 'Capture window', 'Capture region & edit',
                  'Capture delay: *', 'Capture output: *', 'Quit put.gd']
        matched = tp.align(labels, lines)
        self.assertEqual([m and m['text'] for m in matched],
                         [rows[0], rows[1], None, rows[2], rows[3], rows[4], rows[5]])
        self.assertEqual(matched[0]['rect'][1], 20)
        # Every line read must be an agreed row; a row read twice in overlapping views still is.
        self.assertEqual(tp.unknown_lines(labels, lines + [lines[3], dict(text='›', rect=[0, 0, 1, 1])]), [])
        self.assertEqual(tp.unknown_lines(labels, lines + [dict(text='Pin to bar', rect=[0, 0, 1, 1])]), ['Pin to bar'])
        self.assertEqual(tp.unknown_lines(labels, [dict(text='&&&', rect=[0, 0, 1, 1]), dict(text='設定項目', rect=[0, 0, 1, 1]),
                                                   dict(text='Ctrl+Shift+4', rect=[0, 0, 1, 1])]), ['設定項目'])
        # A scrolled view's row cut by the card's top edge is not a row of its own when the next line
        # was also read in the first view; any other unknown line still counts.
        box = (0, 10, 300, 400)
        first = [dict(text=t, rect=[10, 20 + 30 * i, 100, 12]) for i, t in enumerate(['Capture region', 'Repeat last region', 'Capture window'])]
        view = [dict(text='vapLul e usencup', rect=[10, 12, 100, 8]), dict(text='Capture window', rect=[10, 40, 100, 12]),
                dict(text='Quit put.gd', rect=[10, 380, 100, 12])]
        self.assertIs(tp.clipped_top(labels, view, box, first), view[0])
        self.assertIsNone(tp.clipped_top(labels, view, box, first[:2]))  # the next line wasn't in the first view
        self.assertIsNone(tp.clipped_top(labels, [dict(view[0], rect=[10, 60, 100, 12])] + view[1:], box, first))  # not at the edge
        # Never at the bottom edge, which shows rows the first view didn't.
        bottom = [view[1], dict(text='Pin to bar', rect=[10, 385, 100, 12])]
        self.assertIsNone(tp.clipped_top(labels, bottom, box, first))
        self.assertEqual(tp.unknown_lines(labels, bottom), ['Pin to bar'])
        # The lookalike alone is not read as Capture region.
        self.assertEqual(tp.align(['Capture region'], [lines[2]]), [None])


if __name__ == '__main__':
    unittest.main()
