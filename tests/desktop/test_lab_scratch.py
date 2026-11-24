"""Lab scratch stays off the builders' RAM-backed /tmp (#1978)."""
import os
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab import scratch  # noqa: E402

DIRECTORY_CALL = re.compile(r'\b(?:mkdtemp|TemporaryDirectory)\(')
SHELL_MKTEMP = re.compile(r'\bmktemp\s+(?:-[a-zA-Z]*d[a-zA-Z]*|--directory)\b(?P<rest>.*)')
MARKER = 'tmpfs-ok:'


def unmarked_tmp_directories(root):
    """Lines under ROOT (Boat excluded: its sandboxes manage their own tmpfs scratch) that make a
    directory in the default temporary directory or in /tmp, without a `tmpfs-ok:` reason on
    that line or the one before."""
    found = []
    for path in sorted(root.rglob('*')):
        if path.suffix not in ('.py', '.sh') or 'boat' in path.relative_to(root).parts or path.name == 'scratch.py':
            continue
        lines = path.read_text(errors='replace').splitlines()
        for number, line in enumerate(lines):
            if MARKER in line or (number and MARKER in lines[number - 1]):
                continue
            call, shell = DIRECTORY_CALL.search(line), SHELL_MKTEMP.search(line)
            if call and not line[:call.start()].endswith('scratch.'):
                tail = line[call.end():]
                if 'dir=' not in tail or re.search(r"dir=['\"]/tmp", tail):
                    found.append(f'{path.relative_to(ROOT)}:{number + 1}')
            elif shell:
                target = shell['rest'].split()
                if not target or target[0].strip('"\'').startswith(('/tmp', '$TMPDIR', '${TMPDIR')) \
                        or target[0].startswith((')', '|', ';', '&')):
                    found.append(f'{path.relative_to(ROOT)}:{number + 1}')
    return found


class ScratchBase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name) / 'work/tmp'

    def test_directories_are_private_and_under_the_base(self):
        with patch.dict(os.environ, {scratch.VARIABLE: str(self.base)}):
            made = scratch.mkdtemp('pgx.')
            with scratch.TemporaryDirectory('pgy.') as other:
                self.assertEqual(Path(other).parent, self.base)
            self.assertFalse(Path(other).exists())
        self.assertEqual(made.parent, self.base)
        self.assertTrue(made.name.startswith('pgx.'))
        self.assertEqual(self.base.stat().st_mode & 0o777, 0o700)
        self.assertEqual(made.stat().st_mode & 0o777, 0o700)

    def test_defaults_to_work_tmp_in_home(self):
        home = Path(self.temporary.name)
        with patch.dict(os.environ, {'HOME': str(home)}):
            os.environ.pop(scratch.VARIABLE, None)
            self.assertEqual(scratch.root(), home / 'work/tmp')

    def test_refuses_a_base_others_can_write_or_a_relative_one(self):
        self.base.mkdir(parents=True)
        self.base.chmod(0o777)
        with patch.dict(os.environ, {scratch.VARIABLE: str(self.base)}):
            with self.assertRaises(scratch.ScratchError):
                scratch.mkdtemp()
        with patch.dict(os.environ, {scratch.VARIABLE: 'work/tmp'}):
            with self.assertRaises(scratch.ScratchError):
                scratch.root()


class LabToolsKeepScratchOnDisk(unittest.TestCase):
    def test_no_lab_tool_makes_an_unmarked_directory_in_tmp(self):
        self.assertEqual(unmarked_tmp_directories(ROOT / 'tools/lab'), [],
                         'make lab scratch with tools/lab/scratch.py (disk), or say on the line before why '
                         'this one belongs in /tmp with "# tmpfs-ok: <reason>"')

    def test_the_check_sees_each_form(self):
        with tempfile.TemporaryDirectory() as folder:
            tree = Path(folder) / 'tools/lab'
            tree.mkdir(parents=True)
            (tree / 'a.py').write_text("tempfile.mkdtemp(prefix='x')\n"
                                       "tempfile.TemporaryDirectory(dir='/tmp')\n"
                                       "scratch.mkdtemp('x')\n"
                                       "tempfile.mkdtemp(dir=run)\n"
                                       "# tmpfs-ok: sockets\n"
                                       "tempfile.mkdtemp(dir='/tmp')\n")
            (tree / 'b.sh').write_text('d=$(mktemp -d)\ne=$(mktemp -d /tmp/x.XXXX)\nf=$(mktemp -d ~/work/tmp/x.XXXX)\n'
                                       'g=$(mktemp --directory)\n')
            with patch.object(sys.modules[__name__], 'ROOT', Path(folder)):
                self.assertEqual(unmarked_tmp_directories(tree),
                                 ['tools/lab/a.py:1', 'tools/lab/a.py:2', 'tools/lab/b.sh:1', 'tools/lab/b.sh:2',
                                  'tools/lab/b.sh:4'])


if __name__ == '__main__':
    unittest.main()
