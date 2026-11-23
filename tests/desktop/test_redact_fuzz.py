"""The sensitive-text fuzz harness's scorer (#1931) on in-memory scenes: no Pango, no helper, no driver.

A scorer that called a visible secret covered, or an extra cover a true one, would hide exactly the
failures the harness exists to find, so these pin how marks are compared with the labels.
"""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab' / 'redact_fuzz'))
import content  # noqa: E402
import fuzz  # noqa: E402


def chars(x, y, n, w=10, h=14):
    return [[x + i * w, y, x + (i + 1) * w, y + h] for i in range(n)]


def span(label, kind, text, x, y, tag=''):
    c = chars(x, y, len(text))
    return {'label': label, 'kind': kind, 'tag': tag, 'text': text, 'pieces': [[c[0][0], y, c[-1][2], y + 14]], 'chars': c}


def meta(spans, words=()):
    return {'id': 't-00000', 'families': ['test'], 'variant': '', 'theme': 'light', 'scale': 1.0, 'mono': 'Mono', 'ui': 'Sans',
            'mono_size': 13, 'ui_size': 13, 'login': 'jdoe', 'realName': 'Jane Doe', 'spans': spans,
            'words': [[s['text'], s['pieces'][0][0], s['pieces'][0][1], s['pieces'][0][2] - s['pieces'][0][0], 14] for s in spans] + list(words)}


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

    def test_one_character_the_true_words_leave_visible_does_not_blame_the_rules(self):
        m = meta([span('secret', 'key', 'ABCDEF', 100, 50)])
        found = fuzz.score_scene(m, result([], [match('key', 'ABCDEF', (95, 46, 55, 22))]), 'all')  # the oracle covers 5 of 6
        self.assertEqual([(f['type'], f['cause'], f['by_rule']) for f in found], [('FN', 'ocr', 1)])

    def test_a_partly_hidden_secret_is_not_scored(self):
        s = span('secret', 'key', 'ABCDEFGHIJ', 100, 50)
        s['clipped'] = True
        self.assertEqual(fuzz.score_scene(meta([s]), result([]), 'all'), [])

    def test_a_cover_over_a_look_alike_is_a_false_positive(self):
        m = meta([span('decoy', 'calendar_day', '17 18 19 20 21 22', 100, 50)])
        found = fuzz.score_scene(m, result([match('phone', '17 18 19 20 21 22', (95, 46, 180, 22))]), 'all')
        self.assertEqual([(f['type'], f['category'], f['decoy'], f['cause']) for f in found], [('FP', 'phone', 'calendar_day', 'ocr')])

    def test_rows_of_a_true_match_beyond_its_secret_are_over_cover(self):
        m = meta([span('secret', 'key', 'ABCDEFGHIJ', 100, 50)])
        found = fuzz.score_scene(m, result([match('key', 'ABCDEFGHIJ more', (95, 46, 110, 22), (95, 80, 300, 22))]), 'all')
        self.assertEqual([f['type'] for f in found], ['OVER'])

    def test_any_ip_and_the_users_own_name_are_covered_by_design(self):
        m = meta([], [['1.1.1.1', 12, 12, 56, 14], ['Jane,', 12, 42, 40, 14]])
        found = fuzz.score_scene(m, result([match('ip', '1.1.1.1', (10, 10, 60, 20)), match('name', 'Jane', (10, 40, 44, 20))]), 'all')
        self.assertEqual([(f['type'], f['decoy']) for f in found], [('DESIGN', 'ip_address'), ('DESIGN', 'name_part')])

    def test_ordinary_text_misread_as_an_ip_stays_a_false_positive(self):
        for word in ('l.l.l.l', '12:34:56'):
            m = meta([], [[word, 12, 12, 56, 14]])
            found = fuzz.score_scene(m, result([match('ip', '1.1.1.1', (10, 10, 60, 20))]), 'all')
            self.assertEqual([f['type'] for f in found], ['FP'], word)

    def test_an_ipv6_address_with_a_port_or_zone_is_an_address(self):
        for word in ('[2001:db8::1]:443', 'fe80::3e22:fbff:fe9a:1e07%wlp2s0', '::1', '10.0.0.5:6443'):
            self.assertTrue(fuzz._ip_literal(word), word)

    def test_the_upload_check_profile_skips_kinds_it_does_not_look_for(self):
        m = meta([span('secret', 'email', 'jane@example.com', 100, 50), span('secret', 'card', '4111 1111 1111 1111', 100, 90)])
        res = {'id': 't-00000', 'words': [], 'ocr': {'check': {'matches': []}}, 'oracle': {'check': {'matches': []}}}
        self.assertEqual([f['kind'] for f in fuzz.score_scene(m, res, 'check')], ['card'])


