#!/usr/bin/env python3
"""Check the documentation tree (#1222). Run from anywhere; the root defaults to this checkout.

Living documents are docs/**/*.md outside docs/evidence, docs/evidence/README.md, README.md,
AGENTS.md and CLAUDE.md. The rules:

1. Every relative link in a living document resolves to a tracked file or directory, and every
   #anchor resolves to a heading (GitHub's slug rules) or an explicit HTML anchor in its target.
2. Every .md under docs/<folder>/ is linked from docs/<folder>/README.md. One in a topic subfolder
   docs/<folder>/<topic>/ may instead be linked from the entry document docs/<folder>/<topic>.md.
   docs/readme/ holds artwork and is indexed by docs/README.md.
3. Directly in docs/ there is only README.md and the stubs shipped software and the website link to,
   and each stub links to its new location.
4. Nothing is tracked under docs/evidence beyond its README and the two directories code reads.
   Those directories are records: their own files are not checked.
5. No path under docs/ has a component the source archive's filter drops.
"""
import argparse
import html
import posixpath
import re
import subprocess
import sys
import unicodedata
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[2]
TOP_FILES = ('README.md', 'AGENTS.md', 'CLAUDE.md')
EVIDENCE_README = 'docs/evidence/README.md'
EVIDENCE_KEPT = ('docs/evidence/live-s3-memory/source114/', 'docs/evidence/cold-family-metadata-race/')
# Old path -> new location. Released packages, installed apps and the put.gd website open the old paths.
STUBS = {'docs/cli.md': 'docs/automation/cli.md',
         'docs/mcp.md': 'docs/automation/mcp.md',
         'docs/package-repositories.md': 'docs/release/package-repositories.md',
         'docs/release.md': 'docs/release/release.md',
         'docs/user-guide.md': 'docs/user/user-guide.md',
         'docs/verification.md': 'docs/testing/verification.md'}
# Folders whose index is not their own README.md.
INDEXED_ELSEWHERE = {'readme': 'docs/README.md'}
# tools/releases/source_archive.py EXCLUDED_COMPONENTS; tests/ci/test_docs.py keeps the two equal.
# A package input in such a path would silently leave the source archive.
ARCHIVE_DROPPED = frozenset({
    '.git', '.github', '.agents', '.codex', '.compound-engineering', '.context', '.claude',
    'work', 'build',
    'context', 'agents', 'tests', 'test', 'fixtures', 'plans', 'reviews', 'evidence',
})
# Existing directories with such a name. Their records are not package inputs.
ARCHIVE_DROPPED_ALLOWED = ('docs/evidence/', 'docs/history/plans/', 'docs/history/reviews/')

FENCE = re.compile(r'^\s{0,3}(```+|~~~+)')
INLINE = re.compile(r'\]\(\s*(<[^>\n]+>|[^()\s]+(?:\([^()\s]*\)[^()\s]*)*)(?:\s+"[^"]*")?\s*\)')
REFDEF = re.compile(r'^\s{0,3}\[[^\]]+\]:\s*(\S+)')
HTML_LINK = re.compile(r'''\b(?:href|src)="([^"]+)"''')
HTML_ANCHOR = re.compile(r'''<[a-zA-Z][^>]*\b(?:id|name)="([^"]+)"''')
ATX = re.compile(r'^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$')
SETEXT = re.compile(r'^\s{0,3}(=+|-+)\s*$')
CODE_SPAN = re.compile(r'(`+)(?:(?!\1).)+?\1')
EXTERNAL = re.compile(r'^[a-zA-Z][a-zA-Z0-9+.-]*:')


def tracked(root):
    output = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z'], text=True)
    return {name for name in output.split('\0') if name and (root / name).exists()}


def lines_outside_fences(text):
    fence = None
    for number, line in enumerate(text.split('\n'), 1):
        match = FENCE.match(line)
        if fence:
            if match and match.group(1)[0] == fence[0] and len(match.group(1)) >= len(fence) and not line.strip().strip(fence[0]):
                fence = None
            continue
        if match:
            fence = match.group(1)
            continue
        yield number, line


def plain_heading(text):
    """Reduce heading Markdown to the text GitHub slugs."""
    text = re.sub(r'!?\[([^\]]*)\]\([^)]*\)', r'\1', text)
    text = re.sub(r'!?\[([^\]]*)\]\[[^\]]*\]', r'\1', text)
    parts = re.split(r'(`+[^`]*`+)', text)
    for index, part in enumerate(parts):
        if part.startswith('`'):
            parts[index] = part.strip('`')
        else:
            part = re.sub(r'<[^>]+>', '', part)
            part = re.sub(r'(\*\*|__)(?=\S)(.+?)(?<=\S)\1', r'\2', part)
            part = re.sub(r'(?<![\w*])([*_])(?=\S)(.+?)(?<=\S)\1(?![\w*])', r'\2', part)
            parts[index] = html.unescape(part)
    return ''.join(parts)


def slug(heading):
    """GitHub's rule: lowercase, keep letters, numbers, marks, connectors and hyphens, spaces to hyphens."""
    out = []
    for char in plain_heading(heading).strip().lower():
        category = unicodedata.category(char)
        if char == ' ':
            out.append('-')
        elif char == '-' or category[0] in 'LN' or category in ('Mn', 'Mc', 'Me', 'Pc'):
            out.append(char)
    return ''.join(out)


