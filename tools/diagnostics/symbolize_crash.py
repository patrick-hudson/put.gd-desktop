#!/usr/bin/env python3
"""Symbolize put.gd crash records (#124, docs/diagnostics.md).

Reads crash records from a diagnostics package (.zip), a log file (.jsonl), a
running.crash file or raw helper stderr, finds each module's debug file by GNU
build ID and prints every frame as function and file:line with eu-addr2line
(or addr2line).

Debug files come from --debug-dir (an extracted putgd-<version>-debug-<arch>.tar.zst,
a build tree with <binary>.debug files, or /usr/lib/debug), and then from
debuginfod for distro libraries (Qt, glibc): debuginfod-find when installed, else
the servers in DEBUGINFOD_URLS or /etc/debuginfod/*.urls over HTTPS.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

PREFIX = 'putgd-crash\t'


def parse_lines(text: str) -> list[dict]:
    """The raw putgd-crash lines, one dict per record (same fields as CrashRecord.cpp)."""
    records, record = [], None
    for line in text.splitlines():
        if not line.startswith(PREFIX):
            continue
        parts = line[len(PREFIX):].split('\t')
        kind = parts[0]
        if kind == 'begin' and len(parts) >= 5:
            record = {'process': parts[2], 'pid': int(parts[3]), 'frames': [], 'modules': []}
            records.append(record)
        elif record is None:
            continue
        elif kind == 'signal' and len(parts) >= 5:
            record.update(signal=int(parts[1]), signalName=parts[2], faultAddress=parts[4])
        elif kind == 'version' and len(parts) >= 2:
            record['version'] = parts[1]
        elif kind == 'frame' and len(parts) >= 5:
            frame = {'index': int(parts[1]), 'address': parts[2]}
            if parts[3] != '-':
                frame.update(module=int(parts[3]), offset=parts[4])
            record['frames'].append(frame)
        elif kind == 'module' and len(parts) >= 5:
            record['modules'].append({'index': int(parts[1]), 'loadBias': parts[2],
                                      'buildId': '' if parts[3] == '-' else parts[3], 'file': '\t'.join(parts[4:])})
        elif kind == 'end':
            record = None
    return records


def from_json_lines(text: str) -> list[dict]:
    """Crash records inside log records: helper crash records and unclean exits."""
    records = []
    for line in text.splitlines():
        try:
            value = json.loads(line)
        except ValueError:
            continue
        fields = value.get('fields', {}) if isinstance(value, dict) else {}
        crash = fields.get('crash')
        if isinstance(crash, dict) and crash.get('frames') is not None:
            records.append(crash)
    return records


def load(path: Path) -> list[dict]:
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            records = from_json_lines(archive.read('logs/putgd.jsonl').decode('utf-8', 'replace'))
            diagnostics = json.loads(archive.read('diagnostics.json'))
        # The resident's own records also sit in diagnostics.uncleanExit.
        for exit_record in diagnostics.get('uncleanExit', {}).get('recent', []):
            crash = exit_record.get('crash')
            if isinstance(crash, dict) and crash not in records:
                records.append(crash)
        return records
    text = path.read_text(errors='replace')
    return parse_lines(text) or from_json_lines(text)


def debug_file(build_id: str, module_file: str, directories: list[Path]) -> Path | None:
    if not build_id:
        return None
    for directory in directories:
        candidate = directory / '.build-id' / build_id[:2] / (build_id[2:] + '.debug')
        if candidate.is_file():
            return candidate
        # A build tree: <binary>.debug beside the binary, checked by build ID.
        sibling = directory / (Path(module_file).name + '.debug')
        if sibling.is_file() and read_build_id(sibling) == build_id:
            return sibling
    finder = shutil.which('debuginfod-find')
    if finder:
        result = subprocess.run([finder, 'debuginfo', build_id], capture_output=True, text=True, timeout=300)
        if result.returncode == 0 and result.stdout.strip():
            return Path(result.stdout.strip().splitlines()[-1])
        return None
    return fetch_debuginfo(build_id)


def debuginfod_urls() -> list[str]:
    value = os.environ.get('DEBUGINFOD_URLS', '')
    if not value:
        for path in sorted(Path('/etc/debuginfod').glob('*.urls')):
            value += ' ' + path.read_text()
    return [url.rstrip('/') for url in value.split() if url.startswith('https://')]


def fetch_debuginfo(build_id: str) -> Path | None:
    """Without debuginfod-find: the debuginfod HTTP protocol, GET <server>/buildid/<id>/debuginfo,
    cached under $XDG_CACHE_HOME/putgd-debuginfod."""
    if not re.fullmatch(r'[0-9a-f]{16,64}', build_id):
        return None
    cache = Path(os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache') / 'putgd-debuginfod'
    target = cache / (build_id + '.debug')
    if target.is_file():
        return target
    for url in debuginfod_urls():
        try:
            with urllib.request.urlopen(url + '/buildid/' + build_id + '/debuginfo', timeout=120) as response:
                cache.mkdir(parents=True, exist_ok=True)
                partial = target.with_suffix('.partial')
                with partial.open('wb') as stream:
                    shutil.copyfileobj(response, stream)
                partial.replace(target)
                return target
        except (OSError, ValueError):
            continue
    return None


def read_build_id(path: Path) -> str | None:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'releases'))
    try:
        import debug_symbols
        return debug_symbols.build_id(path)
    except (ValueError, OSError):
        return None


def symbolize(record: dict, directories: list[Path]) -> list[str]:
    tool = shutil.which('eu-addr2line') or shutil.which('addr2line')
    if tool is None:
        raise SystemExit('Install elfutils (eu-addr2line) or binutils (addr2line)')
    modules = {module['index']: module for module in record.get('modules', [])}
    files = {index: debug_file(module.get('buildId', ''), module.get('file', ''), directories) for index, module in modules.items()}
    lines = [f"{record.get('process', '?')} pid {record.get('pid', '?')}: {record.get('signalName', '?')} "
             f"at {record.get('faultAddress', '?')} ({record.get('version', 'unknown version')})"]
    for frame in record.get('frames', []):
        module = modules.get(frame.get('module'))
        if module is None or not frame.get('offset'):
            lines.append(f"#{frame['index']:<3} {frame.get('address', '?')}  ??")
            continue
        offset = int(frame['offset'], 16)
        # Frame 0 is the faulting instruction; the others are return addresses,
        # so look up the call instruction just before them.
        lookup = offset if frame['index'] == 0 else offset - 1
        target = files.get(frame['module'])
        location = '??'
        if target is not None:
            result = subprocess.run([tool, '-f', '-C', '-i', '-e', str(target), hex(lookup)], capture_output=True, text=True)
            output = [line for line in result.stdout.splitlines() if line]
            location = ' / '.join(f'{output[i]} {output[i + 1]}' for i in range(0, len(output) - 1, 2)) or '??'
        name = Path(module['file']).name
        found = '' if target else f"  [no debug file for build ID {module.get('buildId') or 'unknown'}]"
        lines.append(f"#{frame['index']:<3} {name}+{hex(offset)}  {location}{found}")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input', type=Path, help='Diagnostics package, log file, running.crash or helper stderr')
    parser.add_argument('--debug-dir', type=Path, action='append', default=[],
                        help='Extracted debug asset, build tree or /usr/lib/debug (repeatable)')
    args = parser.parse_args()
    records = load(args.input)
    if not records:
        raise SystemExit('No crash records in ' + str(args.input))
    for record in records:
        print('\n'.join(symbolize(record, [*args.debug_dir, Path('/usr/lib/debug')])))
        print()


if __name__ == '__main__':
    sys.exit(main())
