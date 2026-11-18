#!/usr/bin/env python3
"""Review recipe contracts: kept-time arithmetic and the target-side verifier. Not acceptance evidence."""
from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.lab.surface_driver.recipes import review


class KeptSpans(unittest.TestCase):
    D = 4_500_000

    def test_table(self):
        cut = lambda a, b: {'start': a, 'end': b}
        cases = [
            ({'trimStart': 0, 'trimEnd': None, 'cuts': []}, [[0, self.D]]),
            ({'trimStart': 100, 'trimEnd': 900, 'cuts': []}, [[100, 900]]),
            ({'trimStart': 0, 'trimEnd': None, 'cuts': [cut(1_000_000, 2_000_000)]}, [[0, 1_000_000], [2_000_000, self.D]]),
            # trim and two cuts, one given out of order and one overlapping the trim start
            ({'trimStart': 500_000, 'trimEnd': 4_000_000, 'cuts': [cut(3_000_000, 3_500_000), cut(0, 700_000)]},
             [[700_000, 3_000_000], [3_500_000, 4_000_000]]),
            ({'trimStart': 0, 'trimEnd': 1000, 'cuts': [cut(0, 1000)]}, []),
        ]
        for edit, expected in cases:
            with self.subTest(edit=edit):
                self.assertEqual(review.kept_spans(edit, self.D), expected)


class Verifier(unittest.TestCase):
    def test_target_script_compiles(self):
        compile(review._MEDIA_CHECK, 'review-media-check', 'exec')

    def test_saved_copy_actions_ignore_failure_actions(self):
        state = {'statusFacts': [{'actions': [{'drive': {'action': 'retryExport'}}, {'drive': {'action': 'dismissStatus'}}]},
                                 {'actions': [{'drive': {'action': 'copyPath'}}, {'drive': {'action': 'dismissSavedCopy'}}]}]}
        self.assertEqual(review._saved_copy_actions(state), {'copyPath', 'dismissSavedCopy'})
        state['statusFacts'].pop()
        self.assertEqual(review._saved_copy_actions(state), set())


if __name__ == '__main__':
    unittest.main()
