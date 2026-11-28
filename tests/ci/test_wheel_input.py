"""A closed control never changes its value on the wheel (#26, #2117).

The wheel over a dropdown, spin box, slider or tab bar scrolls the pane under it.
Every dropdown in src/ is built with Ui::choiceBox(), every other such control
with Ui::wheelSafe<T>(), and a subclass of one calls Ui::leaveWheelToPane(this).
A widget that takes the wheel on purpose (a canvas that pans, a timeline that
zooms) says why with a `wheel-input:` comment on the line.
"""
from pathlib import Path
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
# Qt controls whose own wheelEvent changes their value. QScrollBar is left out:
# scrolling is what it is for.
CONTROLS = ('QComboBox', 'QFontComboBox', 'QAbstractSpinBox', 'QSpinBox', 'QDoubleSpinBox',
            'QDateTimeEdit', 'QDateEdit', 'QTimeEdit', 'QAbstractSlider', 'QSlider', 'QDial',
            'QTabBar', 'QTabWidget', 'QCalendarWidget')
NAMES = '|'.join(CONTROLS)
RAW = re.compile(rf'\bnew\s+(?:Qt::)?({NAMES})\b|\bstd::make_(?:unique|shared)\s*<\s*({NAMES})\s*>')
DROPDOWN = re.compile(r'\bwheelSafe<\s*QComboBox\s*>')
SUBCLASS = re.compile(rf'\b(?:class|struct)\s+(\w+)(?:\s+final)?\s*:[^;{{]*?\b({NAMES})\b[^;{{]*\{{')
WHEEL_OVERRIDE = re.compile(r'\bvoid\s+wheelEvent\s*\(')
OPT_IN = 'wheel-input:'


def body(text, start):
    """The text of the braced block that opens at text[start]."""
    depth = 0
    for index in range(start, len(text)):
        depth += {'{': 1, '}': -1}.get(text[index], 0)
        if depth == 0:
            return text[start:index + 1]
    return text[start:]


def code(text):
    """text without its // comments, so a guard that is only mentioned doesn't count."""
    return re.sub(r'//[^\n]*', '', text)


def guarded(src, name, base, declaration):
    """The class calls the guard in its body or in an out-of-line constructor. A tab widget may
    guard its tab bar, where the wheel lands."""
    guards = ('leaveWheelToPane(this)',) + (('leaveWheelToPane(tabBar())',) if base == 'QTabWidget' else ())
    if any(guard in code(declaration) for guard in guards):
        return True
    constructor = re.compile(rf'\b{name}::{name}\s*\(')
    for path in src.rglob('*.cpp'):
        text = path.read_text()
        for match in constructor.finditer(text):
            if any(guard in code(body(text, text.index('{', match.end()))) for guard in guards):
                return True
    return False


def check(src):
    """Every place in src that lets a closed control take the wheel, as (file, line, why)."""
    problems = []
    for path in sorted(src.rglob('*')):
        if path.suffix not in {'.cpp', '.h'} or not path.is_file():
            continue
        name, text = path.relative_to(src.parent).as_posix(), path.read_text()
        lines = text.splitlines()

        def report(line, why):
            if OPT_IN not in lines[line - 1]:
                problems.append((name, line, why))

        def line_of(match):
            return text.count('\n', 0, match.start()) + 1

        # Whole-file scans: `new` and the class name may sit on two lines.
        for match in RAW.finditer(text):
            control = match.group(1) or match.group(2)
            use = 'Ui::choiceBox()' if control == 'QComboBox' else f'Ui::wheelSafe<{control}>()'
            report(line_of(match), f'raw {control} takes the wheel when closed; use {use}')
        for match in DROPDOWN.finditer(text):
            report(line_of(match), 'a dropdown is Ui::choiceBox(), not Ui::wheelSafe<QComboBox>()')
        for number, line in enumerate(lines, 1):
            if WHEEL_OVERRIDE.search(line) and '::wheelEvent' not in line:
                report(number, 'wheelEvent override: say why with a wheel-input: comment, '
                               'or use Ui::leaveWheelToPane()')
        for match in SUBCLASS.finditer(text):
            if not guarded(src, match.group(1), match.group(2), body(text, match.end() - 1)):
                report(text.count('\n', 0, match.start()) + 1,
                       f'{match.group(1)} derives from {match.group(2)}; '
                       'call Ui::leaveWheelToPane(this) in its constructor')
    return sorted(problems)


