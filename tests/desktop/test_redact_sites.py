"""The real-website sweep's triage and report (#1975) on in-memory results: no browser, no helper, no driver.

A report that counted a capture with a false prompt as clean, or a rule that judged the wrong page,
would make the before-and-after table say a fix worked when it did not.
"""
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab' / 'redact_sites'))
import sweep  # noqa: E402


def match(category, text):
    return {'category': category, 'text': text, 'words': [0], 'marks': []}


class Triage(unittest.TestCase):
    def rules(self, text):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'triage.tsv'
            p.write_text(text)
            return sweep.read_triage(str(p))

    def test_the_first_rule_that_matches_decides(self):
        rules = self.rules('# comment\nkey\turl=jwt\\.io text=.\ttrue\tsample\nkey\t.\tfalse\tnoise\n')
        self.assertEqual(sweep.triage(rules, 'key', 'eyJhbGci', 'https://jwt.io/'), ('true', 'sample'))
        self.assertEqual(sweep.triage(rules, 'key', 'eyJhbGci', 'https://example.org/'), ('false', 'noise'))

    def test_a_rule_holds_only_for_its_kind_and_unmatched_text_is_untriaged(self):
        rules = self.rules('phone\t^19\\d\\d-\tfalse\tyears\n')
        self.assertEqual(sweep.triage(rules, 'phone', '1995-2026', 'https://x/'), ('false', 'years'))
        self.assertEqual(sweep.triage(rules, 'card', '1995-2026', 'https://x/'), ('untriaged', ''))


class Report(unittest.TestCase):
    def test_a_capture_counts_as_false_when_any_match_is_not_true(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            shots = [dict(id='000-ch-100-l', url='https://a/', category='news', browser='chromium', scale=1.0, theme='light', png='shots/a.png'),
                     dict(id='001-ch-100-l', url='https://b/', category='news', browser='chromium', scale=1.0, theme='light', png='shots/b.png'),
                     dict(id='002-ch-100-l', url='https://c/', category='news', browser='chromium', scale=1.0, theme='light', png='shots/c.png')]
            (d / 'shots.jsonl').write_text(''.join(json.dumps(s) + '\n' for s in shots))
            results = [{'id': '000-ch-100-l', 'ocr': {'default': {'matches': [match('key', 'TRUEKEY'), match('key', 'noise')]}, 'all': {'matches': []}}},
                       {'id': '001-ch-100-l', 'ocr': {'default': {'matches': [match('key', 'TRUEKEY')]}, 'all': {'matches': []}}},
                       {'id': '002-ch-100-l', 'ocr': {'default': {'matches': []}, 'all': {'matches': []}}}]
            (d / 'res.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in results))
            (d / 'triage.tsv').write_text('key\tTRUEKEY\ttrue\treal\n')
            args = types.SimpleNamespace(shots=[str(d)], results=[str(d / 'res.jsonl')], triage=str(d / 'triage.tsv'), out=str(d / 'rep'))
            sweep.report(args)
            summary = json.loads((d / 'rep' / 'summary.json').read_text())
        self.assertEqual(summary['table']['default@1.0'], dict(captures=3, prompted=2, false=1, true_only=1, incomplete=0))
        self.assertEqual(summary['pages']['default']['false'], 1)
        self.assertEqual(summary['groups'], {'default:untriaged:key': 1})


    def test_a_capture_the_check_could_not_finish_counts_as_a_prompt(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            shot = dict(id='000-ch-100-l', url='https://a/', category='news', browser='chromium', scale=1.0, theme='light', png='shots/a.png')
            (d / 'shots.jsonl').write_text(json.dumps(shot) + '\n')
            (d / 'res.jsonl').write_text(json.dumps({'id': '000-ch-100-l', 'truncated': True, 'ocr': {'default': {'matches': []}, 'all': {'matches': []}}}) + '\n')
            args = types.SimpleNamespace(shots=[str(d)], results=[str(d / 'res.jsonl')], triage='', out=str(d / 'rep'))
            sweep.report(args)
            summary = json.loads((d / 'rep' / 'summary.json').read_text())
        self.assertEqual(summary['table']['default@1.0'], dict(captures=1, prompted=1, false=1, true_only=0, incomplete=1))
        self.assertEqual(summary['groups']['default:incomplete: truncated'], 1)

if __name__ == '__main__':
    unittest.main()
