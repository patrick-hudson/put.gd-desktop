import itertools
import unittest
from score import distance, normalize, score, extract


def oracle(a, b):
    row = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        nxt = [i]
        for j, y in enumerate(b, 1):
            nxt.append(min(nxt[-1]+1, row[j]+1, row[j-1]+(x != y)))
        row = nxt
    return row[-1]


class Scoring(unittest.TestCase):
    def test_distance_against_dynamic_program(self):
        strings = [''.join(s) for n in range(5) for s in itertools.product('aé', repeat=n)]
        for a in strings:
            for b in strings:
                self.assertEqual(distance(a, b), oracle(a, b))

    def test_small_entities_are_outside_exact_match_bar(self):
        capture=dict(id='tiny', **{'class':'interface'}, font_pixels=10, urls=['https://example.invalid'])
        self.assertEqual(score(capture,'sample','sample')['urls']['total'],0)

    def test_normalization_and_case(self):
        self.assertEqual(normalize(' e\u0301\n\t A  '), 'é A')
        self.assertEqual(distance(normalize('A'), normalize('a')), 1)

    def test_exact_entities_and_occurrence_counts(self):
        capture = dict(id='one', **{'class': 'body'}, font_pixels=12,
                       urls=['https://example.invalid/a', 'https://example.invalid/a'],
                       emails=['lab@example.invalid'])
        result = score(capture, 'hello', 'https://example.invalid/ab https://example.invalid/a lab@example.invalid')
        self.assertEqual(result['urls']['exact'], 1)
        self.assertEqual(result['emails']['rate'], 1)
        self.assertEqual(extract({'ok': True, 'result': {'text': 'hello'}}), 'hello')


if __name__ == '__main__':
    unittest.main()