def anchors(text):
    seen, found = {}, set()
    previous = None
    for _, line in lines_outside_fences(text):
        heading = None
        match = ATX.match(line)
        if match:
            heading = match.group(2)
        elif previous and previous.strip() and SETEXT.match(line) and not previous.lstrip().startswith(('|', '-', '*', '>')):
            heading = previous
        if heading is not None:
            base = slug(heading)
            count = seen.get(base, 0)
            seen[base] = count + 1
            found.add(base if count == 0 else f'{base}-{count}')
        found.update(HTML_ANCHOR.findall(line))
        previous = line
    return found


def links(text):
    for number, line in lines_outside_fences(text):
        code = [match.span() for match in CODE_SPAN.finditer(line)]
        for match in INLINE.finditer(line):
            if not any(start <= match.start() < end for start, end in code):
                target = match.group(1)
                yield number, target[1:-1] if target.startswith('<') else target
        for target in HTML_LINK.findall(line):
            yield number, target
        match = REFDEF.match(line)
        if match:
            yield number, match.group(1)


def living(files):
    return sorted(name for name in files if name.endswith('.md') and (
        (name.startswith('docs/') and not name.startswith('docs/evidence/')) or name in TOP_FILES or name == EVIDENCE_README))


def check_links(root, files):
    """Rule 1. Also returns which tracked paths each living document links to."""
    directories = {str(parent) for name in files for parent in Path(name).parents}
    problems, linked_from, anchor_cache = [], {}, {}
    for document in living(files):
        here = posixpath.dirname(document)
        for number, target in links((root / document).read_text(errors='replace')):
            if EXTERNAL.match(target) or target.startswith('//'):
                continue
            path, _, fragment = target.partition('#')
            path = unquote(path.partition('?')[0])
            if path.startswith('/'):
                problems.append(f'{document}:{number}: absolute link {target}: write it relative to this file')
                continue
            resolved = posixpath.normpath(posixpath.join(here, path)) if path else document
            if path:
                if resolved not in files and resolved not in directories:
                    problems.append(f'{document}:{number}: broken link {target}: {resolved} is not a tracked file or '
                                    'directory; fix the path (docs/history/2026-11-docs-layout.md maps old ones) or git add the target')
                    continue
                linked_from.setdefault(document, set()).add(resolved)
            if fragment and resolved.endswith('.md') and resolved in files:
                if resolved not in anchor_cache:
                    anchor_cache[resolved] = anchors((root / resolved).read_text(errors='replace'))
                found = anchor_cache[resolved]
                if unquote(fragment) not in found and unquote(fragment).lower() not in found:
                    problems.append(f'{document}:{number}: missing anchor {target}: {resolved} has no such heading; '
                                    'use the heading\'s GitHub slug')
    return problems, linked_from


def check_indexes(files, linked_from):
    """Rule 2."""
    problems = []
    for name in sorted(files):
        parts = name.split('/')
        if parts[0] != 'docs' or len(parts) < 3 or not name.endswith('.md') or parts[1] == 'evidence':
            continue
        folder = '/'.join(parts[:2])
        readme = INDEXED_ELSEWHERE.get(parts[1], folder + '/README.md')
        if name == readme:
            continue
        if readme not in files:
            problems.append(f'{name}: {readme} is missing; add it with the folder\'s Guarantees and Documents')
            continue
        if name in linked_from.get(readme, ()):
            continue
        entry = f'{folder}/{parts[2]}.md'
        if len(parts) > 3 and parts[1] != 'history':
            if name not in linked_from.get(entry, ()):
                problems.append(f'{name}: not indexed; link it from {readme} or from the entry document {entry}')
        else:
            problems.append(f'{name}: not indexed; add a link to it in {readme}')
    return problems


def check_top_level(files, linked_from):
    """Rule 3."""
    problems = []
    for name in sorted(files):
        if posixpath.dirname(name) == 'docs' and name.endswith('.md') and name != 'docs/README.md' and name not in STUBS:
            problems.append(f'{name}: move it into a docs/<folder>/ and link it from that folder\'s README.md; '
                            'only README.md and the stubs that shipped software and the website link to stay directly in docs/')
    for stub, target in STUBS.items():
        if stub not in files:
            problems.append(f'{stub}: restore this stub; shipped software or the website links to it')
        elif target not in linked_from.get(stub, ()):
            problems.append(f'{stub}: this stub must link to its new location, {target}')
    return problems


def check_evidence(files):
    """Rule 4."""
    return [f'{name}: evidence does not go in this tree; publish it to put.gd-evidence with '
            'python3 tools/maintenance/evidence.py publish <dir>'
            for name in sorted(files) if name.startswith('docs/evidence/') and name != EVIDENCE_README
            and not name.startswith(EVIDENCE_KEPT)]


def check_components(files):
    """Rule 5."""
    problems = set()
    for name in files:
        parts = name.split('/')
        if parts[0] != 'docs' or name.startswith(ARCHIVE_DROPPED_ALLOWED):
            continue
        for index, part in enumerate(parts[1:], 1):
            if part in ARCHIVE_DROPPED:
                problems.add(f'{"/".join(parts[:index + 1])}: rename it; tools/releases/source_archive.py drops any path '
                             f'with a component named {part}, so a package input here would leave the source archive')
                break
    return sorted(problems)


def check(root):
    """Every problem in the tree at root, as messages that say what to do."""
    files = tracked(root)
    problems, linked_from = check_links(root, files)
    return (problems + check_indexes(files, linked_from) + check_top_level(files, linked_from)
            + check_evidence(files) + check_components(files))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.root.resolve()
    problems = check(root)
    for problem in problems:
        print(problem)
    print(f'FAILED: {len(problems)} problems' if problems else f'OK: {len(living(tracked(root)))} documents')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
