"""The sensitive-text fuzz harness's scorer (#1931) on in-memory scenes: no Pango, no helper, no driver.

A scorer that called a visible secret covered, or an extra cover a true one, would hide exactly the
failures the harness exists to find, so these pin how marks are compared with the labels.
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab' / 'redact_fuzz'))
import content  # noqa: E402
import fuzz  # noqa: E402


def chars(x, y, n, w=10, h=14):
    return [[x + i * w, y, x + (i + 1) * w, y + h] for i in range(n)]


def span(label, kind, text, x, y, tag=''):
    c = chars(x, y, len(text))
    return {'label': label, 'kind': kind, 'tag': tag, 'text': text, 'pieces': [[c[0][0], y, c[-1][2], y + 14]], 'chars': c}


def meta(spans):
    return {'id': 't-00000', 'families': ['test'], 'variant': '', 'theme': 'light', 'scale': 1.0, 'mono': 'Mono', 'ui': 'Sans',
            'mono_size': 13, 'ui_size': 13, 'login': 'jdoe', 'realName': 'Jane Doe', 'spans': spans,
            'words': [[s['text'], s['pieces'][0][0], s['pieces'][0][1], s['pieces'][0][2] - s['pieces'][0][0], 14] for s in spans]}


def result(ocr_matches, oracle_matches=()):
    return {'id': 't-00000', 'words': [], 'ocr': {'all': {'matches': list(ocr_matches)}}, 'oracle': {'all': {'matches': list(oracle_matches)}}}


def match(category, text, *rects):
    return {'category': category, 'text': text, 'words': [0], 'marks': [[list(r), list(r)] for r in rects]}


class Score(unittest.TestCase):
    def test_a_covered_secret_is_no_finding(self):
        m = meta([span('secret', 'email', 'jane@example.com', 100, 50)])
        found = fuzz.score_scene(m, result([match('email', 'jane@example.com', (95, 46, 170, 22))]), 'all')
        self.assertEqual(found, [])

    def test_a_cover_that_leaves_characters_visible_is_partial(self):
        m = meta([span('secret', 'key', 'ABCDEFGHIJ', 100, 50)])
        found = fuzz.score_scene(m, result([match('key', 'ABCDEFGHIJ', (95, 46, 55, 22))]), 'all')
        self.assertEqual([(f['type'], f['visible'], f['chars']) for f in found], [('PARTIAL', 5, 10)])

    def test_a_miss_is_the_rules_fault_only_when_the_true_words_miss_too(self):
        m = meta([span('secret', 'card', '4111 1111 1111 1111', 100, 50)])
        by_rule = fuzz.score_scene(m, result([]), 'all')
        by_reading = fuzz.score_scene(m, result([], [match('card', '4111 1111 1111 1111', (95, 46, 200, 22))]), 'all')
        self.assertEqual([(f['type'], f['cause']) for f in by_rule], [('FN', 'rule')])
        self.assertEqual([(f['type'], f['cause']) for f in by_reading], [('FN', 'ocr')])

    def test_a_cover_over_a_look_alike_is_a_false_positive(self):
        m = meta([span('decoy', 'calendar_day', '17 18 19 20 21 22', 100, 50)])
        found = fuzz.score_scene(m, result([match('phone', '17 18 19 20 21 22', (95, 46, 180, 22))]), 'all')
        self.assertEqual([(f['type'], f['category'], f['decoy'], f['cause']) for f in found], [('FP', 'phone', 'calendar_day', 'ocr')])

    def test_rows_of_a_true_match_beyond_its_secret_are_over_cover(self):
        m = meta([span('secret', 'key', 'ABCDEFGHIJ', 100, 50)])
        found = fuzz.score_scene(m, result([match('key', 'ABCDEFGHIJ more', (95, 46, 110, 22), (95, 80, 300, 22))]), 'all')
        self.assertEqual([f['type'] for f in found], ['OVER'])

    def test_any_ip_and_the_users_own_name_are_covered_by_design(self):
        m = meta([])
        found = fuzz.score_scene(m, result([match('ip', '1.1.1.1', (10, 10, 60, 20)), match('name', 'Jane', (10, 40, 40, 20))]), 'all')
        self.assertEqual([f['type'] for f in found], ['DESIGN'])

    def test_the_upload_check_profile_skips_kinds_it_does_not_look_for(self):
        m = meta([span('secret', 'email', 'jane@example.com', 100, 50), span('secret', 'card', '4111 1111 1111 1111', 100, 90)])
        res = {'id': 't-00000', 'words': [], 'ocr': {'check': {'matches': []}}, 'oracle': {'check': {'matches': []}}}
        self.assertEqual([f['kind'] for f in fuzz.score_scene(m, res, 'check')], ['card'])


class Content(unittest.TestCase):
    def test_generated_card_numbers_pass_luhn(self):
        import random
        R = random.Random(1931)
        for _ in range(200):
            self.assertTrue(content.luhn_ok(content.card_digits(R)))


if __name__ == '__main__':
    unittest.main()
