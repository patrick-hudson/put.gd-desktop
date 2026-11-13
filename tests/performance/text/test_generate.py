import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('text_generate',Path(__file__).resolve().parents[3]/'tools/lab/text_corpus/generate.py')
generator=importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)


class Synthetic(unittest.TestCase):
    def test_luhn_fixture(self):
        digits=[int(x) for x in generator.card()[::-1]]
        self.assertEqual(sum(d if i%2==0 else (2*d if d<5 else 2*d-9) for i,d in enumerate(digits))%10,0)

    def test_all_categories_and_reserved_networks(self):
        items=generator.sensitive()
        self.assertEqual({c for c,t in items},{'email','phone','address','card','token'})
        self.assertIn(('address','192.0.2.42'),items)
        self.assertIn(('address','2001:db8::42'),items)
        self.assertIn(('email','corpus@example.invalid'),items)
