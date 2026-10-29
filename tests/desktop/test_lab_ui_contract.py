"""The lab drivers' copies of app wording and fields still exist in the app source.

The drivers match what the real app shows (tray titles, menu labels, size
units, editor fields). A UI change that renames one of them must fail here, in
the contracts job, rather than on the next lab run.
"""
import importlib.util
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location('lab_ui_' + name, ROOT / 'tools/lab' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def literals(*paths):
    """Every C++ string literal in the given source files."""
    found = set()
    for path in paths:
        text = (ROOT / path).read_text(encoding='utf-8')
        # As written in the source; the wording checked here carries no escapes.
        found.update(re.findall(r'"((?:[^"\\\n]|\\.)*)"', text))
    return found


class LabUiContract(unittest.TestCase):
    def assertLiterals(self, wanted, *paths):
        present = literals(*paths)
        missing = sorted(value for value in wanted if value not in present)
        self.assertEqual(missing, [], f'Lab driver strings missing from {", ".join(paths)}')

    def test_tray_titles_attention_and_phase_labels(self):
        tray = load('tray_progress')
        wanted = {tray.IDLE, tray.UPLOADING, tray.COMPLETE, tray.ATTENTION, tray.FINISHING, tray.REVIEW,
                  *tray.LABELS.values(), *(label for labels in tray.ATTENTION_LABELS.values() for label in labels)}
        self.assertLiterals(wanted, 'src/ui/TrayController.cpp')
        # The native item draws the same titles when it is given none.
        self.assertLiterals({tray.IDLE, tray.UPLOADING, tray.ATTENTION}, 'src/ui/NativeTray.cpp')

    def test_tray_sizes_use_the_app_byte_summary_units(self):
        tray = load('tray_progress')
        self.assertLiterals({' MB', ' GB', '<0.01 MB'}, 'src/core/ByteSize.h')
        for value, text in ((1, '<0.01 MB'), (5 * 1024**2 // 2, '2.5 MB'), (3 * 1024**3, '3 GB')):
            with self.subTest(value=value):
                self.assertEqual(tray.byte_summary(value), text)
                self.assertRegex(text, '^' + tray.SIZE + '$')

    def test_tray_quit_label(self):
        native = load('native_acceptance')
        self.assertLiterals({native.QUIT_LABEL}, 'src/ui/NativeTray.cpp', 'src/ui/TrayController.cpp')

    def test_editor_view_fields_the_editor_driver_reads(self):
        source = (ROOT / 'tools/lab/edit_acceptance.py').read_text(encoding='utf-8')
        read = set(re.findall(r"\bview(?:\['|\.get\(')([A-Za-z]+)'", source))
        self.assertTrue({'zoomPercent', 'fitted', 'visibleRect'} <= read)
        # The window's view state answers editor.view (EditorWindow::viewState).
        self.assertIn("view = editor('view', {'id': op_id})", source)
        self.assertIn('"editor.view"', (ROOT / 'src/app/LocalServer.cpp').read_text(encoding='utf-8'))
        state = (ROOT / 'src/ui/EditorWindow.cpp').read_text(encoding='utf-8')
        body = state[state.index('QJsonObject EditorWindow::viewState() const'):]
        body = body[:body.index('\n}\n')]
        self.assertEqual(sorted(key for key in read if '"' + key + '"' not in body), [])


if __name__ == '__main__':
    unittest.main()
