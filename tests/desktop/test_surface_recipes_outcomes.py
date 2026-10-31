"""Fault-injection checks for Tier 1's independent result oracles."""
from pathlib import Path
import sys
import unittest
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.lab.lab import FIXTURE_RGB
from tools.lab.surface_driver.recipes.outcomes import edited_pixels, pattern_visible, pixels_equal
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


if __name__ == '__main__':
    unittest.main()
