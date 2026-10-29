"""tools/ci/check_catalog_references.py and update_catalog_references.py, on synthetic renders."""
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools/ci' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


image_diff = load('image_diff')
check = load('check_catalog_references')
update = load('update_catalog_references')


def png(path, colour=(20, 24, 30, 255), size=(16, 12), block=None):
    data = bytearray(bytes(colour) * (size[0] * size[1]))
    if block:
        x, y, w, h, fill = block
        for row in range(y, y + h):
            for col in range(x, x + w):
                i = (row * size[0] + col) * 4
                data[i:i + 4] = bytes(fill)
    path.parent.mkdir(parents=True, exist_ok=True)
    image_diff.write_png(path, image_diff.Image(size[0], size[1], bytes(data)))


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.refs, self.out = self.root / 'refs', self.root / 'render'
        png(self.refs / '10-same.png')
        png(self.out / '10-same.png')
        png(self.refs / '11-noise.png')
        png(self.out / '11-noise.png', block=(3, 3, 1, 1, (200, 0, 0, 255)))
        png(self.refs / '12-moved.png', block=(2, 2, 8, 6, (250, 250, 250, 255)))
        png(self.out / '12-moved.png', block=(6, 2, 8, 6, (250, 250, 250, 255)))
        png(self.refs / '13-gone.png')
        png(self.out / '14-new.png')


class Checking(Fixture):
    def test_every_state_is_classified(self):
        results = check.compare(self.out, self.refs, self.out / 'failures')
        self.assertEqual({n: r['status'] for n, r in results.items()},
                         {'10-same': 'match', '11-noise': 'match', '12-moved': 'differs',
                          '13-gone': 'missing', '14-new': 'unreferenced'})
        failures = sorted(p.name for p in (self.out / 'failures').iterdir())
        self.assertEqual(failures, ['12-moved.actual.png', '12-moved.diff.png', '12-moved.expected.png',
                                    '14-new.actual.png'])

    def test_no_failures_directory_when_everything_matches(self):
        for name in ('12-moved', '13-gone', '14-new'):
            for directory in (self.refs, self.out):
                (directory / (name + '.png')).unlink(missing_ok=True)
        results = check.compare(self.out, self.refs, self.out / 'failures')
        self.assertTrue(all(r['status'] == 'match' for r in results.values()))
        self.assertFalse((self.out / 'failures').exists())

    def test_output_is_replaced_only_when_it_is_ours(self):
        foreign = self.root / 'foreign'
        foreign.mkdir()
        (foreign / 'notes.txt').write_text('keep')
        with self.assertRaises(SystemExit):
            check.prepare_output(foreign)
        self.assertTrue((foreign / 'notes.txt').exists())
        ours = self.root / 'ours'
        ours.mkdir()
        (ours / check.REPORT).write_text('{}')
        (ours / 'old.png').write_text('x')
        check.prepare_output(ours)
        self.assertEqual(list(ours.iterdir()), [])
        check.prepare_output(self.root / 'new' / 'dir')
        self.assertTrue((self.root / 'new' / 'dir').is_dir())

    def test_references_cover_every_scheme_and_only_pngs(self):
        references = ROOT / 'tests/gui/catalog-references'
        self.assertEqual(sorted(p.name for p in references.iterdir() if p.is_dir()), sorted(check.SCHEMES))
        for scheme in check.SCHEMES:
            files = list((references / scheme).iterdir())
            self.assertGreater(len(files), 100, scheme)
            self.assertEqual([p.name for p in files if p.suffix != '.png'], [], scheme)


class Updating(Fixture):
    def test_only_changes_beyond_the_tolerance_are_written(self):
        noise_before = (self.refs / '11-noise.png').read_bytes()
        out = io.StringIO()
        self.assertEqual(update.apply(self.out, self.refs, dry_run=True, out=out), 3)
        self.assertIn('delete 13-gone', out.getvalue())
        self.assertTrue((self.refs / '13-gone.png').exists())
        self.assertEqual(update.apply(self.out, self.refs, out=io.StringIO()), 3)
        self.assertEqual((self.refs / '11-noise.png').read_bytes(), noise_before)
        self.assertEqual((self.refs / '12-moved.png').read_bytes(), (self.out / '12-moved.png').read_bytes())
        self.assertEqual((self.refs / '14-new.png').read_bytes(), (self.out / '14-new.png').read_bytes())
        self.assertFalse((self.refs / '13-gone.png').exists())
        self.assertEqual(update.plan(self.out, self.refs), ([], [], []))

    def test_main_requires_complete_renders(self):
        source = self.root / 'artifact'
        png(source / 'dark' / '10-same.png')
        with self.assertRaises(SystemExit):
            update.main(['--from', str(source), '--scheme', 'dark', '--references', str(self.refs)])
        (source / 'dark' / 'manifest.json').write_text(json.dumps({'kind': 'synthetic-ui-catalog'}))
        refs = self.root / 'fresh'
        stdout = io.StringIO()
        sys.stdout, saved = stdout, sys.stdout
        try:
            update.main(['--from', str(source), '--scheme', 'dark', '--references', str(refs)])
        finally:
            sys.stdout = saved
        self.assertEqual([p.name for p in (refs / 'dark').iterdir()], ['10-same.png'])


if __name__ == '__main__':
    unittest.main()
