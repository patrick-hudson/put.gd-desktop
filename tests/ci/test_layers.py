"""The include graph of src/ stays in its layers; a new backwards include fails."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ci_check_layers', ROOT / 'tools/ci/check_layers.py')
layers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layers)

# Each was a backwards edge on nightly before #1222.
REMOVED = (('core', 'app'), ('storage/s3', 'app'), ('storage/s3', 'ui'), ('record', 'app'),
           ('platform/kde', 'platform/x11'), ('platform/kde', 'app'),
           ('platform/wayland', 'app'), ('platform/x11', 'app'))


class Tree(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.src = self.root / 'src'
        self.src.mkdir()

    def write(self, name, *lines):
        path = self.src / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('\n'.join(lines) + '\n')

    def edges(self):
        return [(i.file, i.line, i.source, i.target) for i in layers.includes(self.src)]


class RealTree(unittest.TestCase):
    def test_src_matches_the_tables(self):
        self.assertEqual(layers.check(ROOT / 'src'), [])

    def test_the_allowed_graph_has_no_cycle(self):
        self.assertIsNone(layers.cycle(layers.ALLOWED))

    def test_every_exception_has_a_reason_and_a_real_file(self):
        for (file, target), reason in layers.TOLERATED.items():
            with self.subTest(file=file, target=target):
                self.assertTrue((ROOT / file).is_file())
                self.assertTrue(reason.strip())

    def test_the_removed_edges_stay_removed(self):
        pinned = {(layers.module_of(Path(file).relative_to('src').as_posix()), target)
                  for file, target in layers.TOLERATED}
        for source, target in REMOVED:
            with self.subTest(source=source, target=target):
                self.assertNotIn(target, layers.ALLOWED.get(source, ()))
                self.assertNotIn((source, target), pinned)


class Modules(unittest.TestCase):
    def test_platform_and_storage_split_by_subdirectory(self):
        for path, module in (('main.cpp', 'main'), ('core/Error.h', 'core'), ('ui/fonts/Font.h', 'ui'),
                             ('platform/Geometry.h', 'platform'), ('platform/x11/X11Capture.h', 'platform/x11'),
                             ('platform/kde/protocols/gen.h', 'platform/kde'), ('storage/Shared.h', 'storage'),
                             ('storage/s3/S3Driver.h', 'storage/s3')):
            with self.subTest(path=path):
                self.assertEqual(layers.module_of(path), module)


class Includes(Tree):
    def test_a_quoted_name_finds_its_neighbour_before_src(self):
        self.write('core/Error.h')
        self.write('ui/Error.h')
        self.write('Error.h')
        self.write('ui/Window.cpp', '#include "Error.h"', '#include "core/Error.h"')
        self.assertEqual(self.edges(), [('src/ui/Window.cpp', 2, 'ui', 'core')])

    def test_a_quoted_name_falls_back_to_src(self):
        self.write('core/Error.h')
        self.write('ui/Window.cpp', '#include "core/Error.h"')
        self.assertEqual(self.edges(), [('src/ui/Window.cpp', 1, 'ui', 'core')])

    def test_parent_paths_and_angle_brackets_and_spacing_count(self):
        self.write('core/Error.h')
        self.write('ui/A.cpp', '#include "../core/Error.h"')
        self.write('ui/B.cpp', '#include <core/Error.h>')
        self.write('ui/C.cpp', '  #  include   "core/Error.h"')
        self.assertEqual([edge[0] for edge in self.edges()], ['src/ui/A.cpp', 'src/ui/B.cpp', 'src/ui/C.cpp'])

    def test_outside_files_comments_and_same_module_includes_do_not_count(self):
        self.write('core/Error.h')
        self.write('core/Other.h', '#include "Error.h"')
        self.write('ui/Window.cpp', '#include <QObject>', '#include "generated-protocol.h"',
                   '// #include "core/Error.h"', 'auto text = "#include \\"core/Error.h\\"";',
                   '#include "../../outside/Error.h"')
        self.assertEqual(self.edges(), [])

    def test_only_source_files_are_read(self):
        self.write('core/Error.h')
        self.write('ui/notes.txt', '#include "core/Error.h"')
        self.assertEqual(self.edges(), [])


class Check(Tree):
    def test_a_new_edge_names_the_include_and_what_to_do(self):
        self.write('app/Service.h')
        self.write('core/Job.cpp', '// first', '#include "app/Service.h"')
        problems = layers.check(self.src, {}, {})
        self.assertEqual(len(problems), 1)
        for text in ('src/core/Job.cpp:2', '#include "app/Service.h"', 'core -> app', 'lower module', 'ALLOWED'):
            self.assertIn(text, problems[0])

    def test_an_allowed_edge_passes(self):
        self.write('core/Error.h')
        self.write('app/Service.h', '#include "core/Error.h"')
        self.assertEqual(layers.check(self.src, {'app': {'core'}}, {}), [])

    def test_a_new_module_without_edges_passes_and_with_an_edge_fails(self):
        self.write('core/Error.h')
        self.write('fresh/Alone.h')
        self.assertEqual(layers.check(self.src, {}, {}), [])
        self.write('fresh/Reach.h', '#include "core/Error.h"')
        self.assertIn('fresh -> core', layers.check(self.src, {}, {})[0])

    def test_a_tolerated_edge_passes_only_from_its_pinned_file(self):
        self.write('storage/s3/Url.h')
        self.write('ui/Settings.cpp', '#include "storage/s3/Url.h"')
        pin = {('src/ui/Settings.cpp', 'storage/s3'): 'reason'}
        self.assertEqual(layers.check(self.src, {}, pin), [])
        self.write('ui/History.cpp', '#include "storage/s3/Url.h"')
        problems = layers.check(self.src, {}, pin)
        self.assertEqual(len(problems), 1)
        for text in ('src/ui/History.cpp:1', '#include "storage/s3/Url.h"', 'tolerated only in src/ui/Settings.cpp',
                     'lower module'):
            self.assertIn(text, problems[0])

    def test_a_pin_covers_one_module_only(self):
        self.write('storage/s3/Url.h')
        self.write('storage/b2/Url.h')
        self.write('ui/Settings.cpp', '#include "storage/s3/Url.h"', '#include "storage/b2/Url.h"')
        problems = layers.check(self.src, {}, {('src/ui/Settings.cpp', 'storage/s3'): 'reason'})
        self.assertEqual(len(problems), 1)
        self.assertIn('ui -> storage/b2 is not in the layer table', problems[0])

    def test_a_stale_allowed_edge_fails(self):
        self.write('core/Error.h')
        self.assertIn('ALLOWED app -> core is stale', layers.check(self.src, {'app': {'core'}}, {})[0])

    def test_a_stale_pin_fails(self):
        self.write('ui/Settings.cpp')
        problems = layers.check(self.src, {}, {('src/ui/Settings.cpp', 'storage/s3'): 'reason'})
        self.assertIn('TOLERATED src/ui/Settings.cpp -> storage/s3 is stale', problems[0])

    def test_an_edge_in_both_tables_fails(self):
        self.write('core/Error.h')
        self.write('app/Service.h', '#include "core/Error.h"')
        problems = layers.check(self.src, {'app': {'core'}}, {('src/app/Service.h', 'core'): 'reason'})
        self.assertTrue(any('in both ALLOWED and TOLERATED' in problem for problem in problems), problems)

    def test_a_cycle_in_the_allowed_graph_fails(self):
        self.write('a/A.h', '#include "b/B.h"')
        self.write('b/B.h', '#include "a/A.h"')
        problems = layers.check(self.src, {'a': {'b'}, 'b': {'a'}}, {})
        self.assertEqual(len(problems), 1)
        self.assertIn('cycle, a -> b -> a', problems[0])

    def test_the_command_line_fails_on_a_bad_tree_and_passes_on_the_real_one(self):
        self.write('app/Service.h')
        self.write('core/Job.cpp', '#include "app/Service.h"')
        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            self.assertEqual(layers.main(['--root', str(self.root)]), 1)
        self.assertIn('src/core/Job.cpp:1', output.getvalue())
        self.assertEqual(layers.main(['--root', str(ROOT)]), 0)


if __name__ == '__main__':
    unittest.main()