class Run(unittest.TestCase):
    def test_a_new_shard_never_truncates_one_already_there(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            (out / 'shard-0000.jsonl').write_text('a\n')
            (out / 'shard-0002.jsonl').write_text('b\n')
            with fuzz.new_shard(out) as fh:
                name = Path(fh.name).name
            self.assertNotIn(name, ('shard-0000.jsonl', 'shard-0002.jsonl'))
            self.assertEqual((out / 'shard-0002.jsonl').read_text(), 'b\n')

    def test_a_failed_scene_is_run_again_then_given_up(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            rows = [{'id': 'a', 'ocr': {}, 'oracle': {}}, {'id': 'b', 'error': 'text_job_timeout'}, {'id': 'c', 'oracle': {}}]
            (out / 'shard-0000.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))
            self.assertEqual(fuzz.completed(out, False, 1), ({'a'}, set()))
            self.assertEqual(fuzz.completed(out, True, 1), ({'a', 'c'}, set()))
            (out / 'shard-0001.jsonl').write_text(json.dumps(rows[1]) + '\n')
            self.assertEqual(fuzz.completed(out, False, 1), ({'a'}, {'b'}))  # given up on, never counted a success

    def test_a_relative_image_path_from_an_older_corpus_still_finds_its_image(self):
        with tempfile.TemporaryDirectory() as d:
            scenes = Path(d) / 'corpus'
            (scenes / 'images').mkdir(parents=True)
            (scenes / 'images' / 's.png').write_bytes(b'x')
            for recorded in ('images/s.png', 'corpus/images/s.png', '/gone/corpus/images/s.png'):
                self.assertEqual(fuzz.scene_image(scenes, {'image': recorded}), (scenes / 'images' / 's.png').resolve(), recorded)

    def test_a_seed_never_becomes_a_path_and_two_seeds_never_share_a_name(self):
        for seed in ('../../etc/x', '/abs', 'lab/a'):
            self.assertRegex(fuzz.safe_name(seed), r'^[A-Za-z0-9_][A-Za-z0-9_.-]*$')
        self.assertEqual(fuzz.safe_name('a1'), 'a1')
        self.assertNotEqual(fuzz.safe_name('lab/a'), fuzz.safe_name('lab_a'))


class ScoreRun(unittest.TestCase):
    def test_a_failed_duplicate_never_hides_a_complete_result(self):
        with tempfile.TemporaryDirectory() as d:
            scenes = Path(d)
            (scenes / 'labels.jsonl').write_text(json.dumps(meta([span('secret', 'key', 'ABCDEFGHIJ', 100, 50)])) + '\n')
            res = scenes / 'results'
            res.mkdir()
            whole = {'id': 't-00000', 'words': [], 'ocr': {'all': {'matches': []}, 'check': {'matches': []}},
                     'oracle': {'all': {'matches': []}, 'check': {'matches': []}}}
            (res / 'shard-0002.jsonl').write_text(json.dumps(whole) + '\n')
            (res / 'shard-0003.jsonl').write_text(json.dumps({'id': 't-00000', 'error': 'text_job_timeout'}) + '\n')
            args = type('A', (), dict(scenes=str(scenes), results=None, oracle=False, out=str(scenes / 'score'), top=0))
            import contextlib, io
            with contextlib.redirect_stdout(io.StringIO()):
                fuzz.score(args)
            totals = json.loads((scenes / 'score' / 'summary.json').read_text())['totals']
            self.assertEqual((totals['scenes'], totals['missing'], totals['incomplete']), (1, 0, 0))


class Content(unittest.TestCase):
    def test_generated_card_numbers_pass_luhn(self):
        import random
        R = random.Random(1931)
        for _ in range(200):
            self.assertTrue(content.luhn_ok(content.card_digits(R)))


if __name__ == '__main__':
    unittest.main()
