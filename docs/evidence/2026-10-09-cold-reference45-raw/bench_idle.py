#!/usr/bin/env python3
"""Measure the actual release resident on physical Cinnamon without acquiring data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def footprint(pid):
    """Attribute resident pages without exporting full mapped file paths."""
    groups = {}
    name = None
    for line in Path(f'/proc/{pid}/smaps').read_text().splitlines():
        fields = line.split()
        if fields and '-' in fields[0] and len(fields) >= 5:
            mapped = fields[5] if len(fields) > 5 else '[anonymous]'
            name = Path(mapped).name if mapped.startswith('/') else mapped
            groups.setdefault(name, {'rssBytes': 0, 'pssBytes': 0, 'privateBytes': 0})
        elif name and fields and fields[0] in ('Rss:', 'Pss:', 'Private_Clean:', 'Private_Dirty:'):
            key = 'rssBytes' if fields[0] == 'Rss:' else 'pssBytes' if fields[0] == 'Pss:' else 'privateBytes'
            groups[name][key] += int(fields[1]) * 1024
    return [{'mapping': name, **values} for name, values in sorted(groups.items(), key=lambda entry: -entry[1]['rssBytes'])]


def run(args):
    if os.environ.get('XDG_SESSION_TYPE') != 'x11' or 'Cinnamon' not in os.environ.get('XDG_CURRENT_DESKTOP', ''):
        raise RuntimeError('This physical baseline requires the actual Cinnamon X11 session')
    virtualization = subprocess.run(['systemd-detect-virt'], text=True, capture_output=True)
    if virtualization.stdout.strip() != 'none':
        raise RuntimeError('VM measurements cannot establish the physical idle budget')
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=False, mode=0o700)
    environment = os.environ.copy()
    for key, directory in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'), ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')]:
        path = root / directory
        path.mkdir(mode=0o700)
        environment[key] = str(path)
    app = str(Path(args.app).resolve())
    cli = str(Path(args.cli).resolve())
    resident = None
    log = (root / 'resident.log').open('wb')
    def request(method, params=None):
        output = subprocess.check_output([cli, method, json.dumps(params or {})], env=environment, timeout=10)
        value = json.loads(output)
        if not value['ok']:
            raise RuntimeError(value['error']['code'])
        return value['result']
    def ready():
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if resident.poll() is not None:
                raise RuntimeError('Resident exited before readiness')
            try:
                if request('diagnostics')['journalReady']:
                    return
            except (subprocess.CalledProcessError, RuntimeError):
                pass
            time.sleep(.2)
        raise RuntimeError('Resident readiness timeout')
    try:
        # The production validator creates a complete isolated configuration.
        environment['QT_QPA_PLATFORM'] = 'offscreen'
        resident = subprocess.Popen([app, '--background'], env=environment, stdout=log, stderr=log)
        ready()
        document = request('settings.developmentTemplate')['document']
        document['profiles'] = []
        document['actions'] = {action: {'destination': 'local', 'profileId': None, 'shortcut': ''}
                               for action in document['actions']}
        document['archive']['root'] = str(root / 'archive')
        document['preferences'] = {'notifications': False, 'launchAtLogin': False}
        metadata = request('settings.get')['metadata']
        request('settings.patch', {'patch': document, 'expectedRevision': metadata['observedRevision']})
        resident.terminate()
        resident.wait(timeout=20)
        environment['QT_QPA_PLATFORM'] = 'xcb'
        resident = subprocess.Popen([app, '--background'], env=environment, stdout=log, stderr=log)
        ready()
        time.sleep(5)
        ticks_per_second = os.sysconf('SC_CLK_TCK')
        page_size = os.sysconf('SC_PAGE_SIZE')
        def sample():
            fields = Path(f'/proc/{resident.pid}/stat').read_text().split(') ', 1)[1].split()
            rss = int(Path(f'/proc/{resident.pid}/statm').read_text().split()[1]) * page_size
            return time.monotonic(), int(fields[11]) + int(fields[12]), rss
        start, initial_ticks, initial_rss = sample()
        result = {'schemaVersion': 1, 'scope': 'physical Cinnamon idle resident; empty history; capture/network not invoked',
                  'appSha256': hashlib.sha256(Path(app).read_bytes()).hexdigest(),
                  'kernel': os.uname().release, 'machine': os.uname().machine,
                  'desktop': os.environ['XDG_CURRENT_DESKTOP'], 'session': 'x11',
                  'cpu': next(line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')),
                  'warmupSeconds': 5, 'samples': [], 'initialRssBytes': initial_rss,
                  'initialFootprint': footprint(resident.pid)}
        last_recorded = -30
        while True:
            if resident.poll() is not None:
                raise RuntimeError('Resident exited during idle sampling')
            now, ticks, rss = sample()
            elapsed = now - start
            if elapsed >= last_recorded + 30 or elapsed >= args.seconds:
                result['samples'].append({'elapsedSeconds': elapsed, 'cpuTicks': ticks - initial_ticks, 'rssBytes': rss})
                last_recorded = elapsed
                write_json(root / 'progress.json', result)
            if elapsed >= args.seconds:
                break
            time.sleep(1)
        result.update(elapsedSeconds=elapsed, idleCpuPercentOneCore=(ticks - initial_ticks) / ticks_per_second / elapsed * 100,
                      finalRssBytes=rss, peakSampledRssBytes=max(s['rssBytes'] for s in result['samples']))
        result['finalFootprint'] = footprint(resident.pid)
        result['budgetPassed'] = elapsed >= 300 and result['idleCpuPercentOneCore'] < .5 and result['peakSampledRssBytes'] <= 100 * 1024 * 1024
        write_json(root / 'results.json', result)
        print(json.dumps({key: result[key] for key in ['elapsedSeconds', 'idleCpuPercentOneCore', 'finalRssBytes', 'peakSampledRssBytes', 'budgetPassed']}), flush=True)
        return 0 if result['budgetPassed'] else 1
    finally:
        if resident is not None and resident.poll() is None:
            resident.terminate()
            resident.wait(timeout=20)
        log.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--seconds', type=int, default=300)
    args = parser.parse_args()
    if args.seconds < 1:
        parser.error('--seconds must be positive; only >=300 establishes the budget')
    raise SystemExit(run(args))
