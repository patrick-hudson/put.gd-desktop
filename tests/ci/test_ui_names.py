"""The app's UI has one name: the old redesign, Graphite and preview names stay retired.

Graphite, VisualStyle and RedesignTheme became putgd::Ui (src/ui/Ui.h), and
putgd-redesign-preview became putgd-ui-catalog, on 2026-10-29. Historical
records under docs/ keep the words they were written with.
"""
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
RETIRED = re.compile(r'\bGraphite\b|\bgraphite[A-Z]|RedesignTheme|VisualStyle|[Rr]edesign|PUTGD_PREVIEW_')
# Code and build inputs; documentation follows in its own change.
SCOPES = ('src/', 'tests/', 'tools/', 'packaging/', '.github/', 'CMakeLists.txt', 'CMakePresets.json')
SKIPPED = ('tests/ci/test_ui_names.py', 'tools/qemu-grid/dist/')


def tracked():
    names = subprocess.run(['git', 'ls-files', '-z', '--', *SCOPES], cwd=ROOT, check=True,
                           stdout=subprocess.PIPE).stdout.decode().split('\0')
    return [name for name in names if name and not name.startswith(SKIPPED)]


def retired_uses(paths):
    found = []
    for name in paths:
        path = ROOT / name
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding='utf-8')
        except UnicodeDecodeError:
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if RETIRED.search(line):
                found.append(f'{name}:{number}: {line.strip()[:120]}')
        if RETIRED.search(path.name):
            found.append(f'{name}: file name')
    return found


class RetiredUiNames(unittest.TestCase):
    def test_the_pattern_catches_each_retired_name(self):
        for line in ('#include "ui/Graphite.h"', 'w->setProperty("graphiteFlush", true);',
                     'RedesignTheme::apply(*this);', 'VisualStyle::icon("tray")',
                     'putgd-redesign-preview', 'the redesign theme', 'PUTGD_PREVIEW_THEME=light'):
            with self.subTest(line=line):
                self.assertRegex(line, RETIRED)
        for line in ('Ui::styleWindow(*this);', 'w->setProperty("uiFlush", true);', 'historyPreview',
                     'putgd-ui-catalog', 'PUTGD_CATALOG_THEME=light'):
            with self.subTest(line=line):
                self.assertNotRegex(line, RETIRED)

    def test_code_and_build_inputs_use_only_the_current_names(self):
        found = retired_uses(tracked())
        self.assertEqual(found, [], 'Retired UI names (use putgd::Ui, ui* properties, putgd-ui-catalog):\n'
                         + '\n'.join(found))


if __name__ == '__main__':
    unittest.main()
