"""Run frozen measurements serially after the desktop workflow and builds finish."""
from pathlib import Path
import datetime, hashlib, json, os, subprocess, sys

ROOT = Path('/home/patrick/work/put.gd')
OUT = ROOT / 'work/memory-surfaces'
EXPERIMENT = Path('/home/patrick/.codex/worktrees/resident-memory/put.gd/.worktrees/optimize-resident-view-surfaces-exp-001')
sys.path.insert(0, str(ROOT / 'tools/lab'))
import lab

def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

assert not (OUT / 'quiet-run.json').exists()
assert int((ROOT / 'work/versioning/release/exit-code').read_text()) == 0
assert sha(EXPERIMENT / 'build/release/putgd') == '12bc4309e2191bb5d715dc74e4e04d98e0b11a7fbd2f42bde80c5217f014fd09'
build_names = {'ninja', 'cmake', 'cc1plus', 'cc1', 'clang', 'clang++', 'c++', 'g++', 'ld', 'lto1'}
host_builds = []
for process in Path('/proc').iterdir():
    if not process.name.isdigit():
        continue
    try:
        name = (process / 'comm').read_text().strip()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        continue
    if name in build_names:
        host_builds.append({'pid': int(process.name), 'name': name})
assert not host_builds, host_builds
record = {'schemaVersion': 1, 'startedUtc': now(), 'hostBuildProcesses': host_builds,
          'hostLoadAverage': os.getloadavg(), 'steps': [], 'guestBuildChecks': {}}
for name in ('cinnamon', 'gnome', 'kde'):
    run = ROOT / f'work/lab-runs/current-{name}-a3d70dc'
    result = subprocess.run(lab.ssh_args(run) + ['ps', '-C', ','.join(sorted(build_names)), '-o', 'pid=,comm='],
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == 1 and not result.stdout.strip(), (name, result.returncode, result.stdout)
    record['guestBuildChecks'][name] = {'exitCode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
lab.write_json(OUT / 'quiet-run.json', record)

commands = [('native-wave', [sys.executable, str(OUT / 'sample-wave.py')])]
for number in (1, 2):
    commands.append((f'physical-{number}', [sys.executable, str(ROOT / 'work/memory-next/measure-views.py'),
        '--app', str(EXPERIMENT / 'build/release/putgd'), '--cli', str(EXPERIMENT / 'build/release/putgd-cli'),
        '--output', str(OUT / f'physical-{number}')]))
commands.append(('cold-300s', [sys.executable, str(ROOT / 'tests/performance/bench_idle.py'),
    '--app', str(EXPERIMENT / 'build/release/putgd'), '--cli', str(EXPERIMENT / 'build/release/putgd-cli'),
    '--output', str(OUT / 'cold-300s'), '--seconds', '300']))
for label, command in commands:
    step = {'label': label, 'command': command, 'startedUtc': now()}
    record['steps'].append(step)
    lab.write_json(OUT / 'quiet-run.json', record)
    print(json.dumps({'stage': label, 'startedUtc': step['startedUtc']}), flush=True)
    with (OUT / f'{label}-launch.stdout').open('w') as out, (OUT / f'{label}-launch.stderr').open('w') as err:
        result = subprocess.run(command, stdout=out, stderr=err)
    step.update(exitCode=result.returncode, finishedUtc=now())
    lab.write_json(OUT / 'quiet-run.json', record)
    print(json.dumps({'stage': label, 'exitCode': result.returncode}), flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
record['finishedUtc'] = now()
lab.write_json(OUT / 'quiet-run.json', record)
