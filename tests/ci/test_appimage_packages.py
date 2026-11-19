"""tools/ci/appimage-packages.txt reaches apt as package names only, in every reader (#1652)."""
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
LIST = ROOT / 'tools/ci/appimage-packages.txt'
READER = "sed 's/#.*//'"


def read(text):
    return subprocess.run(['bash', '-c', f"{READER} \"$1\"", '_', '/dev/stdin'], input=text, text=True,
                          stdout=subprocess.PIPE, check=True).stdout.split()


class AppimagePackages(unittest.TestCase):
    def test_comments_run_to_the_end_of_the_line_and_a_comment_line_adds_nothing(self):
        self.assertEqual(read('# header\npatchelf # needed for bundling\nnasm libva-dev#note\n  # indented\n'),
                         ['patchelf', 'nasm', 'libva-dev'])

    def test_the_list_holds_only_package_names(self):
        names = read(LIST.read_text())
        self.assertGreater(len(names), 30)
        for name in names:
            self.assertRegex(name, r'^[a-z0-9][a-z0-9+.-]+$')

    def test_the_image_build_is_the_one_reader_and_uses_that_comment_syntax(self):
        provision = (ROOT / 'tools/ci/images/provision.sh').read_text()
        self.assertEqual(provision.count(f'{READER} "$root/tools/ci/appimage-packages.txt"'), 1)
        for workflow in (ROOT / '.github/workflows').glob('*.yml'):
            self.assertNotIn('appimage-packages.txt', workflow.read_text(), workflow.name)

if __name__ == '__main__':
    unittest.main()
