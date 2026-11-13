#!/usr/bin/env python3
"""Keep the include graph of src/ in its layers.

A module is a directory under src/, with platform/<x> and storage/<x> counted
separately and src/main.cpp as `main`. ALLOWED is the whole cross-module graph
apart from the backwards edges in TOLERATED, and has no cycles. TOLERATED pins
each backwards edge to the exact file that includes it, with the reason it
stays, so it can't spread: another file taking that edge fails.

An entry nothing uses any more also fails, so the table stays the real graph.
"""
import argparse
from collections import defaultdict, namedtuple
import os
from pathlib import Path, PurePosixPath
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SUFFIXES = ('.c', '.cc', '.cpp', '.h', '.hpp')
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*(?:"([^"\n]+)"|<([^>\n]+)>)')
# Comments and raw strings, which may hold an include; ordinary strings and line
# comments are matched so that a /* inside one doesn't open a block comment.
NON_CODE = re.compile(r'''//[^\n]*|/\*.*?(?:\*/|\Z)|\b(?:u8|[uUL])?R"([^()\\\s]*)\(.*?\)\1"|"(?:\\.|[^"\\\n])*"?''', re.S)
# One subdirectory per desktop or provider; each is its own module.
SPLIT_MODULES = ('platform', 'storage')

# Lower layers first. main, the helpers (capture, record, ocr) and the CLI are leaves.
ALLOWED = {
    'core': {'ipc'},
    'secrets': {'core'},
    'client': {'core'},
    'platform': {'core'},
    'platform/gnome': {'core', 'platform'},
    'platform/wayland': {'core', 'ipc', 'platform'},
    'platform/kde': {'core', 'platform', 'platform/wayland'},
    'platform/budgie': {'core', 'platform', 'platform/wayland'},
    'platform/hyprland': {'core', 'platform', 'platform/wayland'},
    'platform/x11': {'core', 'ipc', 'platform'},
    'storage/b2': {'core', 'secrets'},
    'storage/http': {'core', 'secrets'},
    'storage/proton': {'core'},
    'storage/s3': {'core', 'ipc', 'secrets'},
    'app': {'client', 'core', 'ipc', 'secrets'},
    'ui': {'client', 'core', 'ipc', 'platform'},
    'capture': {'core', 'ipc', 'platform', 'platform/x11', 'ui'},
    'record': {'core', 'ipc', 'platform', 'platform/wayland', 'platform/x11', 'ui'},
    'ocr': {'core', 'ipc'},
    'cli': {'core'},
    'main': {'app', 'core', 'platform', 'platform/budgie', 'platform/gnome', 'platform/hyprland', 'platform/kde', 'platform/wayland', 'platform/x11',
             'storage/b2', 'storage/http', 'storage/proton', 'storage/s3', 'ui'},
}

# (including file, included module): why that file may still do it.
TOLERATED = {
    ('src/cli/UpdateInstall.cpp', 'app'): 'the CLI installs AppImage updates with the resident\'s AppImageInstall',
    ('src/cli/UpdateInstall.h', 'app'): 'the CLI installs AppImage updates with the resident\'s AppImageInstall',
    ('src/cli/main.cpp', 'app'): 'the CLI shares the resident\'s reply format, socket path and deadlines',
    ('src/editor/EditorCanvas.cpp', 'ui'): 'the canvas draws with the app style (Theme, Ui)',
    ('src/ipc/CaptureControl.h', 'core'): 'only the putgd::Error value type, from a header with no core link',
    ('src/ipc/UiControl.h', 'core'): 'only the putgd::Error value type, from a header with no core link',
    ('src/platform/gnome/GnomeAdapter.h', 'app'): 'the GNOME adapter is also the app\'s RecordingShell',
    ('src/platform/wayland/LayerShellRegionOverlay.cpp', 'ui'): 'the layer-shell selector shares the region widgets',
    ('src/platform/wayland/LayerShellRegionOverlay.h', 'ui'): 'the layer-shell selector shares the region widgets',
    ('src/platform/wayland/region_main.cpp', 'ui'): 'the Wayland region helper uses the app theme and Quick backend',
    ('src/platform/x11/NativeRegionOverlay.cpp', 'ui'): 'the X11 selector shares the region widgets',
    ('src/platform/x11/NativeRegionOverlay.h', 'ui'): 'the X11 selector shares the region widgets',
    ('src/platform/x11/X11SourceAdapter.cpp', 'ui'): 'the X11 adapter opens the shared region overlay',
    ('src/platform/x11/X11SourceAdapter.h', 'ui'): 'the X11 adapter opens the shared region overlay',
    ('src/ui/CustomUploaderPanel.cpp', 'storage/http'): 'the custom uploader form reads .sxcu files and names secret values with the provider\'s rules',
    ('src/ui/EditorWindow.h', 'editor'): 'the editor window hosts the canvas; editor and ui are not yet split',
    ('src/ui/ImageEditor.cpp', 'editor'): 'the image editor drives the edit document; editor and ui are not yet split',
    ('src/ui/SettingsWindow.cpp', 'storage/b2'): 'Settings bounds its link fields with the provider\'s limits',
    ('src/ui/SettingsWindow.cpp', 'storage/proton'): 'Settings bounds its timeout field with the provider\'s limit',
    ('src/ui/SettingsWindow.cpp', 'storage/s3'): 'Settings previews public links with the S3 URL rules',
    ('src/ui/ShortcutKeyboard.cpp', 'platform/wayland'): 'the shortcut recorder shares the compositor keymap reader',
    ('src/ui/TrayController.cpp', 'app'): 'the tray runs in the resident and calls its services directly',
    ('src/ui/TrayController.h', 'app'): 'the tray runs in the resident and calls its services directly',
}

