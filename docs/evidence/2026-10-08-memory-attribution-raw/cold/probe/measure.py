#!/usr/bin/env python3
"""External /proc sampler for an owned native Qt locating process only."""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import selectors
import subprocess
import time

COUNTERS = ('Rss', 'Pss', 'Private_Clean', 'Private_Dirty', 'Shared_Clean',
            'Shared_Dirty', 'Anonymous', 'Swap')
STAGES = ['native-empty', 'visual-style', 'brand-icon', 'tray-icon',
          'empty-tray-menu', 'populated-tray-menu', 'after-one-trim']


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def mapping_name(path):
    if not path:
        return '[anonymous]'
    if path.startswith('['):
        return path if path in ('[heap]', '[stack]', '[vdso]', '[vvar]', '[vvar_vclock]') else '[anonymous]'
    if 'memfd:' in path:
        return 'memfd:xorg' if 'memfd:xorg' in path else 'memfd:[other]'
    name = pathlib.PurePosixPath(path.removesuffix(' (deleted)')).name
    if name == 'native-cold-stages':
        return name
    if path.startswith(('/usr/lib/', '/lib/', '/usr/lib64/', '/lib64/')) and '.so' in name:
        return name
    if path.startswith('/usr/share/fonts/') and name.endswith(('.ttf', '.otf', '.ttc')):
        return name
    return '[other-file]'


def footprint(pid):
    regions = {}
    current = None
    for line in pathlib.Path(f'/proc/{pid}/smaps').read_text().splitlines():
        if re.match(r'^[0-9a-f]+-[0-9a-f]+\s', line):
            fields = line.split(None, 5)
            name = mapping_name(fields[5] if len(fields) > 5 else '')
            current = regions.setdefault(name, {'mapping': name, 'mappingCount': 0})
            current['mappingCount'] += 1
        elif current is not None:
            match = re.match(r'^([A-Za-z_]+):\s+(\d+)\s+kB', line)
            if match and match[1] in COUNTERS:
                key = match[1] + 'Bytes'
                current[key] = current.get(key, 0) + int(match[2]) * 1024
    rollup = {}
    for line in pathlib.Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines():
        match = re.match(r'^([A-Za-z_]+):\s+(\d+)\s+kB', line)
        if match and match[1] in COUNTERS:
            rollup[match[1] + 'Bytes'] = int(match[2]) * 1024
    return rollup, sorted(regions.values(), key=lambda x: x.get('RssBytes', 0), reverse=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--settle-ms', type=int, default=5000)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('output already exists; choose a fresh receipt path')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    source = pathlib.Path(__file__).resolve().parent
    binary = args.binary.resolve()
    result = {'schemaVersion': 1, 'scope': 'Native Cinnamon Qt locating stages only; no product services, windows, clipboard, cloud or budget verdict.',
              'timestampUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'binarySha256': digest(binary), 'samplerSha256': digest(__file__),
              'sourceSha256': digest(source / 'main.cpp'), 'cmakeSha256': digest(source / 'CMakeLists.txt'),
              'command': [str(binary), str(args.settle_ms)], 'settleMs': args.settle_ms,
              'samples': [], 'status': 'running',
              'limitations': ['Sequential retained-object stages, not independent paired product workloads.',
                              'One final glibc trim only; heap arena counters do not identify individual live owners.',
                              'Native fonts/theme/dialog support remain enabled; menu entries have no product effects.',
                              'No graphical acceptance, production saving or40MiB achievement claim.']}
    started = time.monotonic()
    child = subprocess.Popen(result['command'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, bufsize=0)
    selector = selectors.DefaultSelector()
    selector.register(child.stdout, selectors.EVENT_READ)
    buffer = b''
    try:
        while len(result['samples']) < len(STAGES):
            events = selector.select((args.settle_ms / 1000) + 15)
            if not events:
                raise RuntimeError('stage_timeout')
            chunk = child.stdout.read(4096)
            if not chunk:
                raise RuntimeError('probe_exited_before_all_stages')
            buffer += chunk
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                sample = json.loads(line)
                if sample['pid'] != child.pid or sample['stage'] != STAGES[len(result['samples'])]:
                    raise RuntimeError('stage_or_process_identity_mismatch')
                if sample['probeCppSha256'] != result['sourceSha256']:
                    raise RuntimeError('compiled_source_digest_mismatch')
                sample['elapsedSeconds'] = time.monotonic() - started
                sample['timestampUtc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
                sample['smapsRollup'], sample['footprint'] = footprint(child.pid)
                names = {x['mapping'] for x in sample['footprint']}
                sample['nativeGtkMapped'] = 'libqgtk3.so' in names
                sample['qtNetworkMapped'] = any(x.startswith('libQt6Network') for x in names)
                result['samples'].append(sample)
                args.output.write_text(json.dumps(result, indent=2) + '\n')
                print(json.dumps({'stage': sample['stage'], 'rssMiB': sample['smapsRollup']['RssBytes'] / 1048576,
                                  'allocatedBytes': sample['allocatedBytes']}), flush=True)
                child.stdin.write(b'n')
                child.stdin.flush()
        result['exitCode'] = child.wait(timeout=5)
        result['status'] = 'complete' if result['exitCode'] == 0 else 'probe_failed'
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        result['status'] = 'failed'
        result['error'] = str(error)
    finally:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=5)
        selector.close()
        result['elapsedSeconds'] = time.monotonic() - started
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    return 0 if result['status'] == 'complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
