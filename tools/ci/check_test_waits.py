#!/usr/bin/env python3
"""Reject unscaled waits in C++ tests.

Every wait for an asynchronous outcome in tests/ goes through
tests/fixtures/Waits.h, whose budgets scale with PUTGD_TEST_TIME_SCALE. This
check rejects the primitives that bypass it:

- qwait:      QTest::qWait / qWait (use settle() for fixed time, a TRY or STEADY macro to wait)
- qwaitfor:   QTest::qWaitFor (use putgd::test::waitFor)
- qtry:       QTRY_* (use PUTGD_TRY_*)
- spy-wait:   QSignalSpy::wait on a *spy* variable (use putgd::test::waitForSignal)
- window-wait: QTest::qWaitForWindowExposed/Active (use putgd::test::waitForWindowExposed/Active)
- settle-assert: settle(ms) followed directly by a plain QVERIFY/QCOMPARE, which is
              a sleep standing in for a wait (use PUTGD_VERIFY_STEADY / PUTGD_COMPARE_STEADY)

Justified exceptions are listed in tools/ci/test-waits-allowlist.json with the
exact count per file and rule, so a new use in a listed file still fails, and a
removed one makes the entry stale.
"""
import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
ALLOWLIST = ROOT / 'tools/ci/test-waits-allowlist.json'
HELPER = 'tests/fixtures/Waits.h'
SUFFIXES = ('.cpp', '.cc', '.h', '.hpp')

RULES = {
    'qwait': re.compile(r'\bqWait\s*\('),
    'qwaitfor': re.compile(r'\bqWaitFor\s*\('),
    'qtry': re.compile(r'\bQTRY_[A-Z0-9_]+\s*\('),
    'spy-wait': re.compile(r'\b\w*[sS]py\w*\s*(?:\.|->)\s*wait\s*\('),
    'window-wait': re.compile(r'\bqWaitForWindow(?:Exposed|Active)\s*\('),
    'settle-assert': re.compile(r'(?<![\w.>:])(?:::)?(?:putgd::test::|test::)?settle\s*\((?:[^;()]|\([^;()]*\))*\)\s*;\s*(?:QVERIFY|QCOMPARE)\w*\s*\('),
}
ADVICE = {
    'qwait': 'use putgd::test::settle(ms) for fixed time, or a PUTGD_TRY/STEADY macro to wait for an outcome',
    'qwaitfor': 'use putgd::test::waitFor(predicate, ms)',
    'qtry': 'use the PUTGD_TRY_* macro of the same name',
    'spy-wait': 'use putgd::test::waitForSignal(spy, count, ms)',
    'window-wait': 'use putgd::test::waitForWindowExposed / waitForWindowActive',
    'settle-assert': 'a fixed sleep before an assertion is a race; use PUTGD_VERIFY_STEADY or PUTGD_COMPARE_STEADY',
}


def strip_comments_and_strings(text):
    """Blank comments and literals, keeping offsets and line breaks."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out[i:j] = ' ' * (j - i)
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out[i:j] = [ch if ch == '\n' else ' ' for ch in text[i:j]]
            i = j
        elif c == 'R' and text.startswith('R"', i):
            m = re.match(r'R"([^(\s]*)\(', text[i:])
            if not m:
                i += 1
                continue
            end = text.find(')' + m.group(1) + '"', i)
            j = n if end < 0 else end + len(m.group(1)) + 2
            out[i:j] = [ch if ch == '\n' else ' ' for ch in text[i:j]]
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out[i + 1:j - 1] = ' ' * max(0, j - 1 - (i + 1))
            i = j
        else:
            i += 1
    return ''.join(out)


def scan_text(text):
    """Return {rule: [line, ...]} for one source text."""
    clean = strip_comments_and_strings(text)
    found = {}
    for rule, pattern in RULES.items():
        for match in pattern.finditer(clean):
            found.setdefault(rule, []).append(clean.count('\n', 0, match.start()) + 1)
    return found


def scan(root=ROOT):
    results = {}
    for path in sorted((root / 'tests').rglob('*')):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        if relative == HELPER:
            continue
        found = scan_text(path.read_text(errors='replace'))
        if found:
            results[relative] = found
    return results


def load_allowlist(path=ALLOWLIST):
    entries = json.loads(path.read_text())['entries']
    allowed = {}
    for entry in entries:
        if entry['rule'] not in RULES or not entry.get('reason', '').strip():
            raise ValueError(f'allowlist entry needs a known rule and a reason: {entry}')
        allowed[(entry['path'], entry['rule'])] = int(entry['count'])
    return allowed


def check(results, allowed):
    problems = []
    for path, rules in results.items():
        for rule, lines in rules.items():
            expected = allowed.get((path, rule), 0)
            if len(lines) > expected:
                where = ', '.join(str(line) for line in lines)
                listed = f' ({expected} allowlisted)' if expected else ''
                problems.append(f'{path}: {len(lines)} {rule}{listed} at line {where}: {ADVICE[rule]}')
    for (path, rule), expected in sorted(allowed.items()):
        actual = len(results.get(path, {}).get(rule, []))
        if actual < expected:
            problems.append(f'{path}: allowlist expects {expected} {rule}, found {actual}; '
                            'lower the count in tools/ci/test-waits-allowlist.json')
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--summary', action='store_true', help='print counts per file and rule')
    args = parser.parse_args(argv)
    results = scan()
    if args.summary:
        for path, rules in results.items():
            print(path, {rule: len(lines) for rule, lines in rules.items()})
    problems = check(results, load_allowlist())
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(f'{len(problems)} unscaled wait problem(s); see tests/fixtures/Waits.h', file=sys.stderr)
        return 1
    print('test waits: every wait goes through tests/fixtures/Waits.h or is allowlisted')
    return 0


if __name__ == '__main__':
    sys.exit(main())