Include = namedtuple('Include', 'file line text source target')


def code(text):
    """The text with block comments and raw strings blank, on the same line numbers."""
    def blank(match):
        return match[0] if match[0][0] == '"' or match[0].startswith('//') else ' ' + '\n' * match[0].count('\n')
    return NON_CODE.sub(blank, text)


def module_of(path):
    """The module of a path relative to src/."""
    parts = PurePosixPath(path).parts
    if len(parts) == 1:
        return 'main'
    if parts[0] in SPLIT_MODULES and len(parts) > 2:
        return '/'.join(parts[:2])
    return parts[0]


def resolve(src, including, quoted, name):
    """The src/-relative path an include finds, as the compiler looks: a quoted
    name beside its file first, then under src/. Anything else is not ours."""
    candidates = [os.path.join(os.path.dirname(including), name)] if quoted else []
    for candidate in candidates + [name]:
        relative = os.path.normpath(candidate)
        if not relative.startswith('..') and (src / relative).is_file():
            return relative
    return None


def includes(src):
    """Every include in src/ that crosses a module boundary."""
    found = []
    for path in sorted(p for p in src.rglob('*') if p.suffix in SUFFIXES and p.is_file()):
        relative = path.relative_to(src).as_posix()
        for number, line in enumerate(code(path.read_text(errors='replace')).splitlines(), 1):
            match = INCLUDE.match(line)
            if not match:
                continue
            target = resolve(src, relative, match[1] is not None, match[1] or match[2])
            if target and module_of(target) != module_of(relative):
                found.append(Include('src/' + relative, number, match[0].strip(), module_of(relative),
                                     module_of(target)))
    return found


def cycle(allowed):
    """One cycle in the allowed graph as a list of modules, or None."""
    done, path = set(), []

    def visit(module):
        if module in path:
            return path[path.index(module):] + [module]
        if module in done:
            return None
        path.append(module)
        for target in sorted(allowed.get(module, ())):
            found = visit(target)
            if found:
                return found
        path.pop()
        done.add(module)
        return None

    for module in sorted(allowed):
        found = visit(module)
        if found:
            return found
    return None


def check(src, allowed=None, tolerated=None):
    """Every way src/ and the tables disagree, as printable lines."""
    allowed = ALLOWED if allowed is None else allowed
    tolerated = TOLERATED if tolerated is None else tolerated
    problems, used_edges, used_pins = [], set(), set()
    pinned = defaultdict(list)
    for file, target in sorted(tolerated):
        pinned[module_of(PurePosixPath(file).relative_to('src')), target].append(file)
    for include in includes(src):
        edge = include.source, include.target
        where = f'{include.file}:{include.line}: {include.text}: {include.source} -> {include.target}'
        if include.target in allowed.get(include.source, ()):
            used_edges.add(edge)
        elif (include.file, include.target) in tolerated:
            used_pins.add((include.file, include.target))
        elif edge in pinned:
            problems.append(f'{where} is tolerated only in {", ".join(pinned[edge])}. Do not spread it: '
                            'move the shared code to a lower module and include it from there.')
        else:
            problems.append(f'{where} is not in the layer table. Move the shared code to a lower module, or, '
                            'if this direction is right, add the edge to ALLOWED in tools/ci/check_layers.py.')
    for source, targets in sorted(allowed.items()):
        problems.extend(f'ALLOWED {source} -> {target} is stale: nothing includes it. Remove it from '
                        'tools/ci/check_layers.py.' for target in sorted(targets) if (source, target) not in used_edges)
    for file, target in sorted(tolerated):
        if (file, target) not in used_pins:
            problems.append(f'TOLERATED {file} -> {target} is stale: that file no longer includes it. '
                            'Remove it from tools/ci/check_layers.py.')
    for (source, target), files in sorted(pinned.items()):
        if target in allowed.get(source, ()):
            problems.append(f'{source} -> {target} is in both ALLOWED and TOLERATED ({", ".join(files)}). Keep one.')
    loop = cycle(allowed)
    if loop:
        problems.append(f'ALLOWED has a cycle, {" -> ".join(loop)}. Pin the backwards direction in TOLERATED.')
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args(argv)
    problems = check(args.root / 'src')
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