class RealTree(unittest.TestCase):
    def test_no_control_in_src_takes_the_wheel_when_closed(self):
        self.assertEqual(check(ROOT / 'src'), [])


class Detection(unittest.TestCase):
    def tree(self, **files):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        src = Path(directory.name) / 'src'
        for name, text in files.items():
            (src / name).parent.mkdir(parents=True, exist_ok=True)
            (src / name).write_text(text)
        return [(file, line) for file, line, _ in check(src)]

    def test_a_raw_dropdown_or_spin_box_fails(self):
        self.assertEqual(self.tree(**{'ui/A.cpp': 'auto* c=new QComboBox(this);\n'
                                                  'auto* s=new QSpinBox;\nauto* t=std::make_unique<QSlider>();\n'
                                                  'auto* w=Ui::wheelSafe<QComboBox>(this);\n'
                                                  'auto u=std::make_unique< QDial >();\n'
                                                  'auto* v=new\n    QSpinBox(this);\n'}),
                         [('src/ui/A.cpp', 1), ('src/ui/A.cpp', 2), ('src/ui/A.cpp', 3), ('src/ui/A.cpp', 4),
                          ('src/ui/A.cpp', 5), ('src/ui/A.cpp', 6)])

    def test_the_guarded_ways_pass(self):
        self.assertEqual(self.tree(**{'ui/A.cpp': 'auto* c=Ui::choiceBox(this);\n'
                                                  'auto* s=Ui::wheelSafe<QSpinBox>(this);\n'
                                                  'auto* d=new QDial; // wheel-input: a test dial turns\n'}), [])

    def test_a_subclass_must_guard_itself(self):
        unguarded = 'class Box final : public QComboBox {\npublic:\n    using QComboBox::QComboBox;\n};\n'
        guarded = 'class Spin final:public QDoubleSpinBox {\npublic:Spin(){Ui::leaveWheelToPane(this);}\n};\n'
        outside = 'class Bar:public QTabBar {\npublic:Bar();\n};\n'
        mentioned = 'class Note final : public QSlider {\npublic:Note(){} // Ui::leaveWheelToPane(this) later\n};\n'
        tabs = 'class Tabs final : public QTabWidget {\npublic:Tabs(){Ui::leaveWheelToPane(tabBar());}\n};\n'
        self.assertEqual(self.tree(**{'record/Box.h': unguarded, 'ui/Spin.h': guarded, 'ui/Bar.h': outside,
                                      'ui/Bar.cpp': 'Bar::Bar() : QTabBar() {\n    Ui::leaveWheelToPane(this);\n}\n',
                                      'ui/Note.h': mentioned, 'ui/Tabs.h': tabs}),
                         [('src/record/Box.h', 1), ('src/ui/Note.h', 1)])

    def test_a_wheel_handler_says_why(self):
        self.assertEqual(self.tree(**{'ui/V.h': 'class V:public QWidget{\n void wheelEvent(QWheelEvent*) override;\n'
                                                ' void keyPressEvent(QKeyEvent*) override;\n};\n',
                                      'ui/V.cpp': 'void V::wheelEvent(QWheelEvent* e){}\n',
                                      'ui/W.h': 'class W:public QWidget{\n void wheelEvent(QWheelEvent*) override; '
                                                '// wheel-input: W pans its picture\n};\n'}),
                         [('src/ui/V.h', 2)])


if __name__ == '__main__':
    unittest.main()
