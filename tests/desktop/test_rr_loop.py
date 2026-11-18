"""rr_loop records the CTest test it was asked for and no other: a wrong pick would record a test nobody chose and report it as that one."""
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'lab'))
import rr_loop  # noqa: E402


def listing(*names):
    return json.dumps({'tests': [{'name': name, 'command': ['/bin/true'], 'properties': []} for name in names]})


class PickTest(unittest.TestCase):
    def test_a_dot_in_the_name_is_not_a_wildcard(self):
        with self.assertRaises(SystemExit):
            rr_loop.pick_test(listing('unit_v2'), 'unit.v2')

    def test_the_exact_name_is_picked(self):
        self.assertEqual(rr_loop.pick_test(listing('unit_v2', 'unit.v2', 'unit.v2_extra'), 'unit.v2')['name'], 'unit.v2')


if __name__ == '__main__':
    unittest.main()
